// The blocky character: what it is made of, and who is allowed to drive it.
//
// **The rig's numbers have to add up or nothing else works**, which is the
// first case: `GroundCharacters` finds the feet by taking half of
// `Humanoid::Height`, so a rig whose parts sum to something else casts its
// ground ray from inside its own shins and can never jump. The height, the
// limb layout and the feet-not-centre convention are one arithmetic fact
// checked from three directions.
//
// The rest is the multiplayer case that has no other check: two players in one
// world, and a keyboard that must reach exactly one of them.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.scene.characters")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AddPlayer;
using engine::scene::Bounds;
using engine::scene::Character;
using engine::scene::CHARACTER_HEIGHT;
using engine::scene::CharacterDesc;
using engine::scene::CharacterLimb;
using engine::scene::CharacterOf;
using engine::scene::FindSpawn;
using engine::scene::Humanoid;
using engine::scene::InputState;
using engine::scene::KeyCode;
using engine::scene::LinkPlayerCharacters;
using engine::scene::LoadCharacter;
using engine::scene::LocalPlayer;
using engine::scene::MakeCharacter;
using engine::scene::MakePart;
using engine::scene::Motion;
using engine::scene::NetworkOwnerOf;
using engine::scene::PartDesc;
using engine::scene::PlayerCharacter;
using engine::scene::PoseCharacters;
using engine::scene::RemoveCharacter;
using engine::scene::SetPlayerCharacter;
using engine::scene::TakeDamage;
using engine::scene::Transform;
using engine::scene::UpdateCharacterControl;

namespace {
	// A furnished world: the classes registered, the services installed, and
	// the two control resources a client would have left behind.
	struct World {
		Store Store_{"characters-test"};

		World() {
			engine::scene::RegisterSceneClasses();
			engine::scene::InstallServices(Store_);
			Store_.SetResource(InputState{});
			Store_.SetResource(engine::scene::CameraController{});
		}

		InputState &Input() {
			return *Store_.ResourceMutable<InputState>();
		}
	};

	// The character's parts, by the names Roblox uses. A rig that renamed one
	// would break every script written against it, so the names are part of
	// the contract rather than an implementation detail.
	Entity Limb(Store &store, Entity model, std::string_view name) {
		return store.FindFirstChild(model, name);
	}

	// How many children a container holds. `Store::HasChildren` answers whether
	// there are any and nothing in the engine needs the number, so the walk is
	// here rather than in a public header.
	size_t Children(const Store &store, Entity container) {
		size_t found = 0;
		store.EachChild(container, [&](Entity) { found++; });
		return found;
	}
}

TEST_CASE("a character's parts add up to its stated height", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	CharacterDesc desc;
	desc.Frame = CFrame(Vector3{10.0f, 0.0f, -4.0f});

	const Entity model = MakeCharacter(store, desc);
	REQUIRE(model != NULL_ENTITY);

	const Character *character = store.Get<Character>(model);
	REQUIRE(character != nullptr);

	// **The frame is the feet and the root is the centre.** An author places a
	// character on a floor; the half-height conversion happens once, in
	// `MakeCharacter`, and this is what pins it there.
	const Transform *root = store.Get<Transform>(character->Root);
	REQUIRE(root != nullptr);
	CHECK(root->Frame.Position.Y == Approx(CHARACTER_HEIGHT * 0.5f));
	CHECK(root->Frame.Position.X == Approx(10.0f));

	// The collider is the whole capsule, which is what makes the character one
	// body rather than six.
	const Bounds *bounds = store.Get<Bounds>(character->Root);
	REQUIRE(bounds != nullptr);
	CHECK(bounds->HalfExtent.Y == Approx(CHARACTER_HEIGHT * 0.5f));

	// Crown to sole, from the limbs themselves. **This is the check that keeps
	// the rig honest**: `physics::GroundCharacters` finds the feet by taking
	// half of `Humanoid::Height` from the root, so the parts have to reach
	// exactly that far or the ground ray starts in the wrong place.
	const Entity head = Limb(store, model, "Head");
	const Entity leg = Limb(store, model, "Left Leg");
	REQUIRE(head != NULL_ENTITY);
	REQUIRE(leg != NULL_ENTITY);

	const float crown = store.Get<Transform>(head)->Frame.Position.Y + store.Get<Bounds>(head)->HalfExtent.Y;
	const float sole = store.Get<Transform>(leg)->Frame.Position.Y - store.Get<Bounds>(leg)->HalfExtent.Y;
	CHECK(crown - sole == Approx(CHARACTER_HEIGHT));
	CHECK(sole == Approx(0.0f));

	// The humanoid steers a part it does not sit on, which is the whole reason
	// `Humanoid::RootPart` exists.
	const Humanoid *humanoid = store.Get<Humanoid>(character->Humanoid);
	REQUIRE(humanoid != nullptr);
	CHECK(humanoid->RootPart == character->Root);
	CHECK(humanoid->Height == Approx(CHARACTER_HEIGHT));

	// One dynamic body and five passengers. A limb with a `Motion` would be
	// integrated and fight the pose pass every tick.
	CHECK(store.Get<Motion>(character->Root) != nullptr);
	CHECK(store.Get<Motion>(leg) == nullptr);
}

TEST_CASE("limbs follow the root and nothing else does", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	const Entity model = MakeCharacter(store, CharacterDesc{});
	const Character *character = store.Get<Character>(model);
	REQUIRE(character != nullptr);

	const Entity head = Limb(store, model, "Head");
	const CFrame rest = store.Get<CharacterLimb>(head)->Offset;

	// Move the body, as the solver would.
	store.Set(character->Root, Transform{CFrame(Vector3{100.0f, 20.0f, -3.0f})});

	// Six limbs and the model itself - the model carries a limb row with an
	// identity offset so that its own placement does not stay behind.
	CHECK(PoseCharacters(store) == 7);

	const Vector3 placed = store.Get<Transform>(head)->Frame.Position;
	CHECK(placed.X == Approx(100.0f + rest.Position.X));
	CHECK(placed.Y == Approx(20.0f + rest.Position.Y));
	CHECK(store.Get<Transform>(model)->Frame.Position.Y == Approx(20.0f));

	// A root that has gone leaves its limbs where they fell rather than
	// collapsing them onto the origin - a pile where it died beats a pile at
	// the world centre.
	store.DestroyInstance(character->Root);
	CHECK(PoseCharacters(store) == 0);
	CHECK(store.Get<Transform>(head)->Frame.Position.X == Approx(100.0f + rest.Position.X));
}

TEST_CASE("a spawn part puts a character on top of itself", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	// No spawn is the origin, which is the honest answer for a world nobody
	// has furnished.
	CHECK(FindSpawn(store).Position.Y == Approx(0.0f));

	PartDesc pad;
	pad.Frame = CFrame(Vector3{5.0f, 8.0f, 5.0f});
	pad.Size = Vector3{12.0f, 2.0f, 12.0f};
	pad.Simulated = false;

	const Entity spawn = MakePart(store, pad);
	store.SetInstanceName(spawn, "SpawnLocation");
	store.SetParent(spawn, engine::scene::WorkspaceOf(store));

	// The *top face*, so the character stands on the pad rather than inside it.
	const CFrame where = FindSpawn(store);
	CHECK(where.Position.Y == Approx(9.0f));
	CHECK(where.Position.X == Approx(5.0f));
}

TEST_CASE("a keyboard drives one character and never somebody else's", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	const Entity mine = AddPlayer(store, "Player1", true);
	const Entity theirs = AddPlayer(store, "Player2");
	REQUIRE(mine != NULL_ENTITY);
	REQUIRE(theirs != NULL_ENTITY);

	const Entity myModel = LoadCharacter(store, mine);
	const Entity theirModel = LoadCharacter(store, theirs);
	REQUIRE(myModel != NULL_ENTITY);
	REQUIRE(theirModel != NULL_ENTITY);

	// Named after the player, which is what makes `workspace:FindFirstChild(
	// player.Name)` the lookup a game script already knows how to write.
	CHECK(store.InstanceNameOf(myModel) == engine::core::Name("Player1"));

	// Each body belongs to its own player, so a client may move one and only
	// one.
	CHECK(NetworkOwnerOf(store, store.Get<Character>(myModel)->Root) == mine);
	CHECK(NetworkOwnerOf(store, store.Get<Character>(theirModel)->Root) == theirs);

	// **The camera is aimed by a rule and not by the spawn**, so it follows the
	// local player's body and is not stolen by the second character to load.
	CHECK(engine::scene::FollowOwnCharacter(store));
	CHECK(store.Resource<engine::scene::CameraController>()->Subject == store.Get<Character>(myModel)->Root);
	CHECK(!engine::scene::FollowOwnCharacter(store));

	world.Input().Down.Set(KeyCode::W, true);

	// **One, and this is the case the whole scoping rule exists for.** Two
	// clients in one world with an unscoped `Each` means each machine walks
	// both characters, and the two fight over one body every tick.
	CHECK(UpdateCharacterControl(store) == 1);
	CHECK(store.Get<Humanoid>(store.Get<Character>(myModel)->Humanoid)->MoveDirection.Magnitude() > 0.5f);
	CHECK(
		store.Get<Humanoid>(store.Get<Character>(theirModel)->Humanoid)->MoveDirection.Magnitude() ==
		Approx(0.0f)
	);

	// Between a death and a respawn there is no body, and keys pressed into
	// that gap must not reach anybody else's.
	CHECK(RemoveCharacter(store, mine));
	CHECK(CharacterOf(store, mine) == NULL_ENTITY);
	CHECK(UpdateCharacterControl(store) == 0);
}

TEST_CASE("loading a character twice leaves one", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Player1");
	const Entity first = LoadCharacter(store, player);
	const Entity second = LoadCharacter(store, player);

	REQUIRE(second != NULL_ENTITY);
	CHECK(first != second);

	// **The old one is destroyed rather than orphaned.** A player holding two
	// characters is two things a camera could follow and two humanoids the
	// controls would drive at once.
	CHECK(!store.Alive(first));
	CHECK(CharacterOf(store, player) == second);

	// Something that is not a player has no character to load.
	CHECK(LoadCharacter(store, engine::scene::WorkspaceOf(store)) == NULL_ENTITY);
}

TEST_CASE("Player.Character is the one hook a game needs", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Player1", true);
	REQUIRE(player != NULL_ENTITY);

	// **A rig a script built, not one `MakeCharacter` built**, which is the
	// whole point: a model with a humanoid and a root in it is a character,
	// however it got there.
	const Entity model = store.CreateInstance(engine::scene::ModelClass(), "Bob");
	store.SetParent(model, engine::scene::WorkspaceOf(store));
	store.Set(model, Transform{CFrame(Vector3{0.0f, 3.0f, 0.0f})});

	PartDesc rootDesc;
	rootDesc.Frame = CFrame(Vector3{0.0f, 3.0f, 0.0f});
	rootDesc.Size = Vector3{2.0f, CHARACTER_HEIGHT, 1.0f};
	rootDesc.Simulated = true;
	const Entity root = MakePart(store, rootDesc);
	store.SetInstanceName(root, "HumanoidRootPart");
	store.SetParent(root, model);

	const Entity humanoid = store.CreateInstance(engine::scene::HumanoidClass(), "Humanoid");
	store.SetParent(humanoid, model);
	store.Set(humanoid, Humanoid{});

	// Nothing is linked yet, so the two gates every mover reads are shut.
	CHECK(CharacterOf(store, player) == NULL_ENTITY);
	CHECK(UpdateCharacterControl(store) == 0);

	// **The assignment, and it resolves the rig by itself.** `Humanoid::
	// RootPart` was never set by the script; without it `StepCharacters` would
	// look for a `Motion` on the humanoid instance, which has no place in the
	// world at all.
	REQUIRE(SetPlayerCharacter(store, player, model));
	CHECK(CharacterOf(store, player) == model);
	CHECK(store.Get<Character>(model)->Root == root);
	CHECK(store.Get<Character>(model)->Humanoid == humanoid);
	CHECK(store.Get<Character>(model)->Owner == player);
	CHECK(store.Get<Humanoid>(humanoid)->RootPart == root);

	// Ownership of the body follows the assignment, which is what lets the
	// client that owns it move it and no one else.
	CHECK(NetworkOwnerOf(store, root) == player);

	// The camera and the keyboard both find it now, and neither was told
	// anything beyond the assignment above.
	CHECK(engine::scene::FollowOwnCharacter(store));
	CHECK(store.Resource<engine::scene::CameraController>()->Subject == root);

	world.Input().Down.Set(KeyCode::W, true);
	CHECK(UpdateCharacterControl(store) == 1);
	CHECK(store.Get<Humanoid>(humanoid)->MoveDirection.Magnitude() > 0.5f);

	// **Released mid-stride, and the body stops.** Without the clear the
	// humanoid keeps its last direction for ever: the pass that would zero it
	// only writes the body its owner is allowed to drive, and there is no
	// owner any more.
	REQUIRE(SetPlayerCharacter(store, player, NULL_ENTITY));
	CHECK(CharacterOf(store, player) == NULL_ENTITY);
	CHECK(store.Get<Humanoid>(humanoid)->MoveDirection.Magnitude() == Approx(0.0f));
	CHECK(store.Get<Character>(model)->Owner == NULL_ENTITY);
	CHECK(UpdateCharacterControl(store) == 0);

	// The camera lets go of a body nobody drives rather than staying on it.
	CHECK(engine::scene::FollowOwnCharacter(store));
	CHECK(store.Resource<engine::scene::CameraController>()->Subject == NULL_ENTITY);

	// **A model with no humanoid is refused where the mistake is made.** The
	// alternative is a player pointing at furniture and a W key that silently
	// does nothing, which is the bug this whole path exists to make loud.
	const Entity crate = MakePart(store, PartDesc{});
	CHECK(!SetPlayerCharacter(store, player, crate));
	CHECK(CharacterOf(store, player) == NULL_ENTITY);
}

TEST_CASE("a replicated assignment builds the same rig", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Player1", true);
	const Entity model = LoadCharacter(store, player);
	REQUIRE(model != NULL_ENTITY);

	const Entity humanoid = store.Get<Character>(model)->Humanoid;

	// **What a client receives: the `PlayerCharacter` row and nothing else
	// resolved.** `Character` crosses the wire too, but the two arrive as
	// separate component streams and the frame between them is real - and a
	// client whose `SubmitMove` refuses on that frame sends no keys at all.
	store.Remove<Character>(model);
	CHECK(UpdateCharacterControl(store) == 0);

	CHECK(LinkPlayerCharacters(store) == 1);
	CHECK(store.Get<Character>(model)->Owner == player);
	CHECK(store.Get<Character>(model)->Humanoid == humanoid);

	// Settled, it costs two reads and reports nothing.
	CHECK(LinkPlayerCharacters(store) == 0);

	world.Input().Down.Set(KeyCode::W, true);
	CHECK(UpdateCharacterControl(store) == 1);

	// **A model destroyed under its player releases the player.** Left alone
	// the reference dangles and the pass retries the same dead entity every
	// tick for the life of the world.
	store.DestroyInstance(model);
	CHECK(LinkPlayerCharacters(store) == 1);
	CHECK(store.Get<PlayerCharacter>(player)->Model == NULL_ENTITY);
	CHECK(LinkPlayerCharacters(store) == 0);
}

TEST_CASE("Player.Character is a writable script property", "[scene][characters]") {
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Player1");
	const Entity model = LoadCharacter(store, player);
	REQUIRE(model != NULL_ENTITY);

	const engine::ecs::PropertyDescriptor *found = nullptr;
	for (const auto &property : engine::ecs::Classes::Describe(engine::scene::PlayerClass()).Properties) {
		if (property.Name == engine::core::Name("Character")) {
			found = &property;
		}
	}

	// **The property is what a script actually reaches**, and the two halves
	// of the pair below are the whole of `player.Character = model`.
	REQUIRE(found != nullptr);
	REQUIRE(found->Writable);
	REQUIRE(found->Get != nullptr);
	REQUIRE(found->Set != nullptr);

	Entity read = NULL_ENTITY;
	CHECK(found->Get(store, player, &read));
	CHECK(read == model);

	const Entity cleared = NULL_ENTITY;
	CHECK(found->Set(store, player, &cleared));
	CHECK(CharacterOf(store, player) == NULL_ENTITY);

	CHECK(found->Set(store, player, &model));
	CHECK(CharacterOf(store, player) == model);
}

TEST_CASE("a character does not outlive the player it belongs to", "[scene][characters]") {
	// **The direction `LinkPlayerCharacters` does not cover.** That function
	// releases a player whose model was destroyed; this is a player destroyed
	// under a model, and a character is a `Model` under Workspace rather than a
	// child of the `Player` - so nothing takes it along.
	//
	// Only two places ever handled it, and both by hand:
	// `studio::PlayLink::Stop` and `TeleportService:Teleport` call
	// `RemoveCharacter` before destroying the instance. Every other way a player
	// can stop existing left a rig standing on the spawn: a `Humanoid` nobody
	// drives, a root the solver keeps awake, six limbs `PoseCharacters` follows.
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Player1", true);
	REQUIRE(player != NULL_ENTITY);

	const Entity model = LoadCharacter(store, player);
	REQUIRE(model != NULL_ENTITY);

	const Entity root = Limb(store, model, "HumanoidRootPart");
	REQUIRE(root != NULL_ENTITY);

	// A settled world collects nothing, which is the every-tick case.
	CHECK(engine::scene::ReclaimOrphanedCharacters(store) == 0);

	// **Destroyed the way everything except those two callers destroys one**:
	// no `RemoveCharacter`, because the whole point is that they had to
	// remember and nothing else does.
	store.DestroyInstance(player);
	REQUIRE_FALSE(store.Alive(player));

	INFO("the body is still standing there with nobody in it");
	CHECK(engine::scene::ReclaimOrphanedCharacters(store) == 1);

	CHECK_FALSE(store.Alive(model));

	// **The limbs too**, because destroying the model alone would leave six
	// parts following an entity that is not alive.
	CHECK_FALSE(store.Alive(root));

	// Idempotent: a second pass over a collected world finds nothing.
	CHECK(engine::scene::ReclaimOrphanedCharacters(store) == 0);
}

TEST_CASE("an NPC is never collected, because it never had an owner", "[scene][characters]") {
	// `Character::Owner` is the test rather than the presence of a `Character`,
	// and a scripted character leaves it null. Collecting those would empty
	// every examples scene in the repository on its first tick.
	World world;
	Store &store = world.Store_;

	CharacterDesc desc;
	desc.Frame = CFrame(Vector3{4.0f, 0.0f, 0.0f});
	const Entity npc = MakeCharacter(store, desc);
	REQUIRE(npc != NULL_ENTITY);

	const Character *rig = store.Get<Character>(npc);
	REQUIRE(rig != nullptr);
	REQUIRE(rig->Owner == NULL_ENTITY);

	CHECK(engine::scene::ReclaimOrphanedCharacters(store) == 0);
	CHECK(store.Alive(npc));
}

TEST_CASE("every spawn re-copies the character scripts and refills the backpack", "[scene][characters]") {
	// **Two spawns, because one passes against code that only ever runs the
	// pipeline once.** The whole point of the `Starter*` split is *when* each
	// container is copied: `StarterPlayerScripts` and `StarterPack` land at the
	// join and never again, and `StarterCharacterScripts` and `StarterGear` land
	// on every life.
	World world;
	Store &store = world.Store_;

	const auto plain = engine::ecs::Classes::Find(engine::core::Name("Instance"));
	const Entity starterPlayer = store.FindFirstRoot("StarterPlayer");
	const Entity characterScripts = store.FindFirstChild(starterPlayer, "StarterCharacterScripts");
	const Entity playerScripts = store.FindFirstChild(starterPlayer, "StarterPlayerScripts");
	const Entity pack = store.FindFirstRoot("StarterPack");
	REQUIRE(characterScripts != NULL_ENTITY);

	store.SetParent(store.CreateInstance(plain, "Animate"), characterScripts);
	store.SetParent(store.CreateInstance(plain, "CameraScript"), playerScripts);
	store.SetParent(store.CreateInstance(plain, "Sword"), pack);

	const Entity player = AddPlayer(store, "Runner");
	REQUIRE(player != NULL_ENTITY);

	const Entity backpack = store.FindFirstChild(player, engine::scene::BACKPACK_NAME);
	const Entity gear = store.FindFirstChild(player, engine::scene::STARTER_GEAR_NAME);
	const Entity scripts = store.FindFirstChild(player, engine::scene::PLAYER_SCRIPTS_NAME);
	REQUIRE(backpack != NULL_ENTITY);

	// --- first life -------------------------------------------------------
	const Entity firstBody = LoadCharacter(store, player);
	REQUIRE(firstBody != NULL_ENTITY);
	CHECK(store.FindFirstChild(firstBody, "Animate") != NULL_ENTITY);
	CHECK(Children(store, backpack) == 1);
	CHECK(store.FindFirstChild(backpack, "Sword") != NULL_ENTITY);

	// **Gear a game grants at run time goes in `StarterGear`**, which is the
	// container that survives - and something dropped straight into `Backpack`
	// is the one that does not. Both are set up here and checked after the
	// second spawn.
	store.SetParent(store.CreateInstance(plain, "Torch"), gear);
	store.SetParent(store.CreateInstance(plain, "Rubbish"), backpack);
	CHECK(Children(store, backpack) == 2);

	// --- second life ------------------------------------------------------
	const Entity secondBody = LoadCharacter(store, player);
	REQUIRE(secondBody != NULL_ENTITY);
	CHECK(secondBody != firstBody);

	// The new body has its own copy of the character script - not the old
	// body's, which went with it.
	const Entity animate = store.FindFirstChild(secondBody, "Animate");
	REQUIRE(animate != NULL_ENTITY);
	CHECK(animate != store.FindFirstChild(characterScripts, "Animate"));

	// Emptied and refilled from `StarterGear`: the granted torch is there, the
	// rubbish is not.
	CHECK(Children(store, backpack) == 2);
	CHECK(store.FindFirstChild(backpack, "Sword") != NULL_ENTITY);
	CHECK(store.FindFirstChild(backpack, "Torch") != NULL_ENTITY);
	CHECK(store.FindFirstChild(backpack, "Rubbish") == NULL_ENTITY);

	// **And the join's two copies were not made again**, which is the half a
	// one-spawn test cannot see: a second `CameraScript` would mean the client's
	// own code ran twice, and a second `Sword` would mean gear doubling per
	// death.
	CHECK(Children(store, scripts) == 1);
	CHECK(Children(store, gear) == 2);
}

TEST_CASE("a respawn waits exactly RespawnTime and then repeats", "[scene][characters]") {
	// **Two full cycles, because one passes against a pass that fires once.**
	// The deadline is a tick number computed from the fixed delta, so the same
	// world respawns on the same tick whatever machine ran it.
	World world;
	Store &store = world.Store_;

	auto *settings =
		store.GetMutable<engine::scene::PlayersServiceComponent>(engine::scene::PlayersOf(store));
	REQUIRE(settings != nullptr);
	settings->RespawnTime = 0.5f;

	const Entity player = AddPlayer(store, "Faller");
	REQUIRE(player != NULL_ENTITY);

	std::vector<Entity> spawned;

	// **The first pass is the initial spawn**, because a player with no
	// character and auto-loading on is a player waiting for one - which is the
	// same rule a death goes through rather than a second one beside it.
	const auto advance = [&](size_t ticks) {
		size_t made = 0;
		for (size_t index = 0; index < ticks; index++) {
			store.AdvanceTick(1.0f / 60.0f);
			(void)LinkPlayerCharacters(store);
			made += engine::scene::UpdateRespawns(store, spawned);
		}
		return made;
	};

	// **The first pass notices and schedules; the deadline is thirty ticks
	// after that**, because half a second at sixty hertz is thirty ticks and
	// `ceil` is computed from the tick the wait began on. So thirty passes are
	// not enough and the thirty-first is.
	CHECK(advance(30) == 0);
	CHECK(CharacterOf(store, player) == NULL_ENTITY);

	REQUIRE(advance(1) == 1);
	const Entity firstBody = CharacterOf(store, player);
	REQUIRE(firstBody != NULL_ENTITY);
	CHECK(spawned.size() == 1);
	CHECK(spawned.front() == player);

	// **A living player is never waiting**, so the pass is idle for as long as
	// the body is there. A version that rescheduled would respawn somebody who
	// had not died.
	CHECK(advance(120) == 0);
	CHECK(CharacterOf(store, player) == firstBody);

	// --- and around again -------------------------------------------------
	store.DestroyInstance(firstBody);

	CHECK(advance(30) == 0);
	CHECK(CharacterOf(store, player) == NULL_ENTITY);

	REQUIRE(advance(1) == 1);
	const Entity secondBody = CharacterOf(store, player);
	REQUIRE(secondBody != NULL_ENTITY);
	CHECK(secondBody != firstBody);

	// **And the flag stops it.** A lobby sets this and spawns its occupants
	// itself; a pass that ignored it would hand everybody a body the game then
	// has to destroy.
	store.GetMutable<engine::scene::PlayersServiceComponent>(engine::scene::PlayersOf(store))
		->CharacterAutoLoads = false;
	store.DestroyInstance(secondBody);
	CHECK(advance(240) == 0);
	CHECK(CharacterOf(store, player) == NULL_ENTITY);
}

TEST_CASE("a character dies at zero, waits where it fell, and is replaced", "[scene][characters]") {
	// **`docs/retired/DEFERRED.md` D00121's whole subject.** Until v0.15 a character
	// died by being *destroyed* and the respawn clock started from the tick a
	// player was first seen with no model - Roblox's delay with Roblox's trigger
	// missing. This is the trigger: health reaches zero, the body stays where it
	// fell for `Player.RespawnTime`, and only then is it removed and replaced.
	World world;
	Store &store = world.Store_;

	auto *settings =
		store.GetMutable<engine::scene::PlayersServiceComponent>(engine::scene::PlayersOf(store));
	REQUIRE(settings != nullptr);
	settings->RespawnTime = 0.5f;

	const Entity player = AddPlayer(store, "Victim");
	const Entity body = LoadCharacter(store, player);
	REQUIRE(body != NULL_ENTITY);

	const Entity humanoid = store.Get<Character>(body)->Humanoid;
	REQUIRE(store.Get<Humanoid>(humanoid)->Health == Approx(100.0f));

	std::vector<Entity> spawned;
	const auto advance = [&](size_t ticks) {
		size_t made = 0;
		for (size_t index = 0; index < ticks; index++) {
			store.AdvanceTick(1.0f / 60.0f);
			(void)LinkPlayerCharacters(store);
			made += engine::scene::UpdateRespawns(store, spawned);
		}
		return made;
	};

	// **Three hits report nothing and the fourth reports the death.** The return
	// is "this call killed it" rather than the health left, because that is the
	// fact a caller cannot derive from a number it reads afterwards.
	CHECK_FALSE(TakeDamage(store, humanoid, 25.0f));
	CHECK_FALSE(TakeDamage(store, humanoid, 25.0f));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(50.0f));
	CHECK_FALSE(TakeDamage(store, humanoid, 25.0f));
	REQUIRE(TakeDamage(store, humanoid, 25.0f));

	// **And the fifth kills nobody**, which is what makes a death happen once
	// without a flag on the row: two hits landing in one tick would otherwise
	// each read zero as a fresh death.
	CHECK_FALSE(TakeDamage(store, humanoid, 25.0f));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(0.0f));

	// Nothing is destroyed by dying. The body is still the player's, and a
	// version that removed it here would be the old rule wearing a new name.
	CHECK(store.Alive(body));
	CHECK(CharacterOf(store, player) == body);

	// **Thirty ticks of half a second is not yet half a second**, for the reason
	// the respawn case above gives: the deadline is `ceil(seconds / delta)` from
	// the tick the wait was noticed on, so the thirty-first is the one.
	CHECK(advance(30) == 0);
	CHECK(store.Alive(body));
	CHECK(CharacterOf(store, player) == body);

	REQUIRE(advance(1) == 1);
	const Entity replacement = CharacterOf(store, player);
	REQUIRE(replacement != NULL_ENTITY);
	CHECK(replacement != body);
	CHECK_FALSE(store.Alive(body));

	// A new body is a whole one, which is `MakeCharacter`'s default rather than
	// anything the respawn had to remember.
	const Entity fresh = store.Get<Character>(replacement)->Humanoid;
	CHECK(store.Get<Humanoid>(fresh)->Health == Approx(100.0f));

	// **And a living player is still never waiting.** The branch that decides
	// this now asks whether the body has health rather than whether it exists,
	// so a version that got the polarity wrong would respawn everybody for ever
	// - and this is what would catch it.
	CHECK(advance(120) == 0);
	CHECK(CharacterOf(store, player) == replacement);
}

TEST_CASE("a replica may not take health off anybody", "[scene][characters]") {
	// **The authority decision, and it is one rule with two doors.**
	// `ecs::Store::SetProperty` already refuses every property write in a
	// replica - that is where this engine answers "who owns a row" - and
	// `TakeDamage` makes the same refusal for the C++ door. A client holds a
	// copy of every `scene.Humanoid` in the world, so a client allowed to
	// subtract from one is a client deciding who died.
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Owned");
	const Entity body = LoadCharacter(store, player);
	REQUIRE(body != NULL_ENTITY);
	const Entity humanoid = store.Get<Character>(body)->Humanoid;

	const engine::ecs::PropertyDescriptor *health = nullptr;
	for (const auto &property : engine::ecs::Classes::Describe(engine::scene::HumanoidClass()).Properties) {
		if (property.Name == engine::core::Name("Health")) {
			health = &property;
		}
	}

	// **Writable, deliberately.** Declaring it read-only would have stopped the
	// *server* script that wants to kill somebody, which is the ordinary way a
	// game does it - the question is who is asking, not whether the value has a
	// meaningful assignment.
	REQUIRE(health != nullptr);
	REQUIRE(health->Writable);

	store.SetAdoptOnly(true);

	// **Lethal, so the *return* is an assertion too.** A replica allowed to run
	// this would report the kill as well as taking the health, and a case that
	// only ever asked for a scratch would pass on the return value whether the
	// refusal was there or not.
	CHECK_FALSE(TakeDamage(store, humanoid, 1000.0f));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(100.0f));

	const float wanted = 1000.0f;
	CHECK_FALSE(store.SetProperty(humanoid, *health, &wanted, sizeof(float)));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(100.0f));

	// The same two calls against the authority, so the refusal above is about
	// the world and not about the arguments.
	store.SetAdoptOnly(false);
	CHECK_FALSE(TakeDamage(store, humanoid, 25.0f));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(75.0f));
	CHECK(store.SetProperty(humanoid, *health, &wanted, sizeof(float)));
}

TEST_CASE("health and its ceiling are clamped against each other", "[scene][characters]") {
	// **What gives `MaxHealth` a reader inside the engine.** A property nothing
	// here consults is the objection D00119 held a whole class back for, so the
	// ceiling means something rather than only decorating whatever bar a game
	// draws: `Health` clamps against it, and lowering it pulls the health down.
	World world;
	Store &store = world.Store_;

	const Entity model = MakeCharacter(store, CharacterDesc{});
	const Entity humanoid = store.Get<Character>(model)->Humanoid;

	const auto property = [&](std::string_view name) {
		const engine::ecs::PropertyDescriptor *found = nullptr;
		for (const auto &candidate :
			 engine::ecs::Classes::Describe(engine::scene::HumanoidClass()).Properties) {
			if (candidate.Name == engine::core::Name(name)) {
				found = &candidate;
			}
		}
		return found;
	};

	const engine::ecs::PropertyDescriptor *health = property("Health");
	const engine::ecs::PropertyDescriptor *ceiling = property("MaxHealth");
	REQUIRE(health != nullptr);
	REQUIRE(ceiling != nullptr);

	// Clamped rather than refused, which is `ClampedProperty`'s own argument: a
	// bar driven off a falling number overshoots at both ends, and a refusal
	// would leave a character alive on the frame a game meant to kill them.
	float wanted = 5000.0f;
	CHECK(health->Set(store, humanoid, &wanted));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(100.0f));

	wanted = -5.0f;
	CHECK(health->Set(store, humanoid, &wanted));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(0.0f));
	CHECK(engine::scene::IsDead(*store.Get<Humanoid>(humanoid)));

	wanted = 250.0f;
	CHECK(ceiling->Set(store, humanoid, &wanted));
	wanted = 250.0f;
	CHECK(health->Set(store, humanoid, &wanted));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(250.0f));

	// **The ceiling takes the health down with it**, which is Roblox's rule and
	// the only one that leaves the two numbers able to agree: without it a
	// humanoid reports more life than it is allowed to have and nothing can say
	// which of the two is wrong.
	wanted = 40.0f;
	CHECK(ceiling->Set(store, humanoid, &wanted));
	CHECK(store.Get<Humanoid>(humanoid)->Health == Approx(40.0f));

	float read = 0.0f;
	CHECK(ceiling->Get(store, humanoid, &read));
	CHECK(read == Approx(40.0f));

	// A ceiling of zero is a thing that cannot be alive, and it falls out of the
	// clamp rather than being a second rule.
	wanted = 0.0f;
	CHECK(ceiling->Set(store, humanoid, &wanted));
	CHECK(engine::scene::IsDead(*store.Get<Humanoid>(humanoid)));
}

TEST_CASE("a wounded character comes back wounded from a file", "[scene][characters]") {
	// **Health is on `scene.Humanoid`, which is registered with the generated
	// serialiser** - so this is not a check that somebody wrote a field into a
	// hand-written pair. It is a check that the two floats went into the object
	// representation rather than off the end of it: a world saved mid-fight and
	// reopened at full health would be a save format that quietly discards the
	// fact the whole entry is about.
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Wounded");
	const Entity body = LoadCharacter(store, player);
	REQUIRE(body != NULL_ENTITY);
	const Entity humanoid = store.Get<Character>(body)->Humanoid;

	REQUIRE_FALSE(TakeDamage(store, humanoid, 30.0f));
	store.GetMutable<Humanoid>(humanoid)->MaxHealth = 120.0f;

	engine::core::ByteWriter writer;
	REQUIRE(store.Save(writer));

	Store restored("characters-test.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const Humanoid *back = restored.Get<Humanoid>(humanoid);
	REQUIRE(back != nullptr);
	CHECK(back->Health == Approx(70.0f));
	CHECK(back->MaxHealth == Approx(120.0f));
	CHECK_FALSE(engine::scene::IsDead(*back));
}

TEST_CASE("a character arriving and leaving is recorded once, in order", "[scene][characters]") {
	// **What `Player.CharacterAdded` and `CharacterRemoving` are built on, and
	// the queue deliberately does not carry all of it.** `scene` is L7 and a
	// signal is L9, so the transitions are written down here and whoever owns a
	// scripting layer drains them - the same split `ecs::Store::TakeTreeChanges`
	// is on.
	//
	// **A body that is *destroyed* is not this queue's to report.** Dying in this
	// engine is being destroyed, so a queue drained a tick later would hand a
	// handler a model it cannot read a property off - `script` fires
	// `CharacterRemoving` from `Store::OnDescendantRemoving` instead, while the
	// model is still whole. What is left here is the release that did not
	// destroy, which is `player.Character = nil`.
	World world;
	Store &store = world.Store_;

	const Entity player = AddPlayer(store, "Recorded");
	REQUIRE(player != NULL_ENTITY);

	std::vector<engine::scene::CharacterChange> changes;
	engine::scene::TakeCharacterChanges(store, changes);
	CHECK(changes.empty());

	const Entity firstBody = LoadCharacter(store, player);
	REQUIRE(firstBody != NULL_ENTITY);

	engine::scene::TakeCharacterChanges(store, changes);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].Added);
	CHECK(changes[0].Player == player);
	CHECK(changes[0].Character == firstBody);

	// **Drained, not read**, so a handler that respawns somebody records into an
	// empty list rather than appending to the one being walked.
	engine::scene::TakeCharacterChanges(store, changes);
	CHECK(changes.empty());

	// **A release that keeps the body is the one departure this queue carries**,
	// and the model is deliberately still alive afterwards - Roblox's
	// `player.Character = nil` does not destroy anything either.
	REQUIRE(SetPlayerCharacter(store, player, NULL_ENTITY));
	CHECK(store.Alive(firstBody));

	engine::scene::TakeCharacterChanges(store, changes);
	REQUIRE(changes.size() == 1);
	CHECK_FALSE(changes[0].Added);
	CHECK(changes[0].Character == firstBody);

	// **A respawn over a live body records the arrival and nothing else.** The
	// old model is destroyed, which is the hook's to report; a version that
	// queued it as well would fire twice in the language bindings.
	const Entity secondBody = LoadCharacter(store, player);
	REQUIRE(secondBody != NULL_ENTITY);
	const Entity thirdBody = LoadCharacter(store, player);
	REQUIRE(thirdBody != NULL_ENTITY);
	CHECK_FALSE(store.Alive(secondBody));

	engine::scene::TakeCharacterChanges(store, changes);
	REQUIRE(changes.size() == 2);
	CHECK(changes[0].Added);
	CHECK(changes[0].Character == secondBody);
	CHECK(changes[1].Added);
	CHECK(changes[1].Character == thirdBody);

	// **Re-linking the body a player already holds records nothing**, which is
	// what makes `LinkPlayerCharacters` cheap to run every tick - a version that
	// recorded would fire `CharacterAdded` once per tick for ever.
	CHECK(SetPlayerCharacter(store, player, thirdBody));
	(void)LinkPlayerCharacters(store);
	engine::scene::TakeCharacterChanges(store, changes);
	CHECK(changes.empty());
}
