// Where a surface camera ends up when it is parented to a face.
//
// **The half of a mirror a test can reach.** What the reflection *looks* like
// needs a GPU; whether the camera is in the right place is six dot products and
// can be asserted against — which is the same split `graph::Frustum` is built on
// and the reason this arithmetic lives in `scene` rather than in the renderer.
//
// The cases below are the ones that were wrong at some point in a script doing
// this by hand: a camera behind the pane facing away from it, a plane at the
// full extent instead of the half, and a mirror rendering perfectly into a
// texture that nothing sampled.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

TEST_SUITE_ID("engine.scene.surfacecameras")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::ActiveCamera;
using engine::scene::AimSurfaceCameras;
using engine::scene::Bounds;
using engine::scene::Camera;
using engine::scene::NormalId;
using engine::scene::NormalOf;
using engine::scene::SurfaceCamera;
using engine::scene::Transform;
using engine::scene::Visual;

namespace {
	constexpr float TOLERANCE = 1e-4f;

	// A world with a pane, a viewer, and a surface camera under the pane.
	struct Mirror {
		Store World{"surfacecameras"};
		Entity Pane;
		Entity Eye;
		Entity Reflection;

		explicit Mirror(NormalId face = NormalId::Front, const CFrame &paneFrame = CFrame(Vector3::Zero)) {
			// **Real instances through the class table, not bare rows.** The
			// parent lookup this whole system turns on is `Hierarchy`, and only
			// an instance carries one — a test built from `Store::Create` would
			// have exercised every line except the one that finds the face.
			// That is not hypothetical: it was the first version of this file
			// and every case returned zero.
			engine::scene::RegisterSceneClasses();

			Pane = World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Pane");
			World.Set<Transform>(Pane, Transform{paneFrame});
			World.Set<Bounds>(Pane, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

			Eye = World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Camera")), "Eye");
			World.Set<Transform>(Eye, Transform{CFrame(Vector3{0.0f, 0.0f, 20.0f})});
			World.SetResource(ActiveCamera{Eye, 16.0f / 9.0f});

			Reflection = World.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "Reflection"
			);

			SurfaceCamera target;
			target.Face = face;
			World.Set<SurfaceCamera>(Reflection, target);

			World.SetParent(Reflection, Pane);
		}

		Vector3 Placed() {
			return World.Get<Transform>(Reflection)->Frame.Position;
		}
	};
}

TEST_CASE("a face normal is Roblox's, including which way front points", "[scene][surfacecameras]") {
	// **`Front` is -Z and getting it backwards puts the reflection behind the
	// pane**, which renders the clear colour and reads as a mirror that does not
	// work. Worth an assertion rather than a comment: it is one sign, and it is
	// the sign that makes the whole feature look broken.
	CHECK(NormalOf(NormalId::Front) == Vector3{0.0f, 0.0f, -1.0f});
	CHECK(NormalOf(NormalId::Back) == Vector3{0.0f, 0.0f, 1.0f});
	CHECK(NormalOf(NormalId::Right) == Vector3{1.0f, 0.0f, 0.0f});
	CHECK(NormalOf(NormalId::Left) == Vector3{-1.0f, 0.0f, 0.0f});
	CHECK(NormalOf(NormalId::Top) == Vector3{0.0f, 1.0f, 0.0f});
	CHECK(NormalOf(NormalId::Bottom) == Vector3{0.0f, -1.0f, 0.0f});
}

TEST_CASE("the camera is mirrored through the face it is parented to", "[scene][surfacecameras]") {
	Mirror mirror;

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// The pane is at the origin with a *half* extent of 0.2 along Z, facing -Z.
	// So the face is at z = -0.2 and the eye at z = 20 is 20.2 in front of it,
	// which puts the reflection 20.2 behind at z = -20.4.
	//
	// Worked through rather than copied from the run: this expectation was
	// written as -20.2 first, by halving the half extent again. A number taken
	// from what the code printed would have agreed with the code about a mistake
	// they were both making.
	const Vector3 placed = mirror.Placed();
	CHECK_THAT(placed.X, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(placed.Y, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(placed.Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));

	// **Aimed, not merely placed.** Identity rotation looks down -Z, so a camera
	// put behind the pane would face *away* from it and render empty space —
	// which is what the first hand-written version of this did. It must look
	// back towards the face.
	const CFrame &frame = mirror.World.Get<Transform>(mirror.Reflection)->Frame;
	CHECK(frame.LookVector().Z > 0.9f);
}

TEST_CASE("the plane sits at the half extent, not the full one", "[scene][surfacecameras]") {
	// `Bounds::HalfExtent` is half of a full extent — the whole reason `Size` is
	// a conversion rather than a member pointer. A plane placed at the full
	// extent sits a whole part outside the part it belongs to, and the
	// reflection lands nowhere near the pane.
	Mirror mirror(NormalId::Top);

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// Face at y = +4.5, eye at y = 0 — so 4.5 below it, and the reflection 4.5
	// above at y = 9. A full-extent plane would put it at y = 18.
	CHECK_THAT(mirror.Placed().Y, Catch::Matchers::WithinAbs(9.0f, TOLERANCE));
}

TEST_CASE("aiming tells the part which surface it shows", "[scene][surfacecameras]") {
	Mirror mirror;

	// **The step that makes this an instance rather than a configuration.**
	// Requiring `Surface` to be set by hand as well as parenting the camera is
	// one fact recorded twice, and its failure mode is a camera rendering
	// perfectly into a texture nothing samples — which looks exactly like a
	// mirror that does not work.
	REQUIRE(mirror.World.Get<Visual>(mirror.Pane)->Surface == -1);

	AimSurfaceCameras(mirror.World);

	CHECK(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);
}

TEST_CASE("the near plane is pushed out to the glass", "[scene][surfacecameras]") {
	Mirror mirror;

	AimSurfaceCameras(mirror.World);

	// The poor-man's oblique clip: the reflected camera is behind the pane
	// looking through it, so everything between the two — the frame, the back of
	// the pane, whatever the viewer stands behind — would occlude the
	// reflection. Clipping at the plane removes exactly that half of the world.
	const Camera *lens = mirror.World.Get<Camera>(mirror.Reflection);
	REQUIRE(lens != nullptr);
	CHECK_THAT(lens->NearPlane, Catch::Matchers::WithinAbs(20.5f, TOLERANCE));
}

TEST_CASE("a camera with no part parent is left where it was put", "[scene][surfacecameras]") {
	Mirror mirror;

	// The script-authored arrangement, which still works: this adds a way to
	// place a surface camera and takes none away.
	const CFrame placed(Vector3{3.0f, 4.0f, 5.0f});
	mirror.World.SetParent(mirror.Reflection, engine::ecs::NULL_ENTITY);
	mirror.World.Set<Transform>(mirror.Reflection, Transform{placed});

	CHECK(AimSurfaceCameras(mirror.World) == 0);
	CHECK(mirror.Placed() == placed.Position);
}

TEST_CASE("a rotated pane reflects along the way it actually faces", "[scene][surfacecameras]") {
	// Turned a quarter turn about Y, so the front face now looks down -X and the
	// reflection has to follow it. A version that used the world axis rather
	// than the part's own would put the camera behind where the pane used to be.
	Mirror mirror(NormalId::Front, CFrame(Vector3::Zero, CFrame::Angles(0.0f, 1.5707963f, 0.0f).Rotation()));

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// The face normal is now -X, the eye is at +Z — so it is level with the
	// plane rather than in front of it, and the mirrored position is the eye
	// itself. What matters is that nothing moved along Z, which a world-axis
	// reflection would have done.
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(20.0f, TOLERANCE));
}

TEST_CASE("a world with no active camera has nothing to reflect", "[scene][surfacecameras]") {
	Mirror mirror;

	// A mirror shows the viewer's world, so a world nobody is looking at has no
	// reflection to compute rather than a default one. Refusing to guess is what
	// stops a surface camera being aimed somewhere arbitrary and then looking
	// like it was aimed wrong.
	mirror.World.SetResource(ActiveCamera{});

	CHECK(AimSurfaceCameras(mirror.World) == 0);
}
