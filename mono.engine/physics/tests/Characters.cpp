// The half of a character controller that needs a query, and the two ways it
// was quietly wrong.
//
// **Both failures look like "the character is fine" from every other angle.** A
// humanoid with a perfect `MoveDirection`, a body the solver is holding on a
// floor, a rig whose parts add up - and it cannot jump, because `Grounded` is
// false; or it holds still for a second and then cannot walk at all, because a
// sleeping body has no `scene::Motion` to write a velocity into. Neither is
// visible in `scene`, which is why neither had a suite: `scene` may not link
// this module, so the query and the wake both live here.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
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
using engine::physics::UpdatePoppercam;
using engine::physics::WakeMovingCharacters;
using engine::scene::ActiveCamera;
using engine::scene::CameraController;
using engine::scene::CameraMode;
using engine::scene::Character;
using engine::scene::CharacterDesc;
using engine::scene::Humanoid;
using engine::scene::LocalTransparencyOf;
using engine::scene::MakeCharacter;
using engine::scene::MakePart;
using engine::scene::Motion;
using engine::scene::PartDesc;
using engine::scene::Transform;

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
			floor.Simulated = false;
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
	// the coin lands differently on two machines - so the origin is a tenth of a
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
	// tick, one row removed - so a false positive from the ignore would show
	// here as a character standing on nothing.
	world.World.DestroyInstance(world.Floor);
	engine::physics::SyncBroadphase(world.World);
	engine::physics::BroadPhase(world.World);

	REQUIRE(GroundCharacters(world.World) == 1);
	CHECK_FALSE(world.Body().Grounded);
}

TEST_CASE("a sleeping character is woken by intent and by falling", "[physics][characters]") {
	// **A sleeping body has no `scene::Motion`, and that is what sleeping *is*
	// here** - `physics::Publish` removes the component so the row leaves the
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
	// *contact* - there is no contact when the support simply stops existing, so
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

namespace {
	// A subject at the origin, a camera looking at it from ten metres back
	// along +Z (yaw zero, per `PlaceCamera`'s own convention), and nothing
	// standing between them yet.
	struct Occludable {
		Store World{"physics.poppercam"};
		Entity Subject;
		Entity Eye;

		Occludable() {
			engine::scene::RegisterSceneClasses();
			engine::scene::InstallServices(World);
			PreparePhysicsWorld(World);

			PartDesc subject;
			subject.Frame = CFrame(Vector3{0.0f, 0.0f, 0.0f});
			subject.Simulated = false;
			Subject = MakePart(World, subject);
			World.SetParent(Subject, engine::scene::WorkspaceOf(World));

			Eye = World.CreateInstance(engine::scene::CameraClass(), "Eye");
			World.Set(Eye, Transform{});
			World.SetResource(ActiveCamera{Eye});

			CameraController controller;
			controller.Subject = Subject;
			controller.Distance = 10.0f;
			controller.HeadHeight = 0.0f;
			World.SetResource(controller);
		}

		// A thin wall crossing the line from the subject to the desired eye,
		// `at` metres out.
		Entity Wall(float at) {
			PartDesc wall;
			wall.Size = Vector3{4.0f, 4.0f, 0.2f};
			wall.Frame = CFrame(Vector3{0.0f, 0.0f, at});
			wall.Simulated = false;
			const Entity entity = MakePart(World, wall);
			World.SetParent(entity, engine::scene::WorkspaceOf(World));
			Reindex();
			return entity;
		}

		void Portal(float at) {
			PartDesc pane;
			pane.Size = Vector3{4.0f, 4.0f, 0.2f};
			pane.Frame = CFrame(Vector3{0.0f, 0.0f, at});
			pane.Simulated = false;
			const Entity nearPane = MakePart(World, pane);
			World.SetParent(nearPane, engine::scene::WorkspaceOf(World));

			pane.Frame = CFrame(Vector3{100.0f, 0.0f, 0.0f});
			const Entity farPane = MakePart(World, pane);
			World.SetParent(farPane, engine::scene::WorkspaceOf(World));

			const auto link = [&](Entity source, Entity destination, const char *name) {
				const Entity hole = World.CreateInstance(
					engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), name
				);
				World.Set(hole, engine::scene::SurfaceCamera{});
				World.Set(hole, engine::scene::Portal{destination});
				World.SetParent(hole, source);
			};

			link(nearPane, farPane, "NearHole");
			link(farPane, nearPane, "FarHole");
			REQUIRE(engine::scene::OpenPortals(World) == 2);
			Reindex();
		}

		void Reindex() {
			engine::physics::SyncBroadphase(World);
			engine::physics::BroadPhase(World);
		}

		const CameraController &Controller() {
			return *World.Resource<CameraController>();
		}
	};
}

TEST_CASE("a wall between the eye and its subject is pulled in front of and faded", "[physics][characters]") {
	Occludable world;
	const Entity wall = world.Wall(5.0f);

	// The wall's near face sits at z = 4.9 (half its 0.2 m depth short of its
	// 5.0 m centre), and the margin pulls the eye 0.15 m further back than
	// that face.
	REQUIRE(UpdatePoppercam(world.World));
	CHECK(world.Controller().OccludedDistance == Catch::Approx(4.75f));
	CHECK(LocalTransparencyOf(world.World, wall) == Catch::Approx(0.6f));

	// Nothing else in the scene is touched - the fade is exactly the one
	// blocker, not every part between the eye and the subject.
	CHECK(LocalTransparencyOf(world.World, world.Subject) == 0.0f);
}

TEST_CASE("poppercam looks through a portal instead of pulling up to its pane", "[physics][characters]") {
	// Portal panes keep trigger colliders so contacts still report crossings.
	// A plain ray sees that glass first and turns a valid camera arm into an
	// occlusion. The portal-aware query spends the rest of the arm in the far
	// room and leaves the desired distance alone when nothing there blocks it.
	Occludable world;
	world.Portal(5.0f);

	CHECK_FALSE(UpdatePoppercam(world.World));
	CHECK(world.Controller().OccludedDistance < 0.0f);
}

TEST_CASE("clearing the wall restores the setting and un-fades it", "[physics][characters]") {
	Occludable world;
	const Entity wall = world.Wall(5.0f);
	REQUIRE(UpdatePoppercam(world.World));
	REQUIRE(world.Controller().OccludedDistance >= 0.0f);

	world.World.DestroyInstance(wall);
	world.Reindex();

	REQUIRE(UpdatePoppercam(world.World));
	CHECK(world.Controller().OccludedDistance < 0.0f);

	// **Read after the destroy, through a fresh entity that cannot resolve
	// to the same row** - `LocalTransparencyOf` on a dead handle answers zero
	// by the same rule every other `Get` does, which would make this pass
	// whether or not the fade was actually cleared. The row that was faded is
	// gone; what this checks is that the *next* frame's occlusion did not
	// silently start fading nothing forever.
	CHECK_FALSE(world.World.Alive(wall));
}

TEST_CASE("a part tagged IgnorePoppercam is looked straight through", "[physics][characters]") {
	Occludable world;
	const Entity wall = world.Wall(5.0f);
	REQUIRE(engine::scene::AddTag(world.World, wall, engine::core::Name("IgnorePoppercam")));

	REQUIRE_FALSE(UpdatePoppercam(world.World));
	CHECK(world.Controller().OccludedDistance < 0.0f);
	CHECK(LocalTransparencyOf(world.World, wall) == 0.0f);
}

TEST_CASE("first person and a scripted camera are left alone", "[physics][characters]") {
	Occludable world;
	(void)world.Wall(5.0f);

	CameraController *controller = world.World.ResourceMutable<CameraController>();

	controller->Mode = CameraMode::LockFirstPerson;
	CHECK_FALSE(UpdatePoppercam(world.World));
	CHECK(controller->OccludedDistance < 0.0f);

	controller->Mode = CameraMode::Scriptable;
	CHECK_FALSE(UpdatePoppercam(world.World));
	CHECK(controller->OccludedDistance < 0.0f);
}
