// Where a surface camera ends up when it is parented to a face.
//
// **The half of a mirror a test can reach.** What the reflection *looks* like
// needs a GPU; whether the camera is in the right place is six dot products and
// can be asserted against - which is the same split `graph::Frustum` is built on
// and the reason this arithmetic lives in `scene` rather than in the renderer.
//
// The cases below are the ones that were wrong at some point in a script doing
// this by hand: a camera behind the pane facing away from it, a plane at the
// full extent instead of the half, and a mirror rendering perfectly into a
// texture that nothing sampled.

#include "SurfaceCamerasFixture.hpp"

TEST_SUITE_ID("engine.scene.surfacecameras")

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
	// put behind the pane would face *away* from it and render empty space -
	// which is what the first hand-written version of this did. It must look
	// back towards the face.
	const CFrame &frame = mirror.World.Get<Transform>(mirror.Reflection)->Frame;
	CHECK(frame.LookVector().Z > 0.9f);
}

TEST_CASE("a temporary view eye aims mirrors without moving the camera", "[scene][surfacecameras]") {
	Mirror mirror;
	mirror.World.SetResource(engine::scene::ViewEye{mirror.Eye, CFrame(Vector3{2.0f, 0.0f, 10.0f})});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK_THAT(mirror.Placed().X, Catch::Matchers::WithinAbs(2.0f, TOLERANCE));
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(-10.4f, TOLERANCE));
	CHECK(mirror.World.Get<Transform>(mirror.Eye)->Frame.Position == Vector3{0.0f, 0.0f, 20.0f});
	CHECK(mirror.World.Resource<ActiveCamera>()->Entity == mirror.Eye);
}

TEST_CASE("the plane sits at the half extent, not the full one", "[scene][surfacecameras]") {
	// `Bounds::HalfExtent` is half of a full extent - the whole reason `Size` is
	// a conversion rather than a member pointer. A plane placed at the full
	// extent sits a whole part outside the part it belongs to, and the
	// reflection lands nowhere near the pane.
	Mirror mirror(NormalId::Top);

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// Face at y = +4.5, eye at y = 0 - so 4.5 below it, and the reflection 4.5
	// above at y = 9. A full-extent plane would put it at y = 18.
	CHECK_THAT(mirror.Placed().Y, Catch::Matchers::WithinAbs(9.0f, TOLERANCE));
}

TEST_CASE("aiming tells the part which surface it shows", "[scene][surfacecameras]") {
	Mirror mirror;

	// **The step that makes this an instance rather than a configuration.**
	// Requiring `Surface` to be set by hand as well as parenting the camera is
	// one fact recorded twice, and its failure mode is a camera rendering
	// perfectly into a texture nothing samples - which looks exactly like a
	// mirror that does not work.
	REQUIRE(mirror.World.Get<Visual>(mirror.Pane)->Surface == -1);

	AimSurfaceCameras(mirror.World);

	CHECK(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);
}

TEST_CASE("the clip plane is the pane itself, not a near plane pushed out to it", "[scene][surfacecameras]") {
	// **This case used to assert the approximation, and now asserts the thing.**
	//
	// The reflected camera is behind the pane looking through it, so everything
	// between the two - the frame, the back of the pane, whatever the viewer
	// stands behind - would occlude the reflection. That used to be handled by
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

	// The pane faces -Z with its face at z = -0.2, and the eye is at z = +20 -
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
	// *between* it and the glass - the back of the wall, in a real scene - and
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
	// therefore does not produce a smaller or softer image - it produces a
	// hard-edged rectangle of reflection floating on a grey wall, which moves and
	// resizes as the viewer walks and reads as a mirror aimed at the wrong thing.
	//
	// A constant field of view cannot cover it. The camera stands as far behind
	// the glass as the viewer stands in front, so the pane subtends *the same
	// angle from the camera as from the viewer* - and that grows without bound as
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
	// texture - which is precisely the condition `opaque.frag` falls back on.
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
		// texture's shape is already inside the extents - which is exactly why
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
				// the whole pane would pass this too - it is a weaker condition,
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
	// - and a clamp that quietly cropped a mirror seen from across a room would
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

TEST_CASE("the fit follows the screen's shape and not only its height", "[scene][surfacecameras]") {
	// **`ActiveCamera::AspectRatio` had no writer outside this suite**, and the
	// case above could not see that because it supplies its own `16/9` at both
	// ends. Every real run left the field at its default `1.0`, so
	// `FrustumCorners` built a *square* viewer frustum, `FitExtents` clamped
	// every mirror against it, and the pane outside that square projected past
	// the texture's `0..1` rectangle - where `opaque.frag` draws the plain lit
	// pane. On the 1631x599 panel it was reported from that is 37% of the width
	// the viewer could actually see, and the rest was a hard vertical edge on
	// each side that read as a cull box. Nothing was culled.
	//
	// Vertical was always right, which is why nothing was ever cut top or
	// bottom: the field of view *is* the vertical one, and only the width is
	// derived from the aspect.
	Mirror mirror;

	// **Close, so the clamp actually binds.** From across a room the fit is
	// already far tighter than the eye's frustum and intersecting with it
	// changes nothing at any aspect - which is the case that must keep passing
	// and is asserted above. Here the pane is 16 wide and the square screen
	// reaches about 8.7 of it.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, 6.0f});

	// Whether everything the viewer can see of the pane lands inside the image,
	// which is exactly the condition the shader falls back on.
	const auto coveredOn = [&](float aspect) {
		const Transform *placed = mirror.World.Get<Transform>(mirror.Reflection);
		const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
		const Transform *eyePlaced = mirror.World.Get<Transform>(mirror.Eye);
		const Camera *eyeLens = mirror.World.Get<Camera>(mirror.Eye);
		REQUIRE(placed != nullptr);
		REQUIRE(fitted != nullptr);
		REQUIRE(eyePlaced != nullptr);
		REQUIRE(eyeLens != nullptr);

		const glm::mat4 viewProjection =
			engine::scene::ResolveSurfaceCamera(
				placed->Frame, engine::scene::SurfaceProjection(*fitted, placed->Frame)
			)
				.ViewProjection;

		const glm::mat4 screen =
			engine::scene::ResolveCamera(eyePlaced->Frame, *eyeLens, aspect).ViewProjection;

		bool covered = true;
		constexpr int STEPS = 16;

		for (int ix = 0; ix <= STEPS; ix++) {
			for (int iy = 0; iy <= STEPS; iy++) {
				const float x = -8.0f + 16.0f * static_cast<float>(ix) / STEPS;
				const float y = -4.5f + 9.0f * static_cast<float>(iy) / STEPS;
				const glm::vec4 point(x, y, -0.2f, 1.0f);

				const glm::vec4 onScreen = screen * point;
				if (!(onScreen.w > 0.0f) || std::abs(onScreen.x / onScreen.w) > 1.0f ||
					std::abs(onScreen.y / onScreen.w) > 1.0f) {
					continue;
				}

				const glm::vec4 clip = viewProjection * point;
				INFO("pane point " << x << ", " << y << " at aspect " << aspect);

				if (!(clip.w > 0.0f)) {
					covered = false;
					continue;
				}
				covered = covered && std::abs(clip.x / clip.w) <= 1.0f && std::abs(clip.y / clip.w) <= 1.0f;
			}
		}
		return covered;
	};

	// The panel the report came from.
	constexpr float WIDE = 1631.0f / 599.0f;

	REQUIRE(engine::scene::SetViewportSize(mirror.World, 1631, 599));
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(coveredOn(WIDE));

	// **And the failure it replaces, asserted rather than described.** With the
	// field back at the default that nothing used to write, the same screen
	// loses its sides - so this case is measuring the field and not merely the
	// arithmetic downstream of it.
	mirror.World.SetResource(ActiveCamera{mirror.Eye});
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK_FALSE(coveredOn(WIDE));

	// A square panel is still covered by a square clamp, which says the
	// difference above is the *shape* and not a fit that got wider by accident.
	REQUIRE(engine::scene::SetViewportSize(mirror.World, 800, 800));
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(coveredOn(1.0f));

	// Taller than it is wide, which no other case in this file exercises and
	// which a fix that hard-coded a widening would get wrong in the other
	// direction.
	REQUIRE(engine::scene::SetViewportSize(mirror.World, 600, 1000));
	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	CHECK(coveredOn(600.0f / 1000.0f));
}

TEST_CASE("a viewport size is recorded, and a degenerate one is refused", "[scene][surfacecameras]") {
	// **The writer the two programs call, checked where they cannot be.** A
	// minimised window reports zero height and a closed panel reports zero of
	// both; storing either is a zero or infinite aspect, which `ResolveCamera`
	// already refuses and which would leave every mirror in the world unfittable
	// for as long as the window stayed down. Keeping the last good value is what
	// makes coming back from minimised free.
	Mirror mirror;

	REQUIRE(engine::scene::SetViewportSize(mirror.World, 1631, 599));
	CHECK_THAT(
		mirror.World.Resource<ActiveCamera>()->AspectRatio,
		Catch::Matchers::WithinAbs(1631.0f / 599.0f, TOLERANCE)
	);
	const uint64_t settled = mirror.World.ChangeVersion();
	REQUIRE(engine::scene::SetViewportSize(mirror.World, 1631, 599));
	CHECK(mirror.World.ChangeVersion() == settled);

	CHECK_FALSE(engine::scene::SetViewportSize(mirror.World, 1024, 0));
	CHECK_FALSE(engine::scene::SetViewportSize(mirror.World, 0, 768));
	CHECK_FALSE(engine::scene::SetViewportSize(mirror.World, 0, 0));

	// Unchanged by any of the three.
	CHECK_THAT(
		mirror.World.Resource<ActiveCamera>()->AspectRatio,
		Catch::Matchers::WithinAbs(1631.0f / 599.0f, TOLERANCE)
	);
	REQUIRE(engine::scene::SetViewportSize(mirror.World, 800, 800));
	CHECK(mirror.World.Resource<ActiveCamera>()->AspectRatio == 1.0f);

	// A world with no camera named has nothing to tell, and says so rather than
	// creating one - a resource minted here would name a dead entity.
	Store bare("no-camera");
	CHECK_FALSE(engine::scene::SetViewportSize(bare, 1024, 768));
}

TEST_CASE("the frustum covers the pane when the viewer looks away from it", "[scene][surfacecameras]") {
	// **Every other fit case moves the eye and leaves it pointed at the pane.**
	// The fixture's viewer has identity rotation, which looks down -Z at a pane
	// facing -Z, so the pane has sat in the middle of the screen for every
	// assertion above. What a player does is turn their head, and the eye-corner
	// clamp - the intersection that buys the close-up sharpness - is a function
	// of where the screen points and not only of where it is.
	//
	// The failure is a *cull* rather than a stretch: `opaque.frag` falls back to
	// the plain lit pane wherever the projected coordinate leaves 0..1, so a
	// clamp that bites too hard takes a bite out of the reflection with a hard
	// edge, and the geometry it dropped is simply not in the picture.
	//
	// Swept rather than sampled at one angle, because the interesting number is
	// *where* it starts and a single case cannot report that.
	Mirror mirror;

	// Close enough that the pane fills much of the screen at every angle in the
	// sweep, which is what keeps the assertion from passing vacuously: a pane
	// entirely off screen has no points to cover.
	constexpr float STANDOFF = 3.0f;

	const auto coverageAt = [&](const Vector3 &at, float pitchDegrees, float yawDegrees) {
		constexpr float TO_RADIANS = 3.14159265f / 180.0f;
		mirror.World.GetMutable<Transform>(mirror.Eye)->Frame =
			CFrame(at, CFrame::Angles(pitchDegrees * TO_RADIANS, yawDegrees * TO_RADIANS, 0.0f).Rotation());
		const float degrees = std::abs(pitchDegrees) > std::abs(yawDegrees) ? pitchDegrees : yawDegrees;

		REQUIRE(AimSurfaceCameras(mirror.World) == 1);

		const Transform *placed = mirror.World.Get<Transform>(mirror.Reflection);
		const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
		REQUIRE(placed != nullptr);
		REQUIRE(fitted != nullptr);

		const glm::mat4 viewProjection =
			engine::scene::ResolveSurfaceCamera(
				placed->Frame, engine::scene::SurfaceProjection(*fitted, placed->Frame)
			)
				.ViewProjection;

		const Transform *eyePlaced = mirror.World.Get<Transform>(mirror.Eye);
		const Camera *eyeLens = mirror.World.Get<Camera>(mirror.Eye);
		REQUIRE(eyePlaced != nullptr);
		REQUIRE(eyeLens != nullptr);

		const glm::mat4 screen =
			engine::scene::ResolveCamera(eyePlaced->Frame, *eyeLens, 16.0f / 9.0f).ViewProjection;

		bool covered = true;
		size_t onScreenPoints = 0;
		constexpr int STEPS = 16;

		for (int ix = 0; ix <= STEPS; ix++) {
			for (int iy = 0; iy <= STEPS; iy++) {
				const float x = -8.0f + 16.0f * static_cast<float>(ix) / STEPS;
				const float y = -4.5f + 9.0f * static_cast<float>(iy) / STEPS;
				const glm::vec4 point(x, y, -0.2f, 1.0f);

				const glm::vec4 onScreen = screen * point;
				if (!(onScreen.w > 0.0f) || std::abs(onScreen.x / onScreen.w) > 1.0f ||
					std::abs(onScreen.y / onScreen.w) > 1.0f) {
					continue;
				}
				onScreenPoints++;

				const glm::vec4 clip = viewProjection * point;
				if (!(clip.w > 0.0f)) {
					INFO(
						"at " << degrees << " degrees, pane point " << x << ", " << y
							  << " is behind the surface camera"
					);
					covered = false;
					continue;
				}

				const bool inside = std::abs(clip.x / clip.w) <= 1.0f && std::abs(clip.y / clip.w) <= 1.0f;
				if (!inside) {
					INFO(
						"at " << degrees << " degrees, pane point " << x << ", " << y
							  << " is on screen but outside the reflection"
					);
				}
				covered = covered && inside;
			}
		}

		INFO("at " << degrees << " degrees, " << onScreenPoints << " pane points were on screen");
		return covered;
	};

	// **A body angle rather than a pane angle.** These are degrees the viewer has
	// turned away from the pane, so zero is the case every test above already
	// covers and eighty is past where the pane leaves the screen entirely.
	//
	// Both signs on both axes, because the clamp's four edges are independent and
	// a sign error shows on one of them only. Pitch from a raised eye as well as
	// a level one: a viewer floating above the floor looking down is the position
	// the tilt actually happens from, and it is the one that puts the pane's
	// corners nearest the camera's own plane.
	for (const float degrees :
		 {0.0f,
		  20.0f,
		  40.0f,
		  50.0f,
		  55.0f,
		  60.0f,
		  70.0f,
		  80.0f,
		  -20.0f,
		  -40.0f,
		  -50.0f,
		  -55.0f,
		  -60.0f,
		  -70.0f,
		  -80.0f}) {
		INFO("viewer yawed " << degrees << " degrees off the pane");
		CHECK(coverageAt(Vector3{0.0f, 0.0f, STANDOFF}, 0.0f, degrees));

		INFO("viewer pitched " << degrees << " degrees off the pane");
		CHECK(coverageAt(Vector3{0.0f, 0.0f, STANDOFF}, degrees, 0.0f));

		INFO("viewer floating, pitched " << degrees << " degrees off the pane");
		CHECK(coverageAt(Vector3{0.0f, 6.0f, STANDOFF}, degrees, 0.0f));

		INFO("viewer floating and off to one side, turned " << degrees << " degrees");
		CHECK(coverageAt(Vector3{5.0f, 6.0f, STANDOFF}, degrees * 0.5f, degrees));
	}
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

	// Square on, it is symmetric again - so the lean is a response to where the
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
	// - a reflection covering half a turn, which no projection covers and which
	// `tan` answers with infinity. Clamping produced a finite matrix for a view
	// nobody can see: a pane edge-on subtends no pixels.
	//
	// So the answer is now that there is no reflection to draw, which is what
	// removes the flash rather than bounding it - see `EDGE_ON_MARGIN`.
	Mirror mirror;
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{0.0f, 0.0f, -0.2f});

	CHECK(AimSurfaceCameras(mirror.World) == 0);

	// **The pane has to be told, not merely the camera.** A surface left holding
	// its slot goes on sampling whatever was last rendered into it, which is a
	// frozen reflection rather than a blank one - worse than the bug, because it
	// is a picture of somewhere the viewer is no longer standing.
	const Visual *pane = mirror.World.Get<Visual>(mirror.Pane);
	REQUIRE(pane != nullptr);
	CHECK(pane->Surface == -1);
}

TEST_CASE("a pane whose camera is gone stops sampling its slot", "[scene][surfacecameras]") {
	// **A mirror deleted in the editor leaves its pane behind.** The walk only
	// visits live `SurfaceCamera` rows, so nothing would visit that pane again
	// and the slot it was handed would be the last thing it was told - while the
	// renderer's texture for that slot still holds the frame the camera drew.
	// The result is a frozen reflection of a room the viewer has walked out of,
	// standing exactly where the mirror was. Same failure as the edge-on band
	// above, by a different route.
	Mirror mirror;

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	REQUIRE(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);

	mirror.World.DestroyInstance(mirror.Reflection);

	CHECK(AimSurfaceCameras(mirror.World) == 0);

	const Visual *pane = mirror.World.Get<Visual>(mirror.Pane);
	REQUIRE(pane != nullptr);
	CHECK(pane->Surface == -1);
}

TEST_CASE("a pane whose camera component was removed stops sampling", "[scene][surfacecameras]") {
	// The other route to the same state, and it is the one `Part.cpp` takes when
	// a script drops `SurfaceSize` below a texel: the instance stays, the
	// `SurfaceCamera` row goes. Nothing is destroyed, so a fix that hung off
	// entity destruction would miss this entirely.
	Mirror mirror;

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	REQUIRE(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);

	mirror.World.Remove<SurfaceCamera>(mirror.Reflection);

	CHECK(AimSurfaceCameras(mirror.World) == 0);
	CHECK(mirror.World.Get<Visual>(mirror.Pane)->Surface == -1);
}

TEST_CASE("a world that loses its viewer leaves its panes alone", "[scene][surfacecameras]") {
	// **The deliberate hole in the sweep, asserted so it stays deliberate.**
	// With no active camera the aim returns before it can hand anything out, and
	// it does *not* release the panes: a world with no viewer draws nothing, so
	// there is no picture for a pane to be stuck showing. A replica between
	// connecting and spawning its camera sits in exactly this state while the
	// authority is still sending it `Visual` rows carrying the slot, and a clear
	// here would fight the wire once per snapshot for a frame nobody renders.
	// `RenderView` will not sample a slot nothing claimed, which is what covers
	// a world that never gets a viewer back.
	Mirror mirror;

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);
	REQUIRE(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);

	mirror.World.DestroyInstance(mirror.Eye);

	CHECK(AimSurfaceCameras(mirror.World) == 0);
	CHECK(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);
}

TEST_CASE("a live mirror keeps its slot across repeated aims", "[scene][surfacecameras]") {
	// The other half of the release, and the reason it is counted before it
	// sweeps: a pane that is still aimed at must survive every frame. A sweep
	// that cleared on the strength of "this pane holds a slot" alone would blank
	// every working mirror in the engine on the frame after it started drawing.
	Mirror mirror;

	for (int frame = 0; frame < 4; frame++) {
		REQUIRE(AimSurfaceCameras(mirror.World) == 1);
		CHECK(mirror.World.Get<Visual>(mirror.Pane)->Surface == 0);
		CHECK(mirror.World.Get<SurfaceCamera>(mirror.Reflection)->Surface == 0);
	}
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
	// case did not do.** It left the viewer at +Z, and the rotated normal is -X -
	// so the eye was exactly *level* with the plane, the mirrored position was
	// the eye itself, and the assertion "Z did not move" passed for a reflection
	// that was never computed. Since `EDGE_ON_MARGIN` that arrangement draws
	// nothing, which is what turned this into a failure and showed up the
	// fixture.
	mirror.World.GetMutable<Transform>(mirror.Eye)->Frame = CFrame(Vector3{-20.0f, 0.0f, 0.0f});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	// The face is at x = -0.2 and the eye 19.8 in front of it, so the reflection
	// lands the same distance behind: -20 + 2 × 19.8. **The real assertion is
	// still that nothing moved along Z** - a version reflecting through the world
	// axis rather than the part's own would have swung the camera along it.
	CHECK_THAT(mirror.Placed().X, Catch::Matchers::WithinAbs(19.6f, TOLERANCE));
	CHECK_THAT(mirror.Placed().Z, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
}

TEST_CASE("the face marker lies on the face the camera projects off", "[scene][surfacecameras]") {
	Mirror mirror;

	std::vector<engine::scene::DrawInstance> list;
	REQUIRE(engine::scene::AppendSurfaceFaceMarkers(mirror.World, list) == 1);
	REQUIRE(list.size() == 1);

	// The same plane the reflection is computed through - face at z = -0.2 -
	// pushed one thickness clear of it so the two do not z-fight. **The sign is
	// the whole assertion**: a marker at z = +0.2 would be sitting on the back
	// of the pane, which is a debugging aid that points at the wrong face and is
	// worse than none at all.
	const engine::scene::DrawInstance &marker = list.front();
	CHECK_THAT(marker.Frame.Position.X, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(marker.Frame.Position.Y, Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK(marker.Frame.Position.Z < -0.2f);

	// Along the pane's *longer* in-plane axis - 8 wide against 4.5 tall - and
	// thin on the other two. A bar across the short axis is a dash somebody has
	// to look for.
	CHECK(marker.HalfExtent.X > marker.HalfExtent.Y);
	CHECK(marker.HalfExtent.X > marker.HalfExtent.Z);
	CHECK(marker.HalfExtent.X < 8.0f);

	// Blended, and that is load-bearing rather than cosmetic: the surface pass
	// draws only the opaque head, so an opaque marker would appear across the
	// glass inside every other mirror in the scene. It casts nothing for the
	// matching reason - a bar on the floor describes the scene it is meant to be
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
	// the other viewport's camera** - each panel writes the same `ActiveCamera`
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
	// frame and the next - then back on the frame after. The projection was
	// never wrong. The fit was discontinuous, and a discontinuity once per orbit
	// is exactly what a flash is.
	//
	// **Tested as a bound on the frame-to-frame *change*, not on the value.**
	// The value is allowed to grow enormously - a corner going edge-on genuinely
	// needs the widest frustum there is - and asserting a ceiling would forbid
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
		// centred on - the motion the flash was reported under.
		mirror.World.Set<Transform>(
			mirror.Eye, Transform{CFrame(Vector3{std::sin(angle) * RADIUS, 0.0f, std::cos(angle) * RADIUS})}
		);

		// **Continuity is only asked of frames that draw**, which is the whole of
		// D00027's fix expressed as a measurement. Crossing the plane, the pane
		// goes blank for a few samples and comes back aimed the other way - the
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
		// any more - a constant, which passes this case without looking at
		// anything. It did, until this was changed.
		//
		// **An angle rather than the raw extents, and that is not cosmetic.**
		// `Right - Left` is a *tangent*: as the reflected camera approaches the
		// pane's plane the pane subtends most of a half-turn and the span grows
		// without bound, so a smooth sweep near the crossing shows enormous
		// steps in it - this case measured exactly that and reported 13 at the
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
		// origin crosses it twice a lap - so the reflected camera whips 180° and
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
	// now, so there is no edge to step across - but the assertion is kept, and
	// kept as a bound on the *change* rather than on the value, because the
	// depth floor is still a place a discontinuity could hide.
	CHECK(worst < 0.25f);

	// **And here is the flash, fixed rather than measured.** This bound used to
	// be `<= 2.001` - asserting the bug, because a look vector changing by
	// exactly 2.0 is a 180 degree turn in one frame, once a lap, where `facing`
	// flips sign as the viewer crosses the pane's plane.
	//
	// It is now *zero* across every visible frame, and that falls out of the fix
	// rather than being tuned to: within one side of the plane `facing` is
	// constant, so the reflected camera's orientation does not change at all as
	// the viewer orbits - only its position does. The band where the sign would
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
	// the plane at all - so the fix has to be shown doing something, not merely
	// failing to do the wrong thing. Twice a lap, a handful of samples each.
	INFO("blank samples " << blank << " of " << SAMPLES);
	CHECK(blank > 0);
	CHECK(blank < SAMPLES / 4);
}
