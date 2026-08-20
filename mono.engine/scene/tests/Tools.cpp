// A thing a character can hold.
//
// **The case that decides whether this was worth building is the second one.**
// `docs/retired/DEFERRED.md` D00120 refused a `Tool` class on the grounds that one with
// no equip behaviour would appear in the insert palette, save into a game file
// and then do nothing for ever - so what has to be proved is not that the class
// registers, but that putting a tool in a hand *moves the handle*, in the
// formation the rig already keeps.
//
// The first case is the one a regression would be silent in: a `Backpack` is
// private to its player on the wire because of where it sits in the tree, and
// equipping is a reparent out of it. If that stopped being true the tool would
// still work and one client would be reading another's inventory.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tools.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_SUITE_ID("engine.scene.tools")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AddPlayer;
using engine::scene::BACKPACK_NAME;
using engine::scene::Bounds;
using engine::scene::Character;
using engine::scene::CharacterLimb;
using engine::scene::CharacterOf;
using engine::scene::EquippedTool;
using engine::scene::EquipTool;
using engine::scene::InstallServices;
using engine::scene::LoadCharacter;
using engine::scene::MakePart;
using engine::scene::Motion;
using engine::scene::PartDesc;
using engine::scene::PlayerOwning;
using engine::scene::PoseCharacters;
using engine::scene::ReclaimOrphanedCharacters;
using engine::scene::RegisterSceneClasses;
using engine::scene::RIGHT_ARM_NAME;
using engine::scene::STARTER_GEAR_NAME;
using engine::scene::TakeDamage;
using engine::scene::Tool;
using engine::scene::ToolClass;
using engine::scene::ToolGrip;
using engine::scene::ToolHandle;
using engine::scene::Transform;
using engine::scene::UnequipTool;
using engine::scene::UpdateToolGrips;
using engine::scene::WorkspaceOf;

namespace {
	// A furnished world with nobody in it. `Join` is separate so a case can put
	// something in `StarterPack` first, which is the only way gear reaches a
	// player's `StarterGear`.
	struct World {
		Store Store_{"tools-test"};

		World() {
			RegisterSceneClasses();
			InstallServices(Store_);
		}

		Entity Join(std::string_view name = "Tester") {
			const Entity player = AddPlayer(Store_, name);
			REQUIRE(player != NULL_ENTITY);
			REQUIRE(LoadCharacter(Store_, player) != NULL_ENTITY);
			return player;
		}

		Entity Backpack(Entity player) {
			return Store_.FindFirstChild(player, BACKPACK_NAME);
		}
	};

	// A `Tool` with the one child that makes it holdable.
	//
	// Built through `Instance.new` and `MakePart` rather than by assembling
	// components, because those are the only two constructors this module has -
	// `scene/AGENTS.md` refuses a second one, and a tool built by hand here
	// would be a tool nothing else in the engine could produce.
	Entity Gear(Store &store, std::string_view name) {
		const Entity tool = store.CreateInstance(ToolClass(), name);
		REQUIRE(tool != NULL_ENTITY);

		PartDesc desc;
		desc.Size = Vector3{0.5f, 0.5f, 2.0f};
		desc.Anchored = false;

		const Entity handle = MakePart(store, desc);
		REQUIRE(handle != NULL_ENTITY);
		store.SetInstanceName(handle, engine::scene::TOOL_HANDLE_NAME);
		store.SetParent(handle, tool);
		return tool;
	}

	// Where a character's root actually is, which is what a grip is measured
	// against.
	CFrame RootOf(const Store &store, Entity character) {
		const Character *rig = store.Get<Character>(character);
		REQUIRE(rig != nullptr);
		return store.Get<Transform>(rig->Root)->Frame;
	}
}

TEST_CASE("a stowed tool is its owner's and a held one is everybody's", "[scene][tools]") {
	// **The wire rule, asserted where the rule lives.** `mono.server` installs
	// `PlayerOwning` as its interest predicate - a row under a `Player` reaches
	// that client and nobody else - and equipping moves the tool out from under
	// the player and into a model in `Workspace`. So the privacy of a backpack
	// and the publicity of a held tool are the same one statement, and this is
	// it. `server.replication` asserts the other end over a real socket.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, world.Backpack(player));

	CHECK(PlayerOwning(world.Store_, tool) == player);
	CHECK(PlayerOwning(world.Store_, ToolHandle(world.Store_, tool)) == player);

	REQUIRE(EquipTool(world.Store_, character, tool));

	// Under the character, which is under `Workspace`, which is under no player
	// at all.
	CHECK(world.Store_.ParentOf(tool) == character);
	CHECK(PlayerOwning(world.Store_, tool) == NULL_ENTITY);
	CHECK(PlayerOwning(world.Store_, ToolHandle(world.Store_, tool)) == NULL_ENTITY);

	// And back, which has to restore the privacy rather than merely the parent.
	REQUIRE(UnequipTool(world.Store_, tool));
	CHECK(world.Store_.ParentOf(tool) == world.Backpack(player));
	CHECK(PlayerOwning(world.Store_, tool) == player);
}

TEST_CASE("equipping hangs the handle off the hand and the pose places it", "[scene][tools]") {
	// **The case D00120 was actually held open on.** The entry said a tool that
	// follows a hand is the same missing piece as a rig that does not fall apart
	// on a slope - so the assertion is not that a row appeared, it is that the
	// handle ends up where the arm is after the same pass that places the arm.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, world.Backpack(player));

	const Entity handle = ToolHandle(world.Store_, tool);
	CHECK(world.Store_.Get<CharacterLimb>(handle) == nullptr);

	REQUIRE(EquipTool(world.Store_, character, tool));

	const Character *rig = world.Store_.Get<Character>(character);
	const CharacterLimb *worn = world.Store_.Get<CharacterLimb>(handle);
	REQUIRE(worn != nullptr);
	CHECK(worn->Root == rig->Root);

	// The grip is read off the rig rather than written down, so this is the
	// bottom face of the right arm and moves if the arm does.
	const CharacterLimb *arm =
		world.Store_.Get<CharacterLimb>(world.Store_.FindFirstChild(character, RIGHT_ARM_NAME));
	REQUIRE(arm != nullptr);
	const float reach =
		world.Store_.Get<Bounds>(world.Store_.FindFirstChild(character, RIGHT_ARM_NAME))->HalfExtent.Y;
	CHECK(worn->Offset.Position.X == Approx(arm->Offset.Position.X));
	CHECK(worn->Offset.Position.Y == Approx(arm->Offset.Position.Y - reach));

	// **Carried rather than simulated**, which is what taking the `Motion` away
	// says - the same archetype move `physics` makes for a sleeping body.
	CHECK(world.Store_.Get<Motion>(handle) == nullptr);

	// And the pass that places limbs places this one. Moving the root is what
	// makes the assertion about following rather than about a single frame.
	Transform moved;
	moved.Frame = CFrame(Vector3{20.0f, 3.0f, -7.0f});
	world.Store_.Set(rig->Root, moved);
	(void)PoseCharacters(world.Store_);

	const CFrame expected = RootOf(world.Store_, character) * worn->Offset;
	const Vector3 landed = world.Store_.Get<Transform>(handle)->Frame.Position;
	CHECK(landed.X == Approx(expected.Position.X));
	CHECK(landed.Y == Approx(expected.Position.Y));
	CHECK(landed.Z == Approx(expected.Position.Z));
}

TEST_CASE("Tool.Grip moves the handle and nothing else", "[scene][tools]") {
	// The one field this class has, and the bar `D00119` set for a class having
	// any: a property with no reader is what the whole entry refused.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, world.Backpack(player));
	world.Store_.Set(tool, Tool{CFrame(Vector3{0.0f, 0.0f, -1.5f})});

	REQUIRE(EquipTool(world.Store_, character, tool));

	const CharacterLimb *worn = world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, tool));
	REQUIRE(worn != nullptr);

	const CFrame grip = ToolGrip(world.Store_, character);
	CHECK(worn->Offset.Position.Z == Approx(grip.Position.Z - 1.5f));

	// Changed after the fact, which is the case that proves `UpdateToolGrips`
	// re-derives rather than remembering what it wrote once.
	world.Store_.Set(tool, Tool{CFrame(Vector3{0.0f, 0.0f, 4.0f})});
	CHECK(UpdateToolGrips(world.Store_) == 1);

	worn = world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, tool));
	CHECK(worn->Offset.Position.Z == Approx(grip.Position.Z + 4.0f));

	// **And a settled world writes nothing**, which is what keeps a held tool
	// off the wire every tick: the row is compared before it is written.
	CHECK(UpdateToolGrips(world.Store_) == 0);
}

TEST_CASE("unequipping puts the tool back and gives the handle its motion back", "[scene][tools]") {
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, world.Backpack(player));
	const Entity handle = ToolHandle(world.Store_, tool);

	REQUIRE(EquipTool(world.Store_, character, tool));
	REQUIRE(UnequipTool(world.Store_, tool));

	CHECK(world.Store_.ParentOf(tool) == world.Backpack(player));
	CHECK(world.Store_.Get<CharacterLimb>(handle) == nullptr);

	// **The `Backpack` and never the `StarterGear`.** Putting it in the
	// persistent list would make picking a thing up a permanent grant.
	CHECK(
		world.Store_.FindFirstChild(world.Store_.FindFirstChild(player, STARTER_GEAR_NAME), "Sword") ==
		NULL_ENTITY
	);

	// The handle is dynamic again, because it carries a body and was only ever
	// stopped from being integrated.
	CHECK(world.Store_.Get<Motion>(handle) != nullptr);
}

TEST_CASE("an anchored handle is not unanchored by being carried", "[scene][tools]") {
	// **The half that would be silent.** Equipping takes a `Motion` away and
	// unequipping hands one back, and a version that handed one back
	// unconditionally would quietly unanchor every anchored handle in the world
	// - `Anchored` *is* the absence of that pair.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = world.Store_.CreateInstance(ToolClass(), "Torch");
	PartDesc desc;
	desc.Size = Vector3{0.5f, 0.5f, 2.0f};
	desc.Anchored = true;

	const Entity handle = MakePart(world.Store_, desc);
	world.Store_.SetInstanceName(handle, engine::scene::TOOL_HANDLE_NAME);
	world.Store_.SetParent(handle, tool);
	world.Store_.SetParent(tool, world.Backpack(player));

	REQUIRE(EquipTool(world.Store_, character, tool));
	REQUIRE(UnequipTool(world.Store_, tool));

	CHECK(world.Store_.Get<Motion>(handle) == nullptr);
	CHECK(world.Store_.Has<engine::scene::Anchored>(handle));
}

TEST_CASE("one hand holds one tool", "[scene][tools]") {
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity sword = Gear(world.Store_, "Sword");
	const Entity torch = Gear(world.Store_, "Torch");
	world.Store_.SetParent(sword, world.Backpack(player));
	world.Store_.SetParent(torch, world.Backpack(player));

	REQUIRE(EquipTool(world.Store_, character, sword));
	REQUIRE(EquipTool(world.Store_, character, torch));

	CHECK(EquippedTool(world.Store_, character) == torch);
	CHECK(world.Store_.ParentOf(sword) == world.Backpack(player));
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, sword)) == nullptr);
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, torch)) != nullptr);
}

TEST_CASE("a script equips by assigning Parent and the pass picks it up", "[scene][tools]") {
	// **The whole script surface, and it is deliberately not a method.** Class
	// tables here carry properties and no methods - `Sound.Playing` is a
	// property for exactly that reason - so `Humanoid:EquipTool` cannot exist
	// and does not need to: a Roblox script equips by writing `tool.Parent`,
	// which is a declared property on `Instance`, and `UpdateToolGrips` is what
	// makes the world agree with the tree afterwards.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, world.Backpack(player));

	REQUIRE(world.Store_.SetProperty(tool, Name("Parent"), &character, sizeof(Entity)));
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, tool)) == nullptr);

	// **Through `PoseCharacters` rather than through `UpdateToolGrips`**, which
	// is the half that proves the wiring: every host that draws a character
	// already runs the pose, and a reconciliation nothing called would leave a
	// script's equip working only where somebody remembered to ask.
	(void)PoseCharacters(world.Store_);
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, tool)) != nullptr);

	// And the same in reverse, so a script can put it away too.
	const Entity backpack = world.Backpack(player);
	REQUIRE(world.Store_.SetProperty(tool, Name("Parent"), &backpack, sizeof(Entity)));
	CHECK(UpdateToolGrips(world.Store_) == 1);
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, tool)) == nullptr);
}

TEST_CASE("a replica cannot move a tool between containers", "[scene][tools]") {
	// **A client that can reparent its own tool can duplicate it**, because the
	// write survives exactly until the next delta contradicts it - which
	// presents as an inventory that works sometimes rather than as a refusal.
	//
	// Three doors and one rule: `ecs::Store::SetProperty` has refused every
	// property write in a replica since v0.3, which is where this engine answers
	// "who owns a row", and `EquipTool`/`UnequipTool` make the same refusal for
	// the C++ side - `scene::TakeDamage`'s pair, one class along.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity sword = Gear(world.Store_, "Sword");
	const Entity torch = Gear(world.Store_, "Torch");
	world.Store_.SetParent(sword, world.Backpack(player));
	world.Store_.SetParent(torch, world.Backpack(player));
	REQUIRE(EquipTool(world.Store_, character, torch));

	world.Store_.SetAdoptOnly(true);

	CHECK_FALSE(EquipTool(world.Store_, character, sword));
	CHECK_FALSE(UnequipTool(world.Store_, torch));
	CHECK_FALSE(world.Store_.SetProperty(sword, Name("Parent"), &character, sizeof(Entity)));

	// Nothing moved, which is the assertion the three refusals are only evidence
	// for.
	CHECK(world.Store_.ParentOf(sword) == world.Backpack(player));
	CHECK(world.Store_.ParentOf(torch) == character);

	// **And the derived half still runs**, because it decides nothing: a replica
	// has to be able to pose a handle whose tool arrived over the wire, and
	// `UpdateToolGrips` reparents nothing at all.
	world.Store_.Remove<CharacterLimb>(ToolHandle(world.Store_, torch));
	CHECK(UpdateToolGrips(world.Store_) == 1);
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, torch)) != nullptr);

	world.Store_.SetAdoptOnly(false);
}

TEST_CASE("a held tool survives a save round trip still held", "[scene][tools]") {
	// Both halves have to cross: the `scene.Tool` row, which is the `Grip`, and
	// the tree, which is the equip. A file that restored one without the other
	// would open with a tool in a hand and no grip, or a grip and no hand.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, world.Backpack(player));
	world.Store_.Set(tool, Tool{CFrame(Vector3{0.0f, 0.25f, -1.5f})});
	REQUIRE(EquipTool(world.Store_, character, tool));

	ByteWriter writer;
	REQUIRE(world.Store_.Save(writer));

	Store restored("tools-test.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const Tool *back = restored.Get<Tool>(tool);
	REQUIRE(back != nullptr);
	CHECK(back->Grip.Position.Z == Approx(-1.5f));
	CHECK(restored.IsA(tool, ToolClass()));
	CHECK(restored.ParentOf(tool) == character);

	// The derived half is derived again rather than trusted, and it lands on the
	// same answer - which is what makes it safe for a replica to run.
	const Entity handle = restored.FindFirstChild(tool, engine::scene::TOOL_HANDLE_NAME);
	const CharacterLimb *worn = restored.Get<CharacterLimb>(handle);
	REQUIRE(worn != nullptr);
	CHECK(UpdateToolGrips(restored) == 0);
}

TEST_CASE("dying keeps the tool and the respawn refills from StarterGear", "[scene][tools]") {
	// **The lifetime question, answered by going through the machinery rather
	// than beside it.** A corpse stays where it fell - `Humanoid.Health` at zero
	// is what death is since v0.15 - so it keeps holding what it was holding,
	// and `LoadCharacter` destroys that body and refills `Backpack` from
	// `StarterGear` on the way to the next one. That is the two-container rule
	// `Services.hpp` already states, unchanged: only `StarterGear` survives a
	// death, equipped or stowed.
	World world;

	const Entity template_ = Gear(world.Store_, "Sword");
	world.Store_.SetParent(
		template_, engine::scene::ServiceOf(world.Store_, engine::ecs::Classes::Find(Name("StarterPack")))
	);

	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity granted = world.Store_.FindFirstChild(world.Backpack(player), "Sword");
	REQUIRE(granted != NULL_ENTITY);
	REQUIRE(EquipTool(world.Store_, character, granted));

	const Character *rig = world.Store_.Get<Character>(character);
	REQUIRE(TakeDamage(world.Store_, rig->Humanoid, 1000.0f));

	// The corpse still holds it, which is the visible half of leaving the body
	// where it fell.
	CHECK(world.Store_.Alive(granted));
	CHECK(world.Store_.ParentOf(granted) == character);

	REQUIRE(LoadCharacter(world.Store_, player) != NULL_ENTITY);

	// The equipped copy went with the body it was in, and a fresh one came out
	// of `StarterGear`.
	CHECK_FALSE(world.Store_.Alive(granted));
	const Entity replaced = world.Store_.FindFirstChild(world.Backpack(player), "Sword");
	CHECK(replaced != NULL_ENTITY);
	CHECK(replaced != granted);
	CHECK(world.Store_.FindFirstChild(replaced, engine::scene::TOOL_HANDLE_NAME) != NULL_ENTITY);
}

TEST_CASE("a player who leaves takes both their tools with them", "[scene][tools]") {
	// The stowed one goes because a `Backpack` is a child of the `Player`; the
	// held one goes because `ReclaimOrphanedCharacters` destroys a model whose
	// owner is gone, and the tool is inside that model. Neither needed a rule of
	// its own, and this is what says so.
	World world;
	const Entity player = world.Join();
	const Entity character = CharacterOf(world.Store_, player);

	const Entity stowed = Gear(world.Store_, "Sword");
	const Entity held = Gear(world.Store_, "Torch");
	world.Store_.SetParent(stowed, world.Backpack(player));
	world.Store_.SetParent(held, world.Backpack(player));
	REQUIRE(EquipTool(world.Store_, character, held));

	world.Store_.DestroyInstance(player);
	CHECK(ReclaimOrphanedCharacters(world.Store_) == 1);

	CHECK_FALSE(world.Store_.Alive(stowed));
	CHECK_FALSE(world.Store_.Alive(held));
	CHECK(WorkspaceOf(world.Store_) != NULL_ENTITY);
}

TEST_CASE("a tool an NPC is holding has nowhere to be put away", "[scene][tools]") {
	// **Refused rather than dropped**, because `Character::Owner` is null for
	// anything that is not a person at a keyboard and there is no backpack to
	// use. Inventing one - parenting it into `Workspace` at the character's feet
	// - would be this module deciding what dropping a tool looks like, which is a
	// game's rule.
	World world;

	engine::scene::CharacterDesc desc;
	desc.Name = "Guard";
	const Entity npc = engine::scene::MakeCharacter(world.Store_, desc);
	REQUIRE(npc != NULL_ENTITY);

	const Entity tool = Gear(world.Store_, "Sword");
	world.Store_.SetParent(tool, WorkspaceOf(world.Store_));

	REQUIRE(EquipTool(world.Store_, npc, tool));
	CHECK(world.Store_.Get<CharacterLimb>(ToolHandle(world.Store_, tool)) != nullptr);

	CHECK_FALSE(UnequipTool(world.Store_, tool));
	CHECK(world.Store_.ParentOf(tool) == npc);

	// **And the swap is refused rather than leaving two in one hand**, which is
	// the same fact reaching `EquipTool`: it empties the hand first, and a hand
	// it cannot empty is one it must not fill again.
	const Entity second = Gear(world.Store_, "Torch");
	world.Store_.SetParent(second, WorkspaceOf(world.Store_));

	CHECK_FALSE(EquipTool(world.Store_, npc, second));
	CHECK(EquippedTool(world.Store_, npc) == tool);
	CHECK(world.Store_.ParentOf(second) == WorkspaceOf(world.Store_));
}
