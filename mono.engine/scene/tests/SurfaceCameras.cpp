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
using engine::scene::SurfaceLens;
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

TEST_CASE("the clip plane is the pane itself, not a near plane pushed out to it", "[scene][surfacecameras]") {
	// **This case used to assert the approximation, and now asserts the thing.**
	//
	// The reflected camera is behind the pane looking through it, so everything
	// between the two — the frame, the back of the pane, whatever the viewer
	// stands behind — would occlude the reflection. That used to be handled by
	// shoving the *near plane* out to `|distance| + 0.3`, which clips at a plane
	// parallel to the face rather than on it: correct looking straight at the
	// glass, and over-clipping at a grazing angle.
	//
	// A real oblique clip skews the projection's near plane onto the pane's own
	// plane, so the near distance is free again and is whatever the author set.
	// Asserting the old 20.5 here would be asserting that the approximation is
	// still in place.
	Mirror mirror;

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(fitted != nullptr);

	// The pane faces -Z with its face at z = -0.2, and the eye is at z = +20 —
	// so it is being looked at *from behind*, the reflected camera lands at
	// z = -20.4, and it looks back along **+Z** through the glass. The plane to
	// keep the far side of is therefore `z >= -0.2`, which is the eye's side.
	CHECK_THAT(fitted->ClipNormal.X, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(fitted->ClipNormal.Y, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(fitted->ClipNormal.Z, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));
	CHECK_THAT(fitted->ClipDistance, Catch::Matchers::WithinAbs(-0.2f, TOLERANCE));

	// **Left alone**, which is the change. The engine used to overwrite this
	// every frame and a script that set it had it taken back.
	const Camera *lens = mirror.World.Get<Camera>(mirror.Reflection);
	REQUIRE(lens != nullptr);
	CHECK_THAT(lens->NearPlane, Catch::Matchers::WithinAbs(Camera{}.NearPlane, TOLERANCE));
	CHECK_THAT(fitted->NearPlane, Catch::Matchers::WithinAbs(Camera{}.NearPlane, TOLERANCE));

	// And the skew actually lands on that plane: a point just behind the pane is
	// clipped, one just in front of it survives. That is the property the whole
	// oblique construction exists for, and it is what the parallel-plane
	// approximation could only manage head-on.
	const Transform *placed = mirror.World.Get<Transform>(mirror.Reflection);
	REQUIRE(placed != nullptr);

	const glm::mat4 viewProjection =
		engine::scene::ResolveSurfaceCamera(
			placed->Frame, engine::scene::SurfaceProjection(*fitted, placed->Frame)
		)
			.ViewProjection;

	// The camera sits at z = -20.4 looking towards +Z, so a point at z = -4 is
	// *between* it and the glass — the back of the wall, in a real scene — and
	// one at z = 0.5 is beyond the glass in the room being reflected.
	const glm::vec4 occluding = viewProjection * glm::vec4(0.0f, 0.0f, -4.0f, 1.0f);
	const glm::vec4 reflected = viewProjection * glm::vec4(0.0f, 0.0f, 0.5f, 1.0f);

	// Depth is `0..1` here, so clipped by the near plane is a negative z. **This
	// is the assertion the parallel-plane approximation could not make**: it put
	// the near plane at a fixed distance and happened to catch this point, where
	// the skew catches everything on that side of the pane at any angle.
	CHECK(occluding.z < 0.0f);
	CHECK(reflected.z > 0.0f);
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
		const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
		REQUIRE(placed != nullptr);
		REQUIRE(fitted != nullptr);

		// **No aspect ratio.** The fit is to the pane's corners, so the
		// texture's shape is already inside the extents — which is exactly why
		// the renderer stopped passing one too.
		const glm::mat4 viewProjection =
			engine::scene::ResolveSurfaceCamera(
				placed->Frame, engine::scene::SurfaceProjection(*fitted, placed->Frame)
			)
				.ViewProjection;

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

	// The frustum narrows rather than staying wide. Measured as a span at the
	// near plane instead of an angle, because the fit no longer produces one.
	const SurfaceLens *distant = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(distant != nullptr);
	CHECK((distant->Right - distant->Left) / distant->NearPlane < 0.2f);
}

TEST_CASE(
	"the frustum leans instead of widening when the viewer is off to one side", "[scene][surfacecameras]"
) {
	// **What the off-axis fit buys, and it is texels rather than correctness.**
	// A symmetric frustum has to reach the far edge of the pane on *both* sides,
	// so a viewer standing off-centre pays for twice the width they can see and
	// the mirror is drawn at half the resolution it could be. An off-axis one
	// puts the same four corners on the same texture with the frustum tilted.
	//
	// `SurfaceCameras.hpp` named this as the change it was waiting for, and the
	// assertion is the definition of asymmetry: the two horizontal edges stop
	// being mirror images about zero.
	Mirror mirror(NormalId::Front);

	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{9.0f, 0.0f, 6.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(fitted != nullptr);

	const float middle = (fitted->Left + fitted->Right) * 0.5f;
	const float span = fitted->Right - fitted->Left;

	INFO("left " << fitted->Left << " right " << fitted->Right);
	CHECK(std::abs(middle) > span * 0.05f);

	// Square on, it is symmetric again — so the lean is a response to where the
	// viewer is and not a constant skew nobody asked for.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 6.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	const SurfaceLens *square = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(square != nullptr);
	CHECK_THAT(square->Left + square->Right, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
}

TEST_CASE("a pane in the plane of the viewer draws nothing", "[scene][surfacecameras]") {
	// **D00027's closure, and it replaces what this case used to assert.** It
	// used to put the viewer exactly in the glass and require a *clamped* frustum
	// — a reflection covering half a turn, which no projection covers and which
	// `tan` answers with infinity. Clamping produced a finite matrix for a view
	// nobody can see: a pane edge-on subtends no pixels.
	//
	// So the answer is now that there is no reflection to draw, which is what
	// removes the flash rather than bounding it — see `EDGE_ON_MARGIN`.
	Mirror mirror;
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});

	CHECK(AimSurfaceCameras(mirror.World) == 0);

	// **The pane has to be told, not merely the camera.** A surface left holding
	// its slot goes on sampling whatever was last rendered into it, which is a
	// frozen reflection rather than a blank one — worse than the bug, because it
	// is a picture of somewhere the viewer is no longer standing.
	const Visual *pane = mirror.World.Get<Visual>(mirror.Pane);
	REQUIRE(pane != nullptr);
	CHECK(pane->Surface == -1);
}

TEST_CASE("a pane just clear of the plane still fits a finite frustum", "[scene][surfacecameras]") {
	// The other side of `EDGE_ON_MARGIN`, so the clamp that case used to test is
	// still covered where it still applies. Just outside the band the viewer is
	// nearly level with the pane, the subtended angle is enormous, and the fit
	// has to stay finite rather than spreading infinities into every bound
	// derived from it.
	Mirror mirror;
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, -0.55f});

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

	// **The eye is moved in front of the turned pane, which the original of this
	// case did not do.** It left the viewer at +Z, and the rotated normal is -X —
	// so the eye was exactly *level* with the plane, the mirrored position was
	// the eye itself, and the assertion "Z did not move" passed for a reflection
	// that was never computed. Since `EDGE_ON_MARGIN` that arrangement draws
	// nothing, which is what turned this into a failure and showed up the
	// fixture.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{-20.0f, 0.0f, 0.0f});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// The face is at x = -0.2 and the eye 19.8 in front of it, so the reflection
	// lands the same distance behind: -20 + 2 × 19.8. **The real assertion is
	// still that nothing moved along Z** — a version reflecting through the world
	// axis rather than the part's own would have swung the camera along it.
	CHECK_THAT(mirror.Placed().X, Catch::Matchers::WithinAbs(19.6f, TOLERANCE));
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
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

TEST_CASE("orbiting the eye does not step the fitted frustum", "[scene][surfacecameras]") {
	// **The mirror flashing, as an assertion.**
	//
	// `FitFieldOfView` used to bail to `FIT_MAXIMUM` the instant one corner of
	// the pane reached zero depth from the reflected camera. That makes the fit
	// a *step function*: orbiting the viewer at a constant distance sweeps the
	// reflected camera toward the pane's plane, a corner crosses the threshold,
	// and the field of view jumps from a fitted half-radian to 172° between one
	// frame and the next — then back on the frame after. The projection was
	// never wrong. The fit was discontinuous, and a discontinuity once per orbit
	// is exactly what a flash is.
	//
	// **Tested as a bound on the frame-to-frame *change*, not on the value.**
	// The value is allowed to grow enormously — a corner going edge-on genuinely
	// needs the widest frustum there is — and asserting a ceiling would forbid
	// the correct answer. What must not happen is arriving there in one step.
	//
	// The orbit is fine-grained on purpose: at a degree per sample the honest
	// change between neighbours is small everywhere, so a single large jump is
	// unambiguous rather than a matter of how coarsely it was sampled.
	Mirror mirror(NormalId::Front);

	constexpr float RADIUS = 14.0f;
	constexpr int SAMPLES = 360;

	float previous = 0.0f;
	float worst = 0.0f;
	float worstAt = 0.0f;
	Vector3 previousLook;
	float worstTurn = 0.0f;
	float worstTurnAt = 0.0f;

	// Whether the previous sample drew anything, so a blank frame breaks the
	// chain instead of being compared across.
	bool hasPrevious = false;
	int blank = 0;

	for (int step = 0; step <= SAMPLES; step++) {
		const float angle = static_cast<float>(step) * (6.2831853f / static_cast<float>(SAMPLES));

		// A constant distance from the origin, which is what the pane is
		// centred on — the motion the flash was reported under.
		mirror.World.Set<Transform>(
			mirror.Eye, Transform{CFrame(Vector3{std::sin(angle) * RADIUS, 0.0f, std::cos(angle) * RADIUS})}
		);

		// **Continuity is only asked of frames that draw**, which is the whole of
		// D00027's fix expressed as a measurement. Crossing the plane, the pane
		// goes blank for a few samples and comes back aimed the other way — the
		// two orientations are never in consecutive *visible* frames, so there is
		// no flash. Comparing across the gap would be asserting continuity of a
		// picture nobody was shown.
		if (AimSurfaceCameras(mirror.World) == 0) {
			blank++;
			hasPrevious = false;
			continue;
		}

		// **The angle the fitted frustum spans, and no longer a field of view.**
		// The fit stopped producing an angle when it became off-axis, so reading
		// `Camera::FieldOfViewRadians` here would read a field nothing writes
		// any more — a constant, which passes this case without looking at
		// anything. It did, until this was changed.
		//
		// **An angle rather than the raw extents, and that is not cosmetic.**
		// `Right - Left` is a *tangent*: as the reflected camera approaches the
		// pane's plane the pane subtends most of a half-turn and the span grows
		// without bound, so a smooth sweep near the crossing shows enormous
		// steps in it — this case measured exactly that and reported 13 at the
		// edge of the blank band. The angle is what the old field of view stood
		// for, is bounded by π, and is what a viewer would perceive stepping.
		const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
		REQUIRE(fitted != nullptr);

		const float fov =
			std::atan(fitted->Right / fitted->NearPlane) - std::atan(fitted->Left / fitted->NearPlane);
		REQUIRE(std::isfinite(fov));

		// **The look direction as well as the field of view**, because the two
		// discontinuities this pass can have are different things: the fit
		// stepping, and the camera *turning round*. `facing` flips sign when the
		// viewer crosses the pane's plane, and orbiting a pane centred on the
		// origin crosses it twice a lap — so the reflected camera whips 180° and
		// the frame after it is unrelated to the frame before.
		const Vector3 look = mirror.World.Get<Transform>(mirror.Reflection)->Frame.LookVector();
		if (hasPrevious) {
			const float change = std::abs(fov - previous);
			if (change > worst) {
				worst = change;
				worstAt = angle;
			}

			const float turned = (look - previousLook).Magnitude();
			if (turned > worstTurn) {
				worstTurn = turned;
				worstTurnAt = angle;
			}
		}
		previous = fov;
		previousLook = look;
		hasPrevious = true;
	}

	INFO("worst single-step change " << worst << " radians of frustum angle, at " << worstAt << " radians");

	// **A degree of orbit must not jump the frustum.** The old fit's step was the
	// whole distance between a fitted angle and its 3.0-radian ceiling, taken in
	// one sample, because the ceiling made it a step function. Nothing clamps
	// now, so there is no edge to step across — but the assertion is kept, and
	// kept as a bound on the *change* rather than on the value, because the
	// depth floor is still a place a discontinuity could hide.
	CHECK(worst < 0.25f);

	// **And here is the flash, fixed rather than measured.** This bound used to
	// be `<= 2.001` — asserting the bug, because a look vector changing by
	// exactly 2.0 is a 180 degree turn in one frame, once a lap, where `facing`
	// flips sign as the viewer crosses the pane's plane.
	//
	// It is now *zero* across every visible frame, and that falls out of the fix
	// rather than being tuned to: within one side of the plane `facing` is
	// constant, so the reflected camera's orientation does not change at all as
	// the viewer orbits — only its position does. The band where the sign would
	// have flipped draws nothing.
	//
	// **Skipping the crossing without blanking it does not work and was tried.**
	// Holding the camera's last transform through the band moves the flip a frame
	// later; the discontinuity belongs to the sign, not to when it is evaluated.
	// What removes it is that no frame in between is shown.
	INFO("worst single-step turn " << worstTurn << " at " << worstTurnAt << " radians");
	CHECK(worstTurn < 0.01f);

	// **The band was actually entered, which the bound above cannot say.** With
	// no blank samples this test would pass by having quietly stopped crossing
	// the plane at all — so the fix has to be shown doing something, not merely
	// failing to do the wrong thing. Twice a lap, a handful of samples each.
	INFO("blank samples " << blank << " of " << SAMPLES);
	CHECK(blank > 0);
	CHECK(blank < SAMPLES / 4);
}

TEST_CASE("a portal stands at the far pane and looks out of it", "[scene][surfacecameras]") {
	// **Worked through by hand, not read off the run**, for the reason the
	// mirror placement case gives: a number taken from what the code printed
	// would agree with the code about a mistake they were both making.
	//
	// Pane A is at the origin with a half extent of 0.2 along Z and its `Front`
	// face, so that face is at `z = -0.2` with an outward normal of `-Z`. The eye
	// is at `z = 20`, which is on the *other* side — so `facing` is -1 and the
	// direction out of the pane towards the viewer is `+Z`. The source frame
	// looks along that.
	//
	// Pane B is a hundred units along X, unrotated and the same size, so its
	// `Front` face is at `(100, 0, -0.2)` with an outward normal of `-Z` too.
	//
	// `destination · half-turn · source⁻¹` takes the eye's 20.2 units of
	// clearance and puts it on the far side of B: `(100, 0, -0.2)` plus 20.2
	// along `+Z`, which is `(100, 0, 20)`. The camera then looks back through B
	// along `-Z`, into the room B faces.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	const Vector3 placed = mirror.Placed();
	CHECK_THAT(placed.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(placed.Y, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(placed.Z, Catch::Matchers::WithinAbs(20.0f, TOLERANCE));

	// **Looking back through the far pane, not away from it.** The camera lands
	// on the same side of B as the eye was of A, because the map is rigid — so
	// it has to turn round to see through the hole. Pointing it the other way
	// gives a portal that works and leads somewhere wrong.
	const CFrame &frame = mirror.World.Get<Transform>(mirror.Reflection)->Frame;
	CHECK_THAT(frame.LookVector().Z, Catch::Matchers::WithinAbs(-1.0f, 1e-3f));

	// And the oblique clip is at the far pane's plane, keeping the room B faces
	// and dropping the wall B is set into. Without this the hole shows the back
	// of that wall, which is the failure the parallel-plane approximation never
	// revealed on a mirror.
	const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(fitted != nullptr);
	CHECK_THAT(fitted->ClipNormal.Z, Catch::Matchers::WithinAbs(-1.0f, 1e-3f));
	CHECK_THAT(fitted->ClipDistance, Catch::Matchers::WithinAbs(0.2f, 1e-3f));
}

TEST_CASE("a portal with no destination is a mirror", "[scene][surfacecameras]") {
	// **The fallback is a mirror rather than a blank**, so a portal whose target
	// was deleted reads as a surface that stopped working rather than as a pane
	// that vanished. Same answer for a destination never set.
	Mirror mirror;
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));

	// A destination that existed and was destroyed takes the same path, which is
	// the case a scene actually hits.
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	REQUIRE_THAT(mirror.Placed().X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));

	mirror.World.Destroy(far);

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));
}

TEST_CASE("a portal pair need not describe one space", "[scene][surfacecameras]") {
	// **The non-Euclidean claim, as an assertion.** Nothing constrains the two
	// frames to be consistent: turning the destination turns what comes out of
	// the hole, which is how a corridor gets to bend more than the room it is in
	// allows. `docs/NON-EUCLIDEAN.md` is the investigation, and this is the one
	// line of it that the engine actually had to gain.
	//
	// A destination yawed by 90° maps the eye's 20.2 units of clearance along
	// the far pane's rotated normal instead of along `+Z`, so the camera lands
	// on a different axis entirely — with no separate feature and no maths past
	// the matrix multiply.
	Mirror straight;
	Mirror turned;

	for (Mirror *mirror : {&straight, &turned}) {
		const Entity far =
			mirror->World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");

		const CFrame frame =
			mirror == &turned ? CFrame(Vector3{100.0f, 0.0f, 0.0f}) * CFrame::Angles(0.0f, 1.5707963f, 0.0f)
							  : CFrame(Vector3{100.0f, 0.0f, 0.0f});

		mirror->World.Set<Transform>(far, Transform{frame});
		mirror->World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
		mirror->World.Set<engine::scene::Portal>(mirror->Reflection, engine::scene::Portal{far});

		REQUIRE(AimSurfaceCameras(mirror->World) == 1);
	}

	// Square on, the clearance runs along Z. Yawed a quarter turn, it runs along
	// X — the same portal, the same eye, a different space on the other side.
	CHECK_THAT(straight.Placed().Z, Catch::Matchers::WithinAbs(20.0f, TOLERANCE));
	CHECK_THAT(turned.Placed().Z, Catch::Matchers::WithinAbs(0.0f, 1e-3f));
	CHECK(std::abs(turned.Placed().X - 100.0f) > 15.0f);
}

TEST_CASE("a body that walks into a portal comes out of the far one", "[scene][surfacecameras]") {
	// **`D00112`, and what it was waiting for was a body.** A portal has drawn
	// correctly since v0.14 and nothing could go through one, because nothing in
	// this engine had a body to move. The cases below are the four things that
	// are silently wrong if the traversal is written by eye:
	//
	//   * the body arrives at the *far* pane rather than at the near one;
	//   * its velocity is turned with it, or it walks out sideways;
	//   * a body that passes beside the hole is not swallowed by its plane;
	//   * a body crossing the other way is not sent back where it came from.
	//
	// The geometry is the file's own: pane A at the origin with its `Front`
	// face at `z = -0.2` and an outward normal of `-Z`; pane B a hundred units
	// along X, unrotated, so its front face is at `(100, 0, -0.2)` and its
	// normal is `-Z` too.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// A body walking along -Z, from in front of pane A to behind it. The
	// previous transform is where the tick started and the current one is where
	// the solver left it, which is what `CrossPortals` reads.
	const Entity walker =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
	mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	// **Out of the far pane, into the space the hole was showing.** The camera
	// case above puts A's camera at `(100, 0, 20)` looking back through B along
	// `-Z`, so what A shows is the room in front of B's *front* face — and a
	// body that walks into A has to arrive in the room it was looking at. One
	// metre of clearance in front of A becomes one metre in front of B.
	const Vector3 landed = mirror.World.Get<Transform>(walker)->Frame.Position;
	CHECK_THAT(landed.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(landed.Z, Catch::Matchers::WithinAbs(-1.0f, TOLERANCE));

	// **And still walking away from the pane it came out of**, at the speed it
	// had. Forgetting to map the velocity is the bug that reads as the portal
	// spitting people back: the body would arrive aimed the way it was aimed in
	// the frame it left, which here is straight back into B.
	const Vector3 speed = mirror.World.Get<engine::scene::Motion>(walker)->Linear;
	CHECK_THAT(speed.Z, Catch::Matchers::WithinAbs(-16.0f, TOLERANCE));
	CHECK_THAT(speed.Magnitude(), Catch::Matchers::WithinAbs(16.0f, TOLERANCE));

	// **Once, not once per tick.** The body is now behind B's plane and in front
	// of nothing, so a second pass moves nobody — a crossing is a change of
	// side, not a place.
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
}

TEST_CASE("a portal swallows only what goes through the hole", "[scene][surfacecameras]") {
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// **Past the edge of the pane, and the pane is eight wide.** A test that
	// only compared sides of the plane would teleport this one, because a plane
	// is infinite and a hole is not — which is a wall you fall through fifty
	// metres from the door.
	const Entity beside =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Beside");
	mirror.World.Set<Transform>(beside, Transform{CFrame(Vector3{40.0f, 0.0f, -1.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		beside, engine::scene::PreviousTransform{CFrame(Vector3{40.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(beside, engine::scene::Motion{});

	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
	CHECK_THAT(
		mirror.World.Get<Transform>(beside)->Frame.Position.X, Catch::Matchers::WithinAbs(40.0f, TOLERANCE)
	);

	// **And a body crossing the other way goes through too**, which is the one
	// case worth stating out loud because the render half is not symmetric: a
	// surface camera is placed for whichever side the *viewer* is on, and the
	// same question about the *crosser* has the same two answers. A pane is a
	// hole rather than a one-way door, so entering from behind maps through the
	// frame that faces backwards and lands on the far side just the same.
	const Entity backwards =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Backwards");
	mirror.World.Set<Transform>(backwards, Transform{CFrame(Vector3{0.0f, 0.0f, 1.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		backwards, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, -1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(backwards, engine::scene::Motion{});

	CHECK(engine::scene::CrossPortals(mirror.World) == 1);
	CHECK_THAT(
		mirror.World.Get<Transform>(backwards)->Frame.Position.X,
		Catch::Matchers::WithinAbs(100.0f, TOLERANCE)
	);

	// An unlinked portal is a mirror, and a mirror is a wall. Nothing goes
	// through it however squarely it is hit.
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{});
	const Entity straight =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Straight");
	mirror.World.Set<Transform>(straight, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		straight, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(straight, engine::scene::Motion{});

	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
}

TEST_CASE("a portal's pane stops solving contacts, so a body can be in it", "[scene][surfacecameras]") {
	// **The wall in front of the feature.** Traversal has been implemented and
	// tested since v0.14 and no character could reach it: a pane is an ordinary
	// `Part`, an ordinary part collides, and the solver parked anybody who
	// walked at a portal on its surface. `CrossPortals` tests whether the
	// segment a body covered changes sign through the plane — a body stopped
	// *on* the plane never changes sign, so the hole was a painting.
	//
	// What it should be is the frame everybody screenshots: the body straddling
	// the plane, half in each space.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

	// The pane collides, which is what a part built by `MakePart` does.
	mirror.World.Set<engine::scene::Collider>(
		mirror.Pane, engine::scene::Collider{Vector3{8.0f, 4.5f, 0.2f}}
	);
	REQUIRE_FALSE(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);

	// Not a portal yet, so it is a mirror — and a mirror is a wall. Nothing is
	// opened, which is the half that keeps this from being "make every pane
	// passable".
	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
	CHECK_FALSE(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);

	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	CHECK(engine::scene::OpenPortals(mirror.World) == 1);

	// **A trigger and not a removed collider.** Contacts are still reported, so
	// a script can tell somebody is in the hole, and `physics::GroundCharacters`
	// still gets an answer out of `Raycast` — a pane that stopped answering
	// queries is a portal you fall through the floor beside.
	const engine::scene::Collider *opened = mirror.World.Get<engine::scene::Collider>(mirror.Pane);
	REQUIRE(opened != nullptr);
	CHECK(opened->Trigger);
	CHECK(opened->Extent.X == 8.0f);

	// Idempotent: every tick after the first writes nothing at all.
	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
}

TEST_CASE("a pane already authored passable is left alone", "[scene][surfacecameras]") {
	// A scene that set `CanCollide = false` on its pane has already said what
	// this pass says, and writing over it would stamp the row every tick for the
	// life of the world — which is what `SyncBroadphase` reads to decide the
	// static index needs rebuilding.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	engine::scene::Collider passable{Vector3{8.0f, 4.5f, 0.2f}};
	passable.Trigger = true;
	mirror.World.Set<engine::scene::Collider>(mirror.Pane, passable);

	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
	CHECK(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);
}
