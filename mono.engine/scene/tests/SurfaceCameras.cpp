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
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.scene.surfacecameras")

using engine::core::CFrame;
using engine::core::Vector2;
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
	// angle: every part of the pane **that the viewer can see** projects inside
	// the image the camera renders.
	//
	// **"That the viewer can see" is the correction, and it is the whole of the
	// close-up sharpness.** The invariant used to be every *corner* of the pane,
	// which is stronger than the shader needs and ruinously expensive up close:
	// a pane subtends nearly half a turn from a point on its own surface while a
	// screen subtends seventy degrees, so covering the corners means spending
	// almost every texel outside the frame and the image goes blocky exactly
	// when it is largest. The fit is intersected with the viewer's own frustum
	// now, and what has to hold is that nothing on screen falls outside the
	// texture — which is precisely the condition `opaque.frag` falls back on.
	Mirror mirror;

	// Half extents 8 by 4.5 on a pane facing -Z, so the face is at z = -0.2.
	// Sampled over a grid rather than at the corners, because the corners are no
	// longer the extreme case: with the fit clamped to the screen, the point
	// that fails first is wherever the two boundaries cross.
	const auto paneIsCovered = [&]() {
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

		// What the screen itself makes of the pane, which is what decides which
		// points have to be covered at all.
		const Transform *eyePlaced = mirror.World.Get<Transform>(mirror.Eye);
		const Camera *eyeLens = mirror.World.Get<Camera>(mirror.Eye);
		REQUIRE(eyePlaced != nullptr);
		REQUIRE(eyeLens != nullptr);

		const glm::mat4 screen =
			engine::scene::ResolveCamera(eyePlaced->Frame, *eyeLens, 16.0f / 9.0f).ViewProjection;

		bool covered = true;
		constexpr int STEPS = 16;

		for (int ix = 0; ix <= STEPS; ix++) {
			for (int iy = 0; iy <= STEPS; iy++) {
				const float x = -8.0f + 16.0f * static_cast<float>(ix) / STEPS;
				const float y = -4.5f + 9.0f * static_cast<float>(iy) / STEPS;
				const glm::vec4 point(x, y, -0.2f, 1.0f);

				// Off screen is not this camera's problem. A fit that covered
				// the whole pane would pass this too — it is a weaker condition,
				// and deliberately: what the shader needs is nothing on screen
				// falling outside the image.
				const glm::vec4 onScreen = screen * point;
				if (!(onScreen.w > 0.0f) || std::abs(onScreen.x / onScreen.w) > 1.0f ||
					std::abs(onScreen.y / onScreen.w) > 1.0f) {
					continue;
				}

				const glm::vec4 clip = viewProjection * point;
				INFO("pane point " << x << ", " << y << " has w " << clip.w);

				// Behind the camera is not covered, and saying so beats a divide
				// that flips the sign and reports the point as central.
				if (!(clip.w > 0.0f)) {
					covered = false;
					continue;
				}

				covered = covered && std::abs(clip.x / clip.w) <= 1.0f && std::abs(clip.y / clip.w) <= 1.0f;
			}
		}
		return covered;
	};

	// The strict form, which still has to hold whenever the pane fits on the
	// screen. **This is the guard that the clamp does not bite when it should
	// not**: at any ordinary distance the fit is already far tighter than the
	// viewer's own frustum, so intersecting with it must change nothing at all
	// — and a clamp that quietly cropped a mirror seen from across a room would
	// look exactly like the bug it was written to fix.
	const auto cornersAreCovered = [&]() {
		const Transform *placed = mirror.World.Get<Transform>(mirror.Reflection);
		const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
		REQUIRE(placed != nullptr);
		REQUIRE(fitted != nullptr);

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
	CHECK(paneIsCovered());
	CHECK(cornersAreCovered());

	// **Two units, which it did not.** The pane needs about 150 degrees from
	// here and had 70, so the corners fell outside the texture and the wall drew
	// its own grey around a rectangle of reflection.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 2.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(paneIsCovered());

	// And off to one side as well as close, which is the case a symmetric
	// frustum has to widen for rather than shift.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{6.0f, 3.0f, 3.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(paneIsCovered());

	// **Far enough away that the fit narrows rather than widens**, which is the
	// half a "make it wider" fix would pass without doing: a distant pane gets a
	// frustum tight around it, and the texels go on the mirror instead of on the
	// room around it.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 200.0f});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(paneIsCovered());
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
	// is at `z = 20`, which is the pane's **back** side.
	//
	// Pane B is a hundred units along X, unrotated and the same size, so its
	// `Front` face is at `(100, 0, -0.2)` with an outward normal of `-Z` too.
	//
	// **The numbers below moved once, and the old ones were the bug.** This case
	// used to build its expectation from a source frame that *flipped with the
	// viewer's side* — "`facing` is -1, so the source looks along `+Z`" — which
	// is two maps for one pane and is exactly what `SeamMapping` stopped doing at
	// v0.15, because two maps that are not each other's inverse send a body that
	// walks in the back of a hole to where one that walked in the front goes.
	// `AimSurfaceCameras` was left composing the old one, and a **cross-world**
	// pane is the only thing still drawn from it — so the immersive scene, which
	// spawns you behind its pane, got a camera forty studs from where a body
	// crossing lands and showed the half of the far room with nothing in it.
	//
	// So: the map is `destination · half-turn · source⁻¹` with `source` built
	// from **the pane's own normal**. It carries A's back hemisphere to B's
	// front, so an eye 20.2 behind A's face lands 20.2 in front of B's — at
	// `(100, 0, -20.4)` — and turns to look back through B along `+Z`.
	//
	// **Which is the half a traveller arrives in, and that is the check that
	// matters.** Looking through A from behind it is looking into A's *front*
	// hemisphere, and A's front is identified with B's *back*. A body that walks
	// through ends in B's back. The picture and the crossing therefore agree,
	// which is the whole property, and the assertion below states it against
	// `SeamMapping` rather than against a hand-worked coordinate.
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
	CHECK_THAT(placed.Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));

	// **Where the seam's own map says, and not a second derivation of it.** This
	// is the assertion the old numbers could not make: one statement of what a
	// hole does, checked against the pass that draws the picture.
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);

	const Vector3 mapped =
		engine::scene::SeamMapping(seams[0]).Point(mirror.World.Get<Transform>(mirror.Eye)->Frame.Position);
	CHECK_THAT(placed.X, Catch::Matchers::WithinAbs(mapped.X, 1e-2f));
	CHECK_THAT(placed.Z, Catch::Matchers::WithinAbs(mapped.Z, 1e-2f));

	// **Looking back through the far pane, not away from it.** The camera lands
	// on the far side of B from the room it has to show, so it has to turn round
	// to see through the hole. Pointing it the other way gives a portal that
	// works and leads somewhere wrong.
	const CFrame &frame = mirror.World.Get<Transform>(mirror.Reflection)->Frame;
	CHECK_THAT(frame.LookVector().Z, Catch::Matchers::WithinAbs(1.0f, 1e-3f));

	// And the oblique clip is at the far pane's plane, keeping the half a
	// traveller arrives in and dropping the wall B is set into. Without this the
	// hole shows the back of that wall, which is the failure the parallel-plane
	// approximation never revealed on a mirror.
	const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(fitted != nullptr);
	CHECK_THAT(fitted->ClipNormal.Z, Catch::Matchers::WithinAbs(1.0f, 1e-3f));

	// **The kept half-space is the one a crossing body lands in**, which is the
	// invariant rather than the coordinate: `examples/tests/Scene.cpp` states the
	// same thing about a room's middle, and this states it about the one point
	// the engine can derive for itself.
	const Vector3 landed = engine::scene::SeamMapping(seams[0]).Point(Vector3{0.0f, 0.0f, -2.0f});
	CHECK(landed.Dot(fitted->ClipNormal) >= fitted->ClipDistance);
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
	// allows. `NON-EUCLIDEAN.md` is the investigation, and this is the one
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
	//
	// The straight number is `-20.4` and not `20` for the reason the case above
	// records at length: the map is built from the pane's own normal now, so an
	// eye behind A lands in front of B rather than behind it.
	CHECK_THAT(straight.Placed().Z, Catch::Matchers::WithinAbs(-20.4f, TOLERANCE));
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

	// **Out of the far pane, into the space the hole was showing.** The map
	// carries a pane's front hemisphere to the far pane's *back* one, which is
	// CodeParade's `Connect(a->front, b->back)` written as a matrix — so a body
	// that ends its step eight tenths in front of A arrives eight tenths behind
	// B, and the sub-camera the pass builds for the same eye is placed by the
	// same product and looks at exactly that spot.
	//
	// **Exactly where the step ended, mapped — the clearance is a floor and not
	// an offset.** A crosser whose step already finished well past the plane
	// gets nothing added, which is what stops a round trip landing beside where
	// it started. See `LANDING_CLEARANCE`, and the case below for the step that
	// does need it.
	const Vector3 landed = mirror.World.Get<Transform>(walker)->Frame.Position;
	CHECK_THAT(landed.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(landed.Z, Catch::Matchers::WithinAbs(0.6f, TOLERANCE));

	// **And clear of the plane rather than merely past it**, which is the
	// property the number is for: B's face is at `z = -0.2`, the body is beyond
	// it, and the gap is the clearance and not a rounding error.
	CHECK(landed.Z > -0.2f + 0.005f);

	// **And still walking away from the pane it came out of**, at the speed it
	// had. Forgetting to map the velocity is the bug that reads as the portal
	// spitting people back: the body would arrive aimed the way it was aimed in
	// the frame it left, which here is straight back into B.
	const Vector3 speed = mirror.World.Get<engine::scene::Motion>(walker)->Linear;
	CHECK_THAT(speed.Z, Catch::Matchers::WithinAbs(16.0f, TOLERANCE));
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

TEST_CASE("a body standing in a portal is cut and drawn on the far side", "[scene][surfacecameras]") {
	// **Half a character is what a hole without this looks like, and two whole
	// characters is what the first attempt at it looked like.** A pane is
	// passable — the case below is what makes it so — so a body may straddle
	// one, and when it does it is one set of parts in one place: whole in the
	// room it came from and absent from the room it is walking into.
	//
	// Appending a copy answers that and opens the other half of it: both copies
	// are then drawn *whole*, so the original hangs out of the back of the pane
	// and the copy hangs out of the far pane. A thick wall hides both, which is
	// why it survived; a free-standing pane shows two crates in a doorway.
	//
	// Five things are silently wrong if this is written by eye, and this covers
	// all of them:
	//
	//   * a body clear of the pane is not copied at all;
	//   * a body in the pane is copied onto the *far* pane, turned;
	//   * both halves carry complementary planes, so their union is one body;
	//   * a pane is never copied through itself, or a portal recurses into it;
	//   * a cross-world pane copies nothing here, because its `Destination` is a
	//     camera stand-in in this world rather than a place.
	//
	// The geometry is this file's: pane A at the origin with its `Front` face at
	// `z = -0.2`, pane B a hundred units along X.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// **A draw list rather than entities, because this pass reads one.** Only a
	// list walk holds the row the original is in, which is what lets it cut the
	// body as well as copy it — see `CutAndCloneSeams`. A row is a frame and a
	// box, so a test builds one directly.
	const auto row = [](const Vector3 &at, const Vector3 &half, int8_t surface) {
		engine::scene::DrawInstance instance;
		instance.Frame = CFrame(at);
		instance.HalfExtent = half;
		instance.Surface = surface;
		return instance;
	};

	const Vector3 BODY{0.5f, 1.0f, 0.5f};

	// Well clear of the pane.
	std::vector<engine::scene::DrawInstance> drawn;
	drawn.push_back(row(Vector3{0.0f, 0.0f, 30.0f}, BODY, -1));

	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);
	CHECK(drawn.size() == 1);
	CHECK(drawn[0].SeamNormal == Vector3{});

	// **In the pane, which is the state a copy exists for and is not a
	// crossing.** `CrossPortals` asks whether a segment changed sides between
	// two ticks; this body has not moved at all and is standing in the hole,
	// which it can do for as long as it likes.
	//
	// The pane itself is in the list too, carrying its slot, which is how this
	// pass knows a hole from a thing in one.
	drawn.clear();
	drawn.push_back(row(Vector3{0.0f, 0.0f, -0.1f}, BODY, -1));
	drawn.push_back(row(Vector3{0.0f, 0.0f, 0.0f}, Vector3{8.0f, 4.5f, 0.2f}, 0));

	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);

	// **The pane is not among them**, though a pane straddles its own plane by
	// construction and fits its own rectangle exactly. One copy, from the one
	// body, appended at the end.
	REQUIRE(drawn.size() == 3);
	CHECK(drawn[1].SeamNormal == Vector3{});

	// **At the far pane**, which is the whole claim: the body is in pane A and
	// its copy is in pane B, a hundred units away. The same map a crossing body
	// goes through, applied to a body that has not crossed.
	//
	// **And the same depth into it, which is what makes the two halves meet.**
	// The map is a rigid isometry taking A's plane onto B's, so whatever slice of
	// the body crosses A's face is exactly the slice of the copy that crosses
	// B's: the two halves join whichever side the map lands on. Which side it
	// lands on is the map's own: a pane's front hemisphere goes to the far pane's
	// back one, so a centre a tenth *short* of A's face copies to a tenth *past*
	// B's, and the copy pokes out of B by exactly what has pushed through A.
	CHECK_THAT(drawn[2].Frame.Position.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(drawn[2].Frame.Position.Z, Catch::Matchers::WithinAbs(-0.3f, TOLERANCE));

	// Its own size and appearance, because it is the same body seen from
	// somewhere else rather than a marker standing in for one.
	CHECK(drawn[2].HalfExtent == Vector3{0.5f, 1.0f, 0.5f});

	// **Never a surface itself.** A copied pane would claim the slot its
	// original writes, and the renderer keeps the first camera to name an index
	// — so the two would fight over one texture from frame to frame.
	CHECK(drawn[2].Surface == -1);

	// **The two planes, which are the cut.** The original keeps the front of
	// pane A — its `Front` face points along -Z — and the copy keeps the front
	// of pane B. Their union is the body and their intersection is empty, which
	// is what stops the seam being two bodies.
	CHECK_THAT(drawn[0].SeamNormal.Z, Catch::Matchers::WithinAbs(-1.0f, TOLERANCE));
	CHECK_THAT(drawn[0].SeamOffset, Catch::Matchers::WithinAbs(0.2f, TOLERANCE));

	// A point a tenth in front of A's face is kept by the original and refused
	// by the copy's plane once carried through the map, and a point a tenth
	// behind it is the other way round. Stated as the two dot products rather
	// than as coordinates, because that is what the shader tests.
	const auto keptBy = [](const engine::scene::DrawInstance &half, const Vector3 &at) {
		return at.Dot(half.SeamNormal) >= half.SeamOffset;
	};

	const Vector3 nearHalf{0.0f, 0.0f, -0.3f};
	const Vector3 farHalf{0.0f, 0.0f, -0.1f};

	CHECK(keptBy(drawn[0], nearHalf));
	CHECK(!keptBy(drawn[0], farHalf));

	// The same two points carried onto the far side, where the answers swap.
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) >= 1);
	const engine::scene::SeamTransform through = engine::scene::SeamMapping(seams[0]);

	CHECK(!keptBy(drawn[2], through.Point(nearHalf)));
	CHECK(keptBy(drawn[2], through.Point(farHalf)));

	// **Nothing wider than the hole is cut**, because a single plane is exact
	// only inside the pane's rectangle and would otherwise slice a body where
	// the hole is not. A slab the size of a room straddles this pane and is left
	// alone: no copy, no cut, and the room next door keeps its own floor.
	drawn.clear();
	drawn.push_back(row(Vector3{0.0f, 0.0f, -0.1f}, Vector3{50.0f, 0.5f, 50.0f}, -1));

	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);
	CHECK(drawn.size() == 1);
	CHECK(drawn[0].SeamNormal == Vector3{});

	// **A cross-world pane copies nothing here.** `Portal::DestinationWorld`
	// makes `Destination` a stand-in that tells the camera where to look, so a
	// copy through one would appear a stand-in's distance behind the pane the
	// body is walking into rather than in the world it is walking to. The host
	// answers that half — `client::AttachForeignSurfaces` — and it is the same
	// distinction that stops `CrossPortals` moving anybody through one.
	engine::scene::Portal crossing{far};
	crossing.DestinationWorld = engine::core::Name("somewhere else");
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, crossing);

	drawn.clear();
	drawn.push_back(row(Vector3{0.0f, 0.0f, -0.1f}, BODY, -1));
	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);

	// The host asks for that one by name, and gets it — from the store, because
	// the far world's list is where it belongs and this world's row is not in
	// it. That path appends and does not cut.
	const Entity inside =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Inside");
	mirror.World.Set<Transform>(inside, Transform{CFrame(Vector3{0.0f, 0.0f, -0.1f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		inside, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, -0.1f})}
	);
	mirror.World.Set<Bounds>(inside, Bounds{BODY});
	mirror.World.Set<engine::scene::Motion>(inside, engine::scene::Motion{});
	mirror.World.Set<Visual>(inside, Visual{});
	mirror.World.Set<engine::scene::SurfaceAppearance>(inside, engine::scene::SurfaceAppearance{});
	mirror.World.Set<engine::scene::Tags>(inside, engine::scene::Tags{});
	mirror.World.Set<engine::scene::Rendered>(inside, engine::scene::Rendered{1, {}});

	std::vector<engine::scene::DrawInstance> foreign;
	CHECK(engine::scene::AppendPortalClones(mirror.World, 0, foreign) == 1);

	// **And nobody is moved through it**, which is the bug the two authorities
	// produced: the engine put the body at the stand-in and the world change put
	// it in the other world, so a body in the seam was claimed twice a tick.
	mirror.World.Set<engine::scene::PreviousTransform>(
		inside, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
	CHECK_THAT(
		mirror.World.Get<Transform>(inside)->Frame.Position.X, Catch::Matchers::WithinAbs(0.0f, TOLERANCE)
	);
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

TEST_CASE("a third-person camera goes through the hole its subject went through", "[scene][surfacecameras]") {
	// **The arm is metres long and the body is a point, which is the whole
	// bug.** A character crosses on the tick its own step changes side; the eye
	// behind it does not, so unless the arm is put through the same map the
	// frame after a crossing is the far room watched from the near one — the
	// character reads as teleporting away from the camera and turning as it
	// goes. It was reported as a portal that spits people out sideways, and it
	// is a camera that stayed behind.
	//
	// The pairing is this file's: pane A at the origin with its `Front` face at
	// `z = -0.2`, pane B a hundred along X and unrotated, so what is in front of
	// A comes out in front of B.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// The subject half a metre through the hole, walking away from it — where a
	// character is on the frame after it crossed.
	const Entity body =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Body");
	mirror.World.Set<Transform>(body, Transform{CFrame(Vector3{0.0f, 0.0f, -0.7f})});

	engine::scene::CameraController arm;
	arm.Subject = body;

	// A yaw of zero looks along -Z, so the arm reaches back along +Z — straight
	// into the pane the subject just came out of.
	arm.Angles = Vector2{0.0f, 0.0f};
	arm.Distance = 12.0f;
	mirror.World.SetResource(arm);

	REQUIRE(engine::scene::PlaceCamera(mirror.World));

	const CFrame placed = mirror.World.Get<Transform>(mirror.Eye)->Frame;

	// **Out in front of the far pane, as far from it as the arm wanted to be
	// from the near one, looking back at it.** The head is at `y = 1.5` and
	// `z = -0.7`, so the arm reaches eleven and a half metres past A's face —
	// and what is through A is what is in front of B, which is where the eye
	// comes out: `z = -0.2 - 11.5`.
	//
	// **Looking back through the hole is the point rather than a side effect.**
	// The player is looking away from the pane, the camera is behind them, and
	// behind them is the other side — so the pane fills the frame and what the
	// pane shows is the character. That is the same view they had a frame
	// before they crossed, which is what makes a crossing look like walking
	// rather than like a teleport.
	CHECK_THAT(placed.Position.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(placed.Position.Y, Catch::Matchers::WithinAbs(1.5f, TOLERANCE));
	CHECK_THAT(placed.Position.Z, Catch::Matchers::WithinAbs(-11.7f, TOLERANCE));

	CHECK_THAT(placed.LookVector().Z, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));

	// **First person has no arm and therefore no crossing.** The eye is the
	// head, the segment is a point, and a camera inside the character must not
	// be flung across the world by a pane it is standing in.
	arm.Distance = 0.0f;
	arm.Mode = engine::scene::CameraMode::LockFirstPerson;
	mirror.World.SetResource(arm);

	REQUIRE(engine::scene::PlaceCamera(mirror.World));
	CHECK_THAT(
		mirror.World.Get<Transform>(mirror.Eye)->Frame.Position.X, Catch::Matchers::WithinAbs(0.0f, TOLERANCE)
	);
}

TEST_CASE("a portal in the plane of the viewer keeps drawing", "[scene][surfacecameras]") {
	// **The band is a mirror's fix and it was being applied to holes.** Which
	// way a *reflected* camera looks depends on which side of the plane the
	// viewer is, both answers are right, and nothing joins them — so a mirror
	// blanks across `EDGE_ON_MARGIN` rather than flashing. A linked portal has
	// no such discontinuity: the frame the viewer's side flips is the frame the
	// viewer is carried through the pane, and the two cancel.
	//
	// What the band cost there is the whole of the feature. It is 0.3 metres
	// either side of the plane, which is exactly where somebody walking through
	// a hole spends the crossing — so the picture went dark on the one frame it
	// mattered.
	Mirror portal;
	const Entity far =
		portal.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	portal.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	portal.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	portal.World.Set<engine::scene::Portal>(portal.Reflection, engine::scene::Portal{far});

	// In the plane of the pane's front face, which is `z = -0.2`.
	portal.World.GetMutable<Transform>(portal.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});

	REQUIRE(AimSurfaceCameras(portal.World) == 1);

	// **And it is a real frustum, not a bounded one.** The floors that keep the
	// matrix finite at the plane — `MINIMUM_DEPTH` and `FIT_MINIMUM_SPAN` —
	// were already there for the mirror case just outside the band, and they
	// are what carries a portal across it.
	const SurfaceLens *lens = portal.World.Get<SurfaceLens>(portal.Reflection);
	REQUIRE(lens != nullptr);
	CHECK(std::isfinite(lens->Left));
	CHECK(std::isfinite(lens->Right));
	CHECK(lens->Right > lens->Left);
	CHECK(lens->Top > lens->Bottom);

	// The pane keeps its slot, which is the half a viewer actually sees: a
	// camera placed perfectly into a texture nothing samples looks exactly like
	// a portal that does not work.
	const Visual *pane = portal.World.Get<Visual>(portal.Pane);
	REQUIRE(pane != nullptr);
	CHECK(pane->Surface >= 0);

	// **And the same geometry without the link still blanks**, so this is an
	// exemption for holes rather than the band being deleted.
	portal.World.Remove<engine::scene::Portal>(portal.Reflection);
	CHECK(AimSurfaceCameras(portal.World) == 0);
}

TEST_CASE("a portal's camera moves smoothly up to its own plane", "[scene][surfacecameras]") {
	// **What the band was standing in for, asserted instead of assumed.** A
	// flash is a discontinuity, so the thing to check is not that the camera is
	// in some particular place but that it never *jumps* — and the interesting
	// stretch is the last third of a metre, which used to draw nothing at all.
	Mirror portal;
	const Entity far =
		portal.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	portal.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	portal.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	portal.World.Set<engine::scene::Portal>(portal.Reflection, engine::scene::Portal{far});

	// Walking at the pane along -Z in centimetre steps, from well outside the
	// old band to within a millimetre of the glass.
	Vector3 previous;
	bool first = true;

	for (int step = 60; step >= 1; step--) {
		const float z = -0.2f + static_cast<float>(step) * 0.01f;
		portal.World.GetMutable<Transform>(portal.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, z});

		REQUIRE(AimSurfaceCameras(portal.World) == 1);

		const Vector3 placed = portal.Placed();
		if (!first) {
			// **A centimetre of eye is a centimetre of camera**, because the map
			// is a rigid one for a matched pair. A flip would move it by twice
			// its distance from the pane, which at this range is metres.
			CHECK((placed - previous).Magnitude() < 0.05f);
		}

		previous = placed;
		first = false;
	}
}

TEST_CASE("a crosser is put down clear of the plane it crossed", "[scene][surfacecameras]") {
	// **A body that lands on a plane is a body that can cross it again**, and
	// one tick of jitter is all it takes — which reads as the portal throwing
	// somebody back and forth rather than as a rounding error. The clearance is
	// the hysteresis and it is the only one: offsetting the *test* plane as
	// CodeParade's demo also does would, at this engine's tick rate, be a band a
	// body can step over without ever changing sign.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// A step that ends barely past the plane, which is the case with almost no
	// depth of its own to be put down at — a character brought to a halt in a
	// doorway by the thing it walked into.
	const Entity walker =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
	mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, -0.2f - 1e-5f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	// **Out of the far pane and clear of it, rather than resting in its plane.**
	// Without the clearance this lands a hundredth of a millimetre past B and
	// every one of the checks below fails.
	const Vector3 landed = mirror.World.Get<Transform>(walker)->Frame.Position;
	CHECK_THAT(landed.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK(landed.Z > -0.2f);
	CHECK(std::abs(landed.Z + 0.2f) > 0.005f);

	// **And it stays there, even when something nudges it back.** The next tick
	// starts where this one ended, and a body standing clear of a plane cannot
	// change sign through it on a step smaller than the clearance — which is
	// exactly the jitter that used to send a crosser back through the hole it
	// had just come out of, once per tick.
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(landed)}
	);
	mirror.World.Set<Transform>(walker, Transform{CFrame(landed - Vector3{0.0f, 0.0f, 0.005f})});
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);

	// **And it is hysteresis rather than a wall.** There is only one pane in
	// this world and the body is now beside the far end of it, so walking back
	// through is the round-trip case rather than this one — see "going back
	// through a scaled hole", which does it with both halves of a pair.
}

TEST_CASE("a hole between panes of different sizes changes what goes through it", "[scene][surfacecameras]") {
	// **The difference between a room bigger on the inside and a picture of
	// one.** A rigid map puts the camera at the far end and leaves everything
	// the size it was, so a doorway twice as big shows the same room through a
	// bigger frame and a body walks out of it unchanged. Carrying the scale is
	// what makes the simulation agree with the claim.
	//
	// Pane A is 16 by 9 across its face — the fixture's half-extents are 8 and
	// 4.5 — and B is twice that on both axes, so the ratio of the areas is four
	// and the scale is its square root.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{16.0f, 9.0f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	CHECK_THAT(seams[0].Scale, Catch::Matchers::WithinAbs(2.0f, TOLERANCE));

	// **And the picture is told**, or the hole draws the far room at the near
	// room's size and the image slides across the glass as the viewer moves.
	// `SurfaceLens` carries the map in three pieces because a `CFrame` cannot
	// hold a scale, and this is the piece that would be silently dropped.
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	const SurfaceLens *lens = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(lens != nullptr);
	CHECK_THAT(lens->MappingScale, Catch::Matchers::WithinAbs(2.0f, TOLERANCE));

	// A body walking into the small end comes out of the large one, twice the
	// size and twice as fast — a speed is a length, and a crosser that kept its
	// old one would cross the far room in half the time.
	const Entity walker =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
	mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f})});
	mirror.World.Set<Bounds>(walker, Bounds{Vector3{0.5f, 1.0f, 0.5f}});
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	CHECK_THAT(mirror.World.Get<Bounds>(walker)->HalfExtent.Y, Catch::Matchers::WithinAbs(2.0f, TOLERANCE));
	CHECK_THAT(
		mirror.World.Get<engine::scene::Motion>(walker)->Linear.Magnitude(),
		Catch::Matchers::WithinAbs(32.0f, 1e-3f)
	);
}

TEST_CASE("a matched pair of panes changes nothing about what crosses", "[scene][surfacecameras]") {
	// **The regression guard for every world already built.** The scale is
	// derived from two measurements rather than authored, so the ordinary pair
	// has to come out at exactly one — not nearly one — or every portal in the
	// repository quietly starts resizing whatever walks through it.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	CHECK(seams[0].Scale == 1.0f);

	// And a mirror, which has no destination to be measured against at all.
	Mirror plain;
	REQUIRE(AimSurfaceCameras(plain.World) == 1);
	CHECK(plain.World.Get<SurfaceLens>(plain.Reflection)->MappingScale == 1.0f);
}

TEST_CASE("going back through a scaled hole undoes the scaling", "[scene][surfacecameras]") {
	// **A corridor of mismatched holes has to be somewhere you can walk about
	// in, not a ratchet.** The scale of a seam is the ratio of two measurements
	// that a crossing does not change, so the reverse pair is the reciprocal by
	// construction — but "by construction" is exactly the sort of claim that
	// stops being true when somebody normalises one of the two.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{16.0f, 9.0f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// The other half of the pair: a surface camera on the far pane, leading
	// back. Its `Front` face is the one the fixture's portal already measures
	// against, so the two seams are the same two rectangles the other way up.
	const Entity back =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "Back");
	SurfaceCamera target;
	target.Face = NormalId::Front;
	mirror.World.Set<SurfaceCamera>(back, target);
	mirror.World.Set<Transform>(back, Transform{CFrame()});
	mirror.World.SetParent(back, far);
	mirror.World.Set<engine::scene::Portal>(back, engine::scene::Portal{mirror.Pane});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 2);
	CHECK_THAT(seams[0].Scale * seams[1].Scale, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));

	const Entity walker =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
	mirror.World.Set<Bounds>(walker, Bounds{Vector3{0.5f, 1.0f, 0.5f}});
	mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	// Straight back the way it came, which is a step against the velocity the
	// crossing gave it. **Taken from the motion rather than written down as an
	// axis**, so the case still tests a round trip when the map changes which
	// side of the far pane a crosser lands on.
	const CFrame arrived = mirror.World.Get<Transform>(walker)->Frame;
	const Vector3 away = mirror.World.Get<engine::scene::Motion>(walker)->Linear.Unit();
	mirror.World.Set<engine::scene::PreviousTransform>(walker, engine::scene::PreviousTransform{arrived});
	mirror.World.Set<Transform>(walker, Transform{CFrame(arrived.Position - away * 4.0f)});

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	CHECK_THAT(mirror.World.Get<Bounds>(walker)->HalfExtent.X, Catch::Matchers::WithinAbs(0.5f, TOLERANCE));
	CHECK_THAT(mirror.World.Get<Bounds>(walker)->HalfExtent.Y, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));
	CHECK_THAT(
		mirror.World.Get<engine::scene::Motion>(walker)->Linear.Magnitude(),
		Catch::Matchers::WithinAbs(16.0f, 1e-3f)
	);
}

TEST_CASE("a body goes through the nearest hole, not the first one gathered", "[scene][surfacecameras]") {
	// **The bug that reads as the camera flipping on some crossings and not
	// others.** `PlaceCamera` puts the third-person arm through the *nearest*
	// pane its segment meets, and `CrossPortals` used to move the body through
	// the *first one gathered* — which is archetype order, and archetype order
	// moves whenever anything in the world changes a component set. On the
	// frames the two chose differently the body went through one hole and the
	// eye through another, and nothing about the crossing distinguished those
	// frames from the ones that worked.
	//
	// The fixture's pane is at `z = -0.2` and leads to `x = 100`. A second one
	// stands *in front* of it at `z = 0.3` and leads to `x = 200`, and is
	// created second so that a first-match rule picks the fixture's.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	const Entity nearPane =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "NearPane");
	mirror.World.Set<Transform>(nearPane, Transform{CFrame(Vector3{0.0f, 0.0f, 0.5f})});
	mirror.World.Set<Bounds>(nearPane, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

	const Entity nearFar =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "NearFar");
	mirror.World.Set<Transform>(nearFar, Transform{CFrame(Vector3{200.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(nearFar, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

	const Entity nearCamera = mirror.World.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "NearReflection"
	);
	SurfaceCamera target;
	target.Face = NormalId::Front;
	mirror.World.Set<SurfaceCamera>(nearCamera, target);
	mirror.World.Set<Transform>(nearCamera, Transform{CFrame()});
	mirror.World.SetParent(nearCamera, nearPane);
	mirror.World.Set<engine::scene::Portal>(nearCamera, engine::scene::Portal{nearFar});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 2);

	// **What makes this a regression guard rather than a coincidence.** The
	// *farther* pane is gathered first, so a rule that took the first match
	// would send the body to `x = 100` and this case would be green against the
	// bug it is written for. If a change to the store's walk order ever flips
	// this, the case below stops testing anything and this line is what says so.
	REQUIRE(seams[0].Pane == mirror.Pane);

	// A step that passes through both planes inside both rectangles.
	const Entity walker =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
	mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, -2.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 2.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	// The one it reached first, which is the one in front.
	CHECK_THAT(
		mirror.World.Get<Transform>(walker)->Frame.Position.X, Catch::Matchers::WithinAbs(200.0f, TOLERANCE)
	);

	// **And the eye agrees, which is the whole point.** The same segment through
	// the same rule has to name the same hole, or the body and the camera end up
	// in two rooms.
	engine::scene::SeamTransform carried;
	REQUIRE(
		engine::scene::PortalCrossing(
			mirror.World, Vector3{0.0f, 0.0f, 2.0f}, Vector3{0.0f, 0.0f, -2.0f}, carried
		)
	);
	CHECK_THAT(carried.Point(Vector3{0.0f, 0.0f, -2.0f}).X, Catch::Matchers::WithinAbs(200.0f, 1e-2f));
}

TEST_CASE("a point is on one side of a hole or the other", "[scene][surfacecameras]") {
	// **A particle is a point, and a point is not a body.** A body has a size,
	// straddles a plane and is cut by it; a spark is wholly in one space or the
	// other, so it is *moved* through a hole rather than copied and cut. Drawing
	// it in both places would be two sparks where the author authored one, and
	// `client::CollectParticleBatches` is the caller this exists for — a torch
	// carried into a doorway whose flame dies at the seam is the artefact.
	//
	// The geometry is this file's: pane A at the origin with its `Front` face at
	// `z = -0.2`, so "through" is `z > -0.2`.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);

	// In front of the face, which is the room the pane is in.
	CHECK_FALSE(engine::scene::SeamCarries(seams[0], Vector3{0.0f, 0.0f, -2.0f}));

	// Past it, inside the rectangle: through.
	CHECK(engine::scene::SeamCarries(seams[0], Vector3{0.0f, 0.0f, 1.0f}));

	// **Past the plane and beside the hole is not through it**, which is the one
	// thing that separates this from a plane test. The rectangle is eight by
	// four and a half, so nine studs across is outside it — and unlike
	// `SeamStraddled` there is no widening, because a point has no reach to
	// widen by.
	CHECK_FALSE(engine::scene::SeamCarries(seams[0], Vector3{9.0f, 0.0f, 1.0f}));
	CHECK_FALSE(engine::scene::SeamCarries(seams[0], Vector3{0.0f, 5.0f, 1.0f}));

	// And the edge of the rectangle is inside it, so a spark exactly on the rim
	// goes through rather than falling between the two answers.
	CHECK(engine::scene::SeamCarries(seams[0], Vector3{8.0f, 0.0f, 1.0f}));
}

TEST_CASE("nothing larger than a hole is drawn through it", "[scene][surfacecameras]") {
	// **A floor is not standing in a doorway, and the straddle test used to
	// think it was.** Every check in that test widens by the body's own reach,
	// which is right for a character and absurd for a slab: a floor fifty studs
	// across has a reach of seventy, so its centre is within reach of every
	// plane in the building and inside every rectangle in it. What got drawn was
	// a copy of the far room's floor laid across the near room, meeting it along
	// a hard straight line through the middle of the scene.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// **And a person is very nearly as big as the doorway they walk through**,
	// which is the half a size rule gets wrong if it is stated against the
	// pane's shorter half-axis. It was, briefly, and it refused every character
	// in every hole — no clone in the far room, no half a body in the picture,
	// which is the artefact this whole mechanism exists to remove.
	//
	// So the rule is not about size at all any more: it is whether the body
	// **fits through the hole's own footprint**, which `CutOfSeam::Fits`
	// answers with the body's oriented box against the pane's half-axes. A room
	// is far wider than its own doorway and is refused; a person centred in one
	// is not. `SeamStraddled` has no size rule of its own and never needed one.
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);

	// A character-shaped body in the seam, which must straddle.
	CHECK(engine::scene::SeamStraddled(seams[0], Vector3{0.0f, 0.0f, -0.2f}, 2.9f));

	// A doorway-sized pane and the same character: still yes. The pane's shorter
	// half-axis is two and the character's reach is nearly three, which is the
	// arithmetic the broken rule refused.
	engine::scene::PortalSeam doorway = seams[0];
	doorway.First = Vector3{2.0f, 0.0f, 0.0f};
	doorway.Second = Vector3{0.0f, 2.5f, 0.0f};
	CHECK(engine::scene::SeamStraddled(doorway, Vector3{0.0f, 0.0f, -0.2f}, 2.9f));

	// The pass that draws the far half is where the guard lives, because it is
	// the one that would otherwise lay a floor across the room next door.
	std::vector<engine::scene::DrawInstance> drawn;

	// A slab the size of a room, straddling this pane. Fifty studs will not go
	// through a sixteen-stud hole, so it is neither copied nor cut.
	engine::scene::DrawInstance slab;
	slab.Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});
	slab.HalfExtent = Vector3{50.0f, 0.5f, 50.0f};
	drawn.push_back(slab);

	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);
	CHECK(drawn[0].SeamNormal == Vector3{});

	// A person-sized body in the same hole, which does fit and therefore does
	// get both.
	engine::scene::DrawInstance body;
	body.Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});
	body.HalfExtent = Vector3{1.0f, 2.0f, 1.0f};
	drawn.push_back(body);

	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[1].SeamNormal != Vector3{});

	// **And the doorway case that broke the size rule the first time.** A pane
	// four by five, and a character whose bounding sphere is bigger than its
	// shorter half-axis: refused by any rule stated against a radius, admitted
	// by one stated against the box, which is what a person walking through a
	// doorway needs.
	engine::scene::PortalSeam narrow = seams[0];
	narrow.First = Vector3{2.0f, 0.0f, 0.0f};
	narrow.Second = Vector3{0.0f, 2.5f, 0.0f};

	const engine::scene::SeamTransform through = engine::scene::SeamMapping(narrow);
	const engine::scene::SeamCut cut = engine::scene::CutOfSeam(
		narrow, through, CFrame(Vector3{0.0f, 0.0f, -0.2f}), Vector3{1.0f, 2.0f, 1.0f}
	);
	CHECK(cut.Fits);
}

TEST_CASE("the near plane and the clip follow the eye into a hole", "[scene][surfacecameras]") {
	// **What makes the last hand's width of an approach seamless.** A near plane
	// is a floor on how close geometry may be drawn, so walking up to a pane with
	// an authored one slices it open and the wall beside the doorway disappears —
	// on the one frame the whole feature is judged. CodeParade's demo drives the
	// near plane down from the nearest hole's distance and pulls the oblique clip
	// back by the same measure; these are those two numbers.
	constexpr float FAR_AWAY = std::numeric_limits<float>::infinity();

	// **The rectangle and not its plane.** An eye level with a doorway but well
	// to the side of it is standing in a wall, and a plane says zero for both —
	// which would spend the depth range on nothing.
	const Vector3 centre{0.0f, 0.0f, -0.2f};
	const Vector3 first{8.0f, 0.0f, 0.0f};
	const Vector3 second{0.0f, 4.5f, 0.0f};
	CHECK_THAT(
		engine::scene::RectangleDistance(centre, first, second, Vector3{0.0f, 0.0f, 4.8f}),
		Catch::Matchers::WithinAbs(5.0f, TOLERANCE)
	);
	CHECK_THAT(
		engine::scene::RectangleDistance(centre, first, second, Vector3{40.0f, 0.0f, -0.2f}),
		Catch::Matchers::WithinAbs(32.0f, TOLERANCE)
	);

	// The same, asked of a world rather than of four vectors.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	CHECK_THAT(
		engine::scene::NearestSeamDistance(mirror.World, Vector3{0.0f, 0.0f, 4.8f}),
		Catch::Matchers::WithinAbs(5.0f, TOLERANCE)
	);

	// **The authored value survives, which is the reason this is a function and
	// not a write-back.** A world with no holes in it, and an eye well away from
	// the one hole there is, both draw with exactly what the scene asked for —
	// so the depth range is only ever spent when something needs it.
	CHECK_THAT(engine::scene::PortalNearPlane(0.1f, FAR_AWAY), Catch::Matchers::WithinAbs(0.1f, TOLERANCE));
	CHECK_THAT(engine::scene::PortalNearPlane(0.1f, 5.0f), Catch::Matchers::WithinAbs(0.1f, TOLERANCE));

	// Half the distance once half the distance is the smaller number, so the
	// pane is never inside the near plane.
	CHECK_THAT(engine::scene::PortalNearPlane(0.1f, 0.08f), Catch::Matchers::WithinAbs(0.04f, TOLERANCE));

	// And a floor, because an eye pressed against the glass would otherwise ask
	// for a near plane of zero and get a projection of infinities.
	CHECK_THAT(
		engine::scene::PortalNearPlane(0.1f, 1.0e-6f),
		Catch::Matchers::WithinAbs(engine::scene::PORTAL_NEAR_MIN, TOLERANCE)
	);

	// The clip bias is the same halving, capped by the width inside which a
	// pane's construction is degenerate anyway, and nothing at all when there is
	// no hole to bias against.
	CHECK_THAT(engine::scene::PortalClipBias(FAR_AWAY), Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(engine::scene::PortalClipBias(0.08f), Catch::Matchers::WithinAbs(0.04f, TOLERANCE));
	CHECK_THAT(engine::scene::PortalClipBias(50.0f), Catch::Matchers::WithinAbs(0.3f, TOLERANCE));
}

TEST_CASE("a crossing reports the turn the body actually made", "[scene][surfacecameras]") {
	// **The turn has to be the crosser's, not the map's idea of north.** Mapping
	// a fixed reference and calling the result the turn is right only while the
	// composed rotation is a pure yaw. Tip either pane and it is not: the yaw of
	// a mapped north is then an angle nothing turned through, wrong by an amount
	// that depends on the geometry rather than on anything the player did — which
	// reads as the view snapping to a heading nobody entered from.
	//
	// So the pair here is deliberately tilted, and the property checked is the
	// one that survives it: the body's own facing turned by exactly what was
	// reported.
	constexpr float QUARTER = 1.57079632679f;

	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(
		far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f}) * CFrame::Angles(0.4f, QUARTER, 0.0f)}
	);
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// Walking along -Z through pane A, aimed a little off its normal so a turn
	// measured off the body differs from one measured off the axis.
	const CFrame started = CFrame(Vector3{0.0f, 0.0f, 1.0f}) * CFrame::Angles(0.0f, 0.6f, 0.0f);
	const Entity walker =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
	mirror.World.Set<Transform>(
		walker, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f}) * CFrame::Angles(0.0f, 0.6f, 0.0f)}
	);
	mirror.World.Set<engine::scene::PreviousTransform>(walker, engine::scene::PreviousTransform{started});
	mirror.World.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	const engine::scene::PortalTransit *went = mirror.World.Get<engine::scene::PortalTransit>(walker);
	REQUIRE(went != nullptr);

	const auto yawOf = [](const CFrame &frame) {
		const Vector3 facing = frame.VectorToWorldSpace(Vector3{0.0f, 0.0f, -1.0f});
		return std::atan2(-facing.X, -facing.Z);
	};

	const float before = yawOf(started);
	const float after = yawOf(mirror.World.Get<Transform>(walker)->Frame);

	// The reported turn takes the old heading to the new one. This is what the
	// camera on the looking machine adds to its own yaw, so anything else is the
	// view and the body disagreeing about which way the room went.
	CHECK_THAT(
		std::remainder(after - before - went->Turn, 2.0f * 3.14159265358979f),
		Catch::Matchers::WithinAbs(0.0f, 1.0e-4f)
	);

	// And it is a turn rather than a heading: an unturned pair reports zero, not
	// whichever way the pair happens to point.
	CHECK(std::abs(went->Turn) > 1.0e-3f);
}

TEST_CASE("a viewpoint is never left standing in a pane", "[scene][surfacecameras]") {
	// **The band a camera cannot render from.** A surface camera built from an
	// eye in its own pane's plane has no half-space for the oblique clip to keep
	// and no bounded fit, and the pane comes out as a vertical smear of
	// stretched texels — which reads as a corrupt texture rather than as an eye
	// standing somewhere it should not. A body already has this rule: it is put
	// down clear of whatever plane it crossed.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// The pane's face is at `z = -0.2` and its normal is `-Z`, so the side the
	// normal points to — the side `SeamOffset` calls positive — is the smaller
	// `z`. A point a little that way is a point that belongs that way.
	//
	// **A hair rather than a hand's width, and that is the whole seamlessness
	// argument.** A same-world hole is drawn by the recursive pass, whose only
	// construction is an oblique clip — it needs the eye off the plane and
	// nothing else — and `PortalNearPlane` shrinks the near plane to meet
	// whatever is left. So an eye may walk right up to one, and being shoved a
	// third of a stud instead is a visible push at the one moment the illusion
	// is judged.
	Vector3 ahead{0.0f, 0.0f, -0.202f};
	CHECK(engine::scene::ClearOfPanes(mirror.World, ahead));
	CHECK(std::abs(ahead.Z + 0.2f) > 0.002f);

	// **Out of the side it was nearer**, which is the same tie-break a crossing
	// body gets from `SeamMapping`: barely in from one side and it belongs on
	// that side.
	CHECK(ahead.Z < -0.2f);

	// And a point on the other side of the middle comes out the other way.
	Vector3 behind{0.0f, 0.0f, -0.198f};
	CHECK(engine::scene::ClearOfPanes(mirror.World, behind));
	CHECK(behind.Z > -0.2f);

	// **A tenth of a stud from a hole is simply standing near it**, which is
	// where the old rule moved a camera and the new one leaves it. This is the
	// assertion that fails if the wide margin ever comes back.
	Vector3 close{0.0f, 0.0f, -0.3f};
	CHECK_FALSE(engine::scene::ClearOfPanes(mirror.World, close));
	CHECK_THAT(close.Z, Catch::Matchers::WithinAbs(-0.3f, TOLERANCE));

	// **A cross-world pane keeps the old margin, because it is still a
	// picture.** It goes through `AimSurfaceCameras`, which fits extents to the
	// rectangle from the viewpoint and runs away as that viewpoint reaches the
	// plane — there is nothing to walk through and no recursion to draw it.
	{
		engine::scene::Portal crossing{far};
		crossing.DestinationWorld = engine::core::Name("somewhere else");
		mirror.World.Set<engine::scene::Portal>(mirror.Reflection, crossing);

		Vector3 near{0.0f, 0.0f, -0.3f};
		CHECK(engine::scene::ClearOfPanes(mirror.World, near));
		CHECK(std::abs(near.Z + 0.2f) > 0.2f);

		mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});
	}

	// **Well clear is left exactly alone**, which is every frame in every scene
	// that has a portal in it and nobody standing in one.
	Vector3 outside{0.0f, 0.0f, 5.0f};
	CHECK_FALSE(engine::scene::ClearOfPanes(mirror.World, outside));
	CHECK_THAT(outside.Z, Catch::Matchers::WithinAbs(5.0f, TOLERANCE));

	// And so is a point level with the pane but well past the end of it: a plane
	// is infinite and a pane is not, so an eye beside a doorway is in a wall
	// rather than in a hole.
	Vector3 beside{40.0f, 0.0f, -0.2f};
	CHECK_FALSE(engine::scene::ClearOfPanes(mirror.World, beside));
	CHECK_THAT(beside.X, Catch::Matchers::WithinAbs(40.0f, TOLERANCE));
}

TEST_CASE("a far-side copy that lands on its original is not drawn", "[scene][surfacecameras]") {
	// **Two coplanar surfaces at one depth is a stripe of flickering colour.** A
	// pair of panes can be arranged so the map is near enough the identity for
	// whatever stands beside them — two rooms laid out adjacent with the hole
	// between them agreeing with the geometry — and every copy then arrives on
	// top of the thing it was copied from. A copy that overlaps its original is
	// not a far half; it is a duplicate, and it can only fight.
	//
	// The degenerate pairing is a pane whose destination is itself: the map
	// composes to something that leaves the pane's own neighbourhood where it
	// is.
	Mirror mirror;
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{mirror.Pane});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);

	std::vector<engine::scene::DrawInstance> drawn;

	engine::scene::DrawInstance body;
	body.Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});
	body.HalfExtent = Vector3{1.0f, 2.0f, 1.0f};
	drawn.push_back(body);

	// The map takes this body onto itself, so there is nothing to add — and
	// nothing to cut either, because a body with no far half must not lose its
	// near one.
	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);
	CHECK(drawn.size() == 1);
	CHECK(drawn[0].SeamNormal == Vector3{});
}

TEST_CASE("a character standing in a hole is drawn on both sides of it", "[scene][surfacecameras]") {
	// **The artefact this whole mechanism exists to remove, asserted against a
	// real rig rather than a box.** A pane is a hole and a body may straddle it;
	// the body is one set of parts in one place, so without a copy the far room
	// draws nothing and the near room draws all of it. Standing in the seam you
	// are whole on the side you came from and absent on the side you are walking
	// into.
	//
	// **A whole character, because that is where it broke.** A character is six
	// anchored limbs posed off an invisible root, so it is found by the limb
	// walk and not the moving-body one — and a size rule stated against the
	// pane's shorter half-axis refused every one of them, because a person is
	// very nearly as big as the doorway they walk through.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	engine::scene::InstallServices(mirror.World);

	// Feet on the bottom edge of the pane's rectangle, standing in its plane.
	engine::scene::CharacterDesc desc;
	desc.Frame = CFrame(Vector3{0.0f, -4.5f, -0.2f});
	const Entity model = engine::scene::MakeCharacter(mirror.World, desc);
	REQUIRE(model != engine::ecs::NULL_ENTITY);

	// What decides whether a part draws at all, and therefore whether the clone
	// walk can see it.
	(void)engine::scene::SyncRendered(mirror.World);
	(void)engine::scene::PoseCharacters(mirror.World);

	// The draw list a collector would build from this world, which is what the
	// pass reads. Built here by hand because `client::CollectInstances` is a
	// tier away and what is under test is the seam.
	std::vector<engine::scene::DrawInstance> drawn;
	mirror.World.Each<const Transform, const Bounds, const Visual, const engine::scene::Rendered>(
		[&drawn](
			Entity,
			const Transform &placement,
			const Bounds &bounds,
			const Visual &visual,
			const engine::scene::Rendered &
		) {
			engine::scene::DrawInstance row;
			row.Frame = placement.Frame;
			row.HalfExtent = bounds.HalfExtent;
			row.Surface = visual.Surface;
			drawn.push_back(row);
		}
	);

	const size_t rows = drawn.size();
	REQUIRE(rows > 0);

	const size_t clones = engine::scene::CutAndCloneSeams(mirror.World, drawn);

	// **More than one, because a character is not one box.** The exact count is
	// how many limbs happen to reach the plane *and fit through it* and is the
	// rig's business; that it is none is the bug.
	CHECK(clones > 0);
	REQUIRE(drawn.size() == rows + clones);

	// **And on the far side**, which is the half that says the copy is a copy
	// through the hole rather than a second one beside the original.
	// Within a rig's own width of the far pane rather than on it: an arm sits a
	// stud and a half off the middle, and it is still in the far room.
	for (size_t index = rows; index < drawn.size(); index++) {
		const engine::scene::DrawInstance &copy = drawn[index];
		CHECK_THAT(copy.Frame.Position.X, Catch::Matchers::WithinAbs(100.0f, 3.0f));

		// Never a surface of its own, or a copied mirror would fight the
		// original for the slot.
		CHECK(copy.Surface == -1);

		// **Cut, and cut the other way from its original.** A limb standing in
		// a doorway is one limb: the near half is drawn here and the far half
		// is drawn there, and the two planes are each other's image.
		CHECK(copy.SeamNormal != Vector3{});
	}

	// Every limb that got a copy was cut in place as well, which is the half a
	// list walk exists to do.
	size_t cut = 0;
	for (size_t index = 0; index < rows; index++) {
		cut += drawn[index].SeamNormal == Vector3{} ? 0u : 1u;
	}
	CHECK(cut == clones);
}

TEST_CASE("the far half of a body reaches the picture in the pane", "[scene][surfacecameras]") {
	// **Being drawn on both sides is half the ask; the *hole* showing it is the
	// other half.** A copy appended to the draw list is drawn by the screen
	// pass, which puts the far half in the far room — but somebody looking *at*
	// the pane sees the surface texture, and if the copy never reached the
	// surface pass the picture in the hole would show a room with nobody in it
	// while the body is visibly standing in the doorway.
	//
	// What decides that is where `OrderScene` puts it. The surface pass draws
	// the `Reflected` run — the opaque part of the scene range that is not
	// itself a mirror — so a copy is shown by the hole exactly when it lands
	// there. It does, because a copy carries `Surface = -1` and the original's
	// transparency, and the ordering has no other opinion.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// The pane itself, which is what makes the run boundaries meaningful: it
	// shows a surface, so it belongs *after* the reflected run and must not be
	// mistaken for content.
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	std::vector<engine::scene::DrawInstance> drawn;

	engine::scene::DrawInstance pane;
	pane.Frame = CFrame(Vector3::Zero);
	pane.HalfExtent = Vector3{8.0f, 4.5f, 0.2f};
	pane.Surface = mirror.World.Get<Visual>(mirror.Pane)->Surface;
	drawn.push_back(pane);

	engine::scene::DrawInstance body;
	body.Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});
	body.HalfExtent = Vector3{1.0f, 2.0f, 1.0f};
	drawn.push_back(body);

	// Nothing in this world carries a `Motion` or a limb, and it does not
	// matter: what decides whether a thing standing in a hole is copied is
	// whether it fits through the hole, which this body does.
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);

	std::vector<uint32_t> order;
	const engine::scene::ScenePlan plan = engine::scene::OrderScene(drawn, Vector3{0.0f, 0.0f, 20.0f}, order);

	// The copy is the last row, and it has to land inside the run the surface
	// pass submits — not in the mirror run after it, and not in the blended
	// tail.
	const auto copy = static_cast<uint32_t>(drawn.size() - 1);
	bool inReflected = false;
	for (uint32_t at = 0; at < plan.Reflected; at++) {
		inReflected = inReflected || order[at] == copy;
	}

	CHECK(inReflected);
	CHECK(plan.Reflected < plan.Opaque);
}

TEST_CASE("a hole's camera stands where its own map says", "[scene][surfacecameras]") {
	// **Two derivations of one map, and they were allowed to disagree.**
	// `scene::SeamMapping` is the single statement of what a hole does to what
	// goes through it: one map per pane, carrying its front hemisphere to the far
	// pane's back one and its back to the far pane's front, so a pair's two maps
	// are each other's exact inverse. A crossing body goes through it, a ray goes
	// through it, the recursive pass's sub-camera goes through it, and the far
	// half of anything standing in the seam goes through it.
	//
	// `AimSurfaceCameras` composed its own instead — out of a *source frame built
	// from which side the viewer is on*. That is the pre-v0.15 shape, and it is
	// side-dependent by construction: the same pane yields one map for a viewer in
	// front and a different one for a viewer behind.
	//
	// **A same-world hole hid it**, because the recursive portal pass took over
	// the picture and this camera stopped reaching a screen. A **cross-world**
	// pane is the one that still draws from here — and the immersive scene spawns
	// you *behind* the pane, which is precisely the side the two maps differ on.
	// What that looks like is a window onto the other world showing the wrong half
	// of it: the floor, which is everywhere, and none of the furniture.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	const engine::scene::SeamTransform through = engine::scene::SeamMapping(seams[0]);

	// The eye's two sides of the pane, whose `Front` face is at `z = -0.2`.
	const auto aimedFrom = [&](const Vector3 &at) {
		mirror.World.Set<Transform>(mirror.Eye, Transform{CFrame(at)});
		REQUIRE(AimSurfaceCameras(mirror.World) == 1);
		return mirror.Placed();
	};

	// **In front of the face**, which is the side a same-world hole is usually
	// walked into from and the side this always agreed on.
	const Vector3 ahead{0.0f, 0.0f, -20.0f};
	const Vector3 aheadCamera = aimedFrom(ahead);
	CHECK_THAT(aheadCamera.X, Catch::Matchers::WithinAbs(through.Point(ahead).X, 1e-2f));
	CHECK_THAT(aheadCamera.Y, Catch::Matchers::WithinAbs(through.Point(ahead).Y, 1e-2f));
	CHECK_THAT(aheadCamera.Z, Catch::Matchers::WithinAbs(through.Point(ahead).Z, 1e-2f));

	// **And behind it**, which is where `ImmersivePortals.luau` puts you: its
	// spawn pad is on the pane's back side, so every frame of that scene is this
	// case. The camera has to be at the same place the body would land, or the
	// picture is of a room nobody can walk to.
	const Vector3 behind{0.0f, 0.0f, 20.0f};
	const Vector3 behindCamera = aimedFrom(behind);
	CHECK_THAT(behindCamera.X, Catch::Matchers::WithinAbs(through.Point(behind).X, 1e-2f));
	CHECK_THAT(behindCamera.Y, Catch::Matchers::WithinAbs(through.Point(behind).Y, 1e-2f));
	CHECK_THAT(behindCamera.Z, Catch::Matchers::WithinAbs(through.Point(behind).Z, 1e-2f));

	// **And from either side the clip keeps the half a traveller lands in**,
	// which is the invariant the coordinates are only evidence for. A camera in
	// the right place looking the wrong way, or clipping the wrong half, shows
	// an empty room just as convincingly as one in the wrong place.
	const auto keepsWhereTheyLand = [&](const Vector3 &eye) {
		mirror.World.Set<Transform>(mirror.Eye, Transform{CFrame(eye)});
		REQUIRE(AimSurfaceCameras(mirror.World) == 1);

		const SurfaceLens *lens = mirror.World.Get<SurfaceLens>(mirror.Reflection);
		REQUIRE(lens != nullptr);

		// A step through the pane from where the eye is, mapped the way a body
		// would be: `CrossPortals` moves a crosser by exactly this.
		const Vector3 crossed{0.0f, 0.0f, eye.Z > 0.0f ? -2.0f : 2.0f};
		const Vector3 landed = through.Point(crossed);

		return landed.Dot(lens->ClipNormal) >= lens->ClipDistance;
	};

	CHECK(keepsWhereTheyLand(ahead));
	CHECK(keepsWhereTheyLand(behind));
}

TEST_CASE("a hole's pane samples the image its own camera drew", "[scene][surfacecameras]") {
	// **The last thing between a correct camera and a correct picture.** A pane
	// does not read its texture by UV: `opaque.frag` projects the fragment's
	// world position through `SurfaceViewProjection · SurfaceMapping` and tests
	// the result against the `0..1` rectangle, falling through to the plainly-lit
	// pane outside it. So a camera standing in exactly the right place still
	// shows nothing if that product does not land the *source* pane inside the
	// image — and what that looks like is a pane drawing its own material, flat,
	// which reads as "the other world does not render" rather than as a matrix.
	//
	// The three matrices come from one place and must therefore agree: the fit
	// and the oblique clip are built against the *mapped* rectangle, and the
	// mapping carries the source pane onto exactly that rectangle. This case is
	// the assertion that they do, stated where a suite can reach it — the
	// renderer needs a device and this arithmetic does not.
	//
	// The geometry is `examples/CrossWorldSeam.luau`'s, which is the scene the
	// cross-world report was captured from: a ten by eight pane at the origin,
	// its stand-in destination a little behind it, and the eye on the pane's
	// **back** side where a player spawns.
	Mirror mirror(NormalId::Front, CFrame(Vector3{0.0f, 4.0f, 0.0f}));
	mirror.World.Set<Bounds>(mirror.Pane, Bounds{Vector3{5.0f, 4.0f, 0.2f}});
	mirror.World.Set<Transform>(mirror.Eye, Transform{CFrame(Vector3{0.0f, 5.0f, 16.0f})});

	const Entity beyond =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Beyond");
	mirror.World.Set<Transform>(beyond, Transform{CFrame(Vector3{0.0f, 4.0f, -0.6f})});
	mirror.World.Set<Bounds>(beyond, Bounds{Vector3{5.0f, 4.0f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{beyond});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	const CFrame &frame = mirror.World.Get<Transform>(mirror.Reflection)->Frame;
	const SurfaceLens *lens = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(lens != nullptr);

	// Exactly what `render::Renderer` composes for the pane: the camera's own
	// matrices, then the map the pane reads the image back through.
	const glm::mat4 sampling =
		engine::scene::ResolveSurfaceCamera(frame, engine::scene::SurfaceProjection(*lens, frame))
			.ViewProjection *
		engine::scene::SurfaceMapping(*lens);

	// The source pane's own face, which is what the fragment shader projects:
	// `Front` puts it at `z = -0.2`, five wide and four tall about `y = 4`.
	const auto lands = [&sampling](float x, float y) {
		const glm::vec4 clip = sampling * glm::vec4{x, y, -0.2f, 1.0f};

		// Behind the camera is the loudest way to miss: `w` turns negative and
		// the divide flips the coordinate to the other side of the image.
		if (!(clip.w > 0.0f)) {
			return false;
		}

		const float u = (clip.x / clip.w) * 0.5f + 0.5f;
		const float v = 0.5f - (clip.y / clip.w) * 0.5f;
		return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
	};

	CHECK(lands(-5.0f, 0.0f));
	CHECK(lands(5.0f, 0.0f));
	CHECK(lands(-5.0f, 8.0f));
	CHECK(lands(5.0f, 8.0f));
	CHECK(lands(0.0f, 4.0f));
}
