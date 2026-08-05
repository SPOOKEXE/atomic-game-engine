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
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/vec4.hpp>

#include <cmath>

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

TEST_CASE("the frustum covers the whole pane, however close the viewer stands", "[scene][surfacecameras]") {
	// **The bug a screenshot showed and no test did.** The reflection is
	// projected back onto the pane per fragment and `opaque.frag` tests the
	// projected coordinate against the texture's 0..1 rectangle, falling through
	// to the plain lit pane outside it. A frustum narrower than the pane
	// therefore does not produce a smaller or softer image — it produces a
	// hard-edged rectangle of reflection floating on a grey wall, which moves and
	// resizes as the viewer walks and reads as a mirror aimed at the wrong thing.
	//
	// A constant field of view cannot cover it. The camera stands as far behind
	// the glass as the viewer stands in front, so the pane subtends *the same
	// angle from the camera as from the viewer* — and that grows without bound as
	// somebody walks up to a mirror. The authored 70° covered this pane at twenty
	// units and covered a third of it at two.
	//
	// So what is asserted is the thing that actually matters, rather than an
	// angle: every corner of the pane projects inside the image the camera
	// renders.
	Mirror mirror;

	// Half extents 8 by 4.5 on a pane facing -Z, so the face is at z = -0.2 and
	// its corners are the four combinations of those.
	const auto cornersAreCovered = [&]() {
		const Transform *placed = mirror.World.Get<Transform>(mirror.Reflection);
		const Camera *lens = mirror.World.Get<Camera>(mirror.Reflection);
		const SurfaceCamera *target = mirror.World.Get<SurfaceCamera>(mirror.Reflection);
		REQUIRE(placed != nullptr);
		REQUIRE(lens != nullptr);
		REQUIRE(target != nullptr);

		const float aspect = static_cast<float>(target->Width) / static_cast<float>(target->Height);
		const glm::mat4 viewProjection =
			engine::scene::ResolveCamera(placed->Frame, *lens, aspect).ViewProjection;

		bool covered = true;
		for (const float x : {-8.0f, 8.0f}) {
			for (const float y : {-4.5f, 4.5f}) {
				const glm::vec4 clip = viewProjection * glm::vec4(x, y, -0.2f, 1.0f);
				INFO("corner " << x << ", " << y << " has w " << clip.w);

				// Behind the camera is not covered, and saying so beats a divide
				// that flips the sign and reports the corner as central.
				if (!(clip.w > 0.0f)) {
					covered = false;
					continue;
				}

				covered = covered && std::abs(clip.x / clip.w) <= 1.0f && std::abs(clip.y / clip.w) <= 1.0f;
			}
		}
		return covered;
	};

	// Twenty units back, which the old constant did cover.
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(cornersAreCovered());

	// **Two units, which it did not.** The pane needs about 150 degrees from
	// here and had 70, so the corners fell outside the texture and the wall drew
	// its own grey around a rectangle of reflection.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 2.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(cornersAreCovered());

	// And off to one side as well as close, which is the case a symmetric
	// frustum has to widen for rather than shift.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{6.0f, 3.0f, 3.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(cornersAreCovered());

	// **Far enough away that the fit narrows rather than widens**, which is the
	// half a "make it wider" fix would pass without doing: a distant pane gets a
	// frustum tight around it, and the texels go on the mirror instead of on the
	// room around it.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 200.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(cornersAreCovered());
	CHECK(mirror.World.Get<Camera>(mirror.Reflection)->FieldOfViewRadians < 0.2f);
}

TEST_CASE(
	"a pane in the plane of the viewer does not produce an infinite frustum", "[scene][surfacecameras]"
) {
	// The 180 degree case, arriving. The viewer walks into the glass, the
	// reflected camera arrives with it, and the angle the pane subtends goes to
	// half a turn — which no projection covers and which `tan` answers with
	// infinity. Clamped, so the frame is a reflection that stops covering the far
	// corners rather than a matrix full of infinities spreading into every
	// culled bound.
	Mirror mirror;
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	const Camera *lens = mirror.World.Get<Camera>(mirror.Reflection);
	REQUIRE(lens != nullptr);
	CHECK(std::isfinite(lens->FieldOfViewRadians));
	CHECK(lens->FieldOfViewRadians > 0.0f);
	CHECK(lens->FieldOfViewRadians < 3.1415926f);
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

TEST_CASE("the face marker lies on the face the camera projects off", "[scene][surfacecameras]") {
	Mirror mirror;

	std::vector<engine::scene::DrawInstance> list;
	REQUIRE(engine::scene::AppendSurfaceFaceMarkers(mirror.World, list) == 1);
	REQUIRE(list.size() == 1);

	// The same plane the reflection is computed through — face at z = -0.2 —
	// pushed one thickness clear of it so the two do not z-fight. **The sign is
	// the whole assertion**: a marker at z = +0.2 would be sitting on the back
	// of the pane, which is a debugging aid that points at the wrong face and is
	// worse than none at all.
	const engine::scene::DrawInstance &marker = list.front();
	CHECK_THAT(marker.Frame.Position.X, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(marker.Frame.Position.Y, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK(marker.Frame.Position.Z < -0.2f);

	// Along the pane's *longer* in-plane axis — 8 wide against 4.5 tall — and
	// thin on the other two. A bar across the short axis is a dash somebody has
	// to look for.
	CHECK(marker.HalfExtent.X > marker.HalfExtent.Y);
	CHECK(marker.HalfExtent.X > marker.HalfExtent.Z);
	CHECK(marker.HalfExtent.X < 8.0f);

	// Blended, and that is load-bearing rather than cosmetic: the surface pass
	// draws only the opaque head, so an opaque marker would appear across the
	// glass inside every other mirror in the scene. It casts nothing for the
	// matching reason — a bar on the floor describes the scene it is meant to be
	// describing.
	CHECK(marker.Transparency > 0.0f);
	CHECK(marker.Surface == -1);
	CHECK_FALSE(marker.CastShadow);
}

TEST_CASE("a camera with no face gets no marker", "[scene][surfacecameras]") {
	Mirror mirror;
	mirror.World.SetParent(mirror.Reflection, engine::ecs::NULL_ENTITY);

	// The same answer `AimSurfaceCameras` gives for the same arrangement, and it
	// has to be: a marker drawn for a camera that is not projecting off anything
	// would be describing a face that does not exist.
	std::vector<engine::scene::DrawInstance> list;
	CHECK(engine::scene::AppendSurfaceFaceMarkers(mirror.World, list) == 0);
	CHECK(list.empty());
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

TEST_CASE("the reflection follows the eye that is live when it is aimed", "[scene][surfacecameras]") {
	// **The contract a caller has to order itself around, and the one that had
	// no test.** This pass reads `ActiveCamera` at the moment it runs, so a host
	// that moves its eye *after* aiming gets a mirror reflecting where the eye
	// used to be. `mono.studio` did exactly that: `EnsureViewerCamera` wrote the
	// viewport's eye after `Universe::Present`, which is the phase
	// `aim-surface-cameras` runs in.
	//
	// With one viewport that is a reflection one frame stale. **With two it is
	// the other viewport's camera** — each panel writes the same `ActiveCamera`
	// in turn, so the last to run decides what every mirror reflects, and a
	// mirror in one panel tracks a camera being flown in the other.
	//
	// The pass is right and the caller was wrong, so what is pinned here is the
	// pass's half: aim, move, aim again, and the answer moves with the eye.
	Mirror mirror;

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// The face is at z = -0.2 and the eye at z = 20, so the reflection lands at
	// 2 * -0.2 - 20.
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));

	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 8.0f});

	// **Still the old answer**, because nothing has re-aimed. This is the state
	// a caller leaves itself in by writing the eye too late, and it is worth
	// asserting rather than assuming: it is what makes the stale reflection a
	// property of the ordering rather than of the arithmetic.
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-8.4f, TOLERANCE));

	// And a second eye entirely, which is the two-viewport shape: whichever
	// entity `ActiveCamera` names when this runs is the one every mirror in the
	// world reflects through. There is one reflection per surface, not one per
	// viewer, so a host showing a world twice has to aim between the two draws.
	const Entity second =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Camera")), "Second");
	mirror.World.Set<Transform>(second, Transform{CFrame(Vector3{0.0f, 0.0f, 40.0f})});
	mirror.World.SetResource(ActiveCamera{second, 16.0f / 9.0f});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-40.4f, TOLERANCE));
}
