// The half of a character controller that needs a query, and the two ways it
// was quietly wrong.
//
// **Both failures look like "the character is fine" from every other angle.** A
// humanoid with a perfect `MoveDirection`, a body the solver is holding on a
// floor, a rig whose parts add up — and it cannot jump, because `Grounded` is
// false; or it holds still for a second and then cannot walk at all, because a
// sleeping body has no `scene::Motion` to write a velocity into. Neither is
// visible in `scene`, which is why neither had a suite: `scene` may not link
// this module, so the query and the wake both live here.

#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.physics.characters")
// The ray that finds the floor, and the ignore that makes it find the floor
// rather than the character.
TEST_DEPENDS("engine.physics.query")
// The rig whose numbers this asks about, and `Humanoid::RootPart`.
TEST_DEPENDS("engine.scene.characters")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::physics::GroundCharacters;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::WakeMovingCharacters;
using engine::scene::Character;
using engine::scene::CharacterDesc;
using engine::scene::Humanoid;
using engine::scene::MakeCharacter;
using engine::scene::MakePart;
using engine::scene::Motion;
using engine::scene::PartDesc;

namespace {
	// A world with a floor, a physics index, and one character standing on it.
	struct Standing {
		Store World{"physics.characters"};
		Entity Floor;
		Entity Model;
		Entity Root;
		Entity Steering;

		Standing() {
			engine::scene::RegisterSceneClasses();
			engine::scene::InstallServices(World);
			PreparePhysicsWorld(World);

			PartDesc floor;
			floor.Size = Vector3{200.0f, 4.0f, 200.0f};
			floor.Frame = CFrame(Vector3{0.0f, -2.0f, 0.0f});
			floor.Anchored = true;
			Floor = MakePart(World, floor);
			World.SetParent(Floor, engine::scene::WorkspaceOf(World));

			Model = MakeCharacter(World, CharacterDesc{});
			const Character *rig = World.Get<Character>(Model);
			Root = rig->Root;
			Steering = rig->Humanoid;

			// The index the ray is answered against. A character standing on an
			// unindexed floor is standing on nothing.
			engine::physics::SyncBroadphase(World);
			engine::physics::BroadPhase(World);
		}

		const Humanoid &Body() {
			return *World.Get<Humanoid>(Steering);
		}
	};
}

TEST_CASE("a character standing on a floor is grounded", "[physics][characters]") {
	// **The ray starts inside the character's own feet, deliberately.** A ray
	// that begins exactly on a face is a coin flip about whether it hits it, and
	// the coin lands differently on two machines — so the origin is a tenth of a
	// metre up. With a root collider the full height of the character that
	// origin is inside its own box, and the nearest hit is therefore always the
	// character.
	//
	// **Which is why the caster is skipped by the query rather than compared
	// against the answer.** The old code did the second and read "not grounded"
	// while standing on a floor, for ever: a character that could not jump, on a
	// body the solver was visibly holding up.
	Standing world;

	REQUIRE(GroundCharacters(world.World) == 1);
	CHECK(world.Body().Grounded);

	// And a character with the floor taken away is not. The same pass, the same
	// tick, one row removed — so a false positive from the ignore would show
	// here as a character standing on nothing.
	world.World.DestroyInstance(world.Floor);
	engine::physics::SyncBroadphase(world.World);
	engine::physics::BroadPhase(world.World);

	REQUIRE(GroundCharacters(world.World) == 1);
	CHECK_FALSE(world.Body().Grounded);
}

TEST_CASE("a sleeping character is woken by intent and by falling", "[physics][characters]") {
	// **A sleeping body has no `scene::Motion`, and that is what sleeping *is*
	// here** — `physics::Publish` removes the component so the row leaves the
	// dynamic archetype entirely. Until v0.14 the only thing that ever gave it
	// back was a contact with an awake neighbour, so a character that stood
	// still settled and could never be walked again: `scene::StepCharacters` had
	// a perfectly good move direction and nowhere to write it.
	Standing world;

	// Asleep, as the solver leaves a body it has put to rest.
	world.World.Remove<Motion>(world.Root);
	REQUIRE(world.World.Get<Motion>(world.Root) == nullptr);

	// **Still, grounded and asleep stays asleep**, which is the half worth
	// protecting: standing still is what a player spends most of a session
	// doing, and waking every idle character every tick would put all of them
	// back through the integrator and the broad phase to hold position.
	REQUIRE(GroundCharacters(world.World) == 1);
	CHECK(WakeMovingCharacters(world.World) == 0);
	CHECK(world.World.Get<Motion>(world.Root) == nullptr);

	// Told to walk. The intent and not a key press, so a server applying a
	// `game::MoveInput` and a scripted NPC wake by the same rule.
	world.World.GetMutable<Humanoid>(world.Steering)->MoveDirection = Vector3{1.0f, 0.0f, 0.0f};
	(void)WakeMovingCharacters(world.World);
	CHECK(world.World.Get<Motion>(world.Root) != nullptr);

	// **And the case with no other answer: the floor goes away.** A sleeping
	// body is never integrated, and `physics`' own wake pass fires on a
	// *contact* — there is no contact when the support simply stops existing, so
	// without this the character hangs in the air for ever.
	world.World.GetMutable<Humanoid>(world.Steering)->MoveDirection = Vector3{};
	world.World.Remove<Motion>(world.Root);
	world.World.DestroyInstance(world.Floor);
	engine::physics::SyncBroadphase(world.World);
	engine::physics::BroadPhase(world.World);

	REQUIRE(GroundCharacters(world.World) == 1);
	CHECK_FALSE(world.Body().Grounded);
	(void)WakeMovingCharacters(world.World);
	CHECK(world.World.Get<Motion>(world.Root) != nullptr);
}
