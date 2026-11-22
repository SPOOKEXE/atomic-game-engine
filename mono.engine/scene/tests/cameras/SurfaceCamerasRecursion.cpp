// Mirror rules away from the world: reflecting from another mirror's camera,
// lens fitting, pane gathering, recursion depth and surface limits.

#include "SurfaceCamerasFixture.hpp"

TEST_SUITE_ID("engine.scene.surfacecamerasrecursion")
TEST_DEPENDS("engine.scene.surfacecameras")

// The rule on its own, away from the world it is normally read out of.
//
// **What these cases can say and the ones above cannot**, which is the whole
// reason `ReflectCamera` exists: `AimSurfaceCameras` places every mirror in the
// world from the world's *one* active camera, so nothing above can ask what a
// pane does to a viewer that is not the eye - and a mirror seen inside another
// mirror is looked at from that mirror's camera rather than from the eye.

namespace {
	// A pane stated by hand, for the cases that are about the rule rather than
	// about the tree it was gathered out of.
	//
	// Sixteen by nine, on whichever two axes the normal is not on, which is what
	// `FaceAxes` would have produced for the same face.
	engine::scene::SurfacePane PaneAt(const Vector3 &centre, const Vector3 &normal) {
		engine::scene::SurfacePane pane;
		pane.Centre = centre;
		pane.Normal = normal;
		pane.First = std::abs(normal.X) > 0.5f ? Vector3{0.0f, 0.0f, 8.0f} : Vector3{8.0f, 0.0f, 0.0f};
		pane.Second = std::abs(normal.Y) > 0.5f ? Vector3{0.0f, 0.0f, 4.5f} : Vector3{0.0f, 4.5f, 0.0f};
		return pane;
	}

	// Where planar reflection through a plane sends a point, written out here so
	// the expectations below are not the implementation quoted back at itself.
	Vector3 MirroredThrough(const Vector3 &centre, const Vector3 &normal, const Vector3 &at) {
		return at - normal * (2.0f * (at - centre).Dot(normal));
	}
}

TEST_CASE("a mirror seen in a mirror is reflected from that mirror's camera", "[scene][surfacecameras]") {
	// **The defect, as arithmetic rather than as a device artefact.** Two panes
	// facing each other and an eye between them: pane B appears inside pane A's
	// reflection, so the camera that draws *that* copy of B has to be B's rule
	// applied to A's camera - not to the eye. While the rule was reachable only
	// from inside a walk over `ActiveCamera`, the renderer had one answer
	// available to it and it was the wrong one, which is `ROADMAP.md` v0.15's
	// mirror-in-mirror-in-mirror drawing its inner panes as flat tint.
	//
	// The two answers are a fixed translation apart, so this is not a tolerance
	// question: worked through below rather than read off the run.
	//
	// A is at the origin facing +Z. B is at z = 10 facing -Z. The eye is at
	// (1, 2, 4), which is four in front of A and six in front of B.
	//
	//   - Through A: 4 in front becomes 4 behind, at (1, 2, -4).
	//   - Through B, from the eye: 6 in front becomes 6 behind, at (1, 2, 16).
	//   - Through B, from A's camera at z = -4: that is 14 in front of B, so it
	//     lands 14 behind, at (1, 2, 24).
	//
	// Eight studs apart, and eight is exactly what a reflection predicts of the
	// gap: the two viewers are 8 apart along B's normal, and a reflection sends
	// a difference `d` to `d - 2n(n·d)`, which flips that component and leaves
	// the rest.
	const engine::scene::SurfacePane paneA = PaneAt(Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f});
	const engine::scene::SurfacePane paneB = PaneAt(Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.0f, 0.0f, -1.0f});

	const CFrame eye{Vector3{1.0f, 2.0f, 4.0f}};

	const engine::scene::MirrorEye inA = engine::scene::ReflectCamera(paneA, eye, {});
	REQUIRE(inA.Renders);
	CHECK_THAT(inA.Frame.Position.Z, Catch::Matchers::WithinAbs(-4.0f, TOLERANCE));

	const engine::scene::MirrorEye composed = engine::scene::ReflectCamera(paneB, inA.Frame, {});
	const engine::scene::MirrorEye direct = engine::scene::ReflectCamera(paneB, eye, {});
	REQUIRE(composed.Renders);
	REQUIRE(direct.Renders);

	// **Different, which is the whole claim.** An implementation that ignored
	// the viewer and reached for the world's eye would put these in one place,
	// and the picture would be B drawn from where nobody is looking at it from.
	CHECK((composed.Frame.Position - direct.Frame.Position).Magnitude() > 1.0f);

	CHECK_THAT(composed.Frame.Position.X, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));
	CHECK_THAT(composed.Frame.Position.Y, Catch::Matchers::WithinAbs(2.0f, TOLERANCE));
	CHECK_THAT(composed.Frame.Position.Z, Catch::Matchers::WithinAbs(24.0f, TOLERANCE));
	CHECK_THAT(direct.Frame.Position.Z, Catch::Matchers::WithinAbs(16.0f, TOLERANCE));

	// And the difference is the translation planar reflection predicts, stated
	// as the general rule rather than as the one coordinate above - a sign error
	// in the implementation would still land 8 apart, and would land the pair on
	// the wrong side of B.
	const Vector3 between = inA.Frame.Position - eye.Position;
	const Vector3 predicted = between - paneB.Normal * (2.0f * between.Dot(paneB.Normal));
	const Vector3 measured = composed.Frame.Position - direct.Frame.Position;
	CHECK_THAT((measured - predicted).Magnitude(), Catch::Matchers::WithinAbs(0.0f, TOLERANCE));

	// **Composing is reflecting twice**, which is what makes a chain of panes a
	// chain rather than a set of independent guesses.
	const Vector3 twice = MirroredThrough(
		paneB.Centre, paneB.Normal, MirroredThrough(paneA.Centre, paneA.Normal, eye.Position)
	);
	CHECK_THAT((composed.Frame.Position - twice).Magnitude(), Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
}

TEST_CASE(
	"the aim pass answers for the eye, which is the answer a recursion cannot use", "[scene][surfacecameras]"
) {
	// **The same defect stated against a real world rather than two panes made
	// up in a test.** `AimSurfaceCameras` places every surface from the world's
	// one `ActiveCamera`, which is exactly right for what the *screen* shows and
	// is the wrong camera for a pane appearing inside another pane's picture.
	// That is not a fault in this pass - there is one screen and it has one eye
	// - it is why the rule had to become something a recursive pass can ask
	// about a viewer of its own.
	Store world{"surfacecameras"};
	engine::scene::RegisterSceneClasses();

	// Two panes facing each other down Z with the eye between them. A is at the
	// origin showing its `Back` face, so its normal is +Z; B is ten along
	// showing `Front`, so its normal is -Z.
	const auto build = [&](const char *name, const Vector3 &at, NormalId face) {
		const Entity part =
			world.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), name);
		world.Set<Transform>(part, Transform{CFrame(at)});
		world.Set<Bounds>(part, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

		const Entity camera =
			world.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), name);
		SurfaceCamera target;
		target.Face = face;
		world.Set<SurfaceCamera>(camera, target);
		world.SetParent(camera, part);
		return camera;
	};

	const Entity inA = build("A", Vector3::Zero, NormalId::Back);
	const Entity inB = build("B", Vector3{0.0f, 0.0f, 10.0f}, NormalId::Front);

	const Entity eye = world.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Camera")), "Eye");
	world.Set<Transform>(eye, Transform{CFrame(Vector3{1.0f, 2.0f, 4.0f})});
	world.SetResource(ActiveCamera{eye, 16.0f / 9.0f});

	REQUIRE(AimSurfaceCameras(world) == 2);

	std::vector<engine::scene::SurfacePane> panes;
	REQUIRE(engine::scene::GatherSurfacePanes(world, panes) == 2);

	const auto paneOf = [&](Entity camera) {
		for (const engine::scene::SurfacePane &found : panes) {
			if (found.Camera == camera) {
				return found;
			}
		}
		FAIL("no pane gathered for the camera");
		return panes[0];
	};

	const Transform *eyePlaced = world.Get<Transform>(eye);
	const Camera *eyeLens = world.Get<Camera>(eye);
	REQUIRE(eyePlaced != nullptr);
	REQUIRE(eyeLens != nullptr);

	Vector3 corners[4];
	const size_t cornerCount =
		engine::scene::FrustumCorners(eyePlaced->Frame, eyeLens->FieldOfViewRadians, 16.0f / 9.0f, corners);
	const std::span<const Vector3> screen(corners, cornerCount);

	// **What the pass wrote for B is B reflected from the eye**, bit for bit.
	const engine::scene::MirrorEye direct =
		engine::scene::ReflectCamera(paneOf(inB), eyePlaced->Frame, screen);
	REQUIRE(direct.Renders);
	CHECK(std::memcmp(&world.Get<Transform>(inB)->Frame, &direct.Frame, sizeof(CFrame)) == 0);

	// **And the copy of B seen inside A needs B reflected from A's own camera**,
	// which is somewhere else - so a pass that reached for the stored
	// `Transform` at every depth samples the inner pane with a matrix fitted to
	// a camera that is not looking at it, the coordinate leaves 0..1, and
	// `opaque.frag` falls back to the plain lit pane.
	const engine::scene::MirrorEye first =
		engine::scene::ReflectCamera(paneOf(inA), eyePlaced->Frame, screen);
	REQUIRE(first.Renders);

	Vector3 inner[4];
	const size_t innerCount = engine::scene::FrustumCorners(first.Frame, first.Lens, inner);
	const engine::scene::MirrorEye composed =
		engine::scene::ReflectCamera(paneOf(inB), first.Frame, std::span<const Vector3>(inner, innerCount));
	REQUIRE(composed.Renders);

	// The gap between the two viewers, reflected through B - the same prediction
	// the panes-only case makes, arrived at from the tree instead.
	const Vector3 between = first.Frame.Position - eyePlaced->Frame.Position;
	const engine::scene::SurfacePane far = paneOf(inB);
	const Vector3 predicted = between - far.Normal * (2.0f * between.Dot(far.Normal));
	const Vector3 measured = composed.Frame.Position - direct.Frame.Position;

	CHECK(measured.Magnitude() > 1.0f);
	CHECK_THAT((measured - predicted).Magnitude(), Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
}

TEST_CASE(
	"a reflected camera stands as far behind the pane as the viewer stands in front",
	"[scene][surfacecameras]"
) {
	// The whole of planar reflection, and the reason the image lines up with the
	// pane instead of sliding across it as the viewer moves.
	const engine::scene::SurfacePane pane = PaneAt(Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f});

	const engine::scene::MirrorEye front =
		engine::scene::ReflectCamera(pane, CFrame(Vector3{3.0f, 1.0f, 7.0f}), {});
	REQUIRE(front.Renders);
	CHECK_THAT(front.Frame.Position.X, Catch::Matchers::WithinAbs(3.0f, TOLERANCE));
	CHECK_THAT(front.Frame.Position.Y, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));
	CHECK_THAT(front.Frame.Position.Z, Catch::Matchers::WithinAbs(-7.0f, TOLERANCE));

	// **Aimed, not merely placed.** A camera behind the pane with an identity
	// rotation faces away from it and renders empty space.
	CHECK_THAT(front.Frame.LookVector().Z, Catch::Matchers::WithinAbs(1.0f, 1e-3f));

	// **And a pane can be looked at from behind, where both answers are right.**
	// The camera lands on the viewer's side of the glass and looks the other
	// way, which is the sign `facing` carries.
	const engine::scene::MirrorEye behind =
		engine::scene::ReflectCamera(pane, CFrame(Vector3{3.0f, 1.0f, -7.0f}), {});
	REQUIRE(behind.Renders);
	CHECK_THAT(behind.Frame.Position.Z, Catch::Matchers::WithinAbs(7.0f, TOLERANCE));
	CHECK_THAT(behind.Frame.LookVector().Z, Catch::Matchers::WithinAbs(-1.0f, 1e-3f));

	// A mirror in the floor, which is the one direction `LookAt` cannot resolve
	// against the default up vector - and a NaN rotation spreads into the frame,
	// the clip plane and every bound derived from them.
	const engine::scene::SurfacePane floor = PaneAt(Vector3::Zero, Vector3{0.0f, 1.0f, 0.0f});
	const engine::scene::MirrorEye above =
		engine::scene::ReflectCamera(floor, CFrame(Vector3{0.0f, 5.0f, 0.0f}), {});
	REQUIRE(above.Renders);
	CHECK_THAT(above.Frame.Position.Y, Catch::Matchers::WithinAbs(-5.0f, TOLERANCE));
	CHECK(std::isfinite(above.Frame.LookVector().Y));
	CHECK_THAT(above.Frame.LookVector().Y, Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("a viewer in the edge-on band gets no camera rather than infinities", "[scene][surfacecameras]") {
	// **`EDGE_ON_MARGIN`, as a property of the rule rather than of the pass.**
	// At the plane there is no continuous orientation to aim for: the answer
	// flips with the side, both are right, and no path joins them - so the
	// honest answer is that a pane covering no pixels has nothing to show.
	const engine::scene::SurfacePane pane = PaneAt(Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f});

	const engine::scene::MirrorEye level = engine::scene::ReflectCamera(pane, CFrame(Vector3::Zero), {});
	CHECK_FALSE(level.Renders);

	// Either side of it, and a hair outside it, which is what makes the band a
	// band rather than a rejection of the plane itself.
	CHECK_FALSE(engine::scene::ReflectCamera(pane, CFrame(Vector3{0.0f, 0.0f, 0.1f}), {}).Renders);
	CHECK_FALSE(engine::scene::ReflectCamera(pane, CFrame(Vector3{0.0f, 0.0f, -0.1f}), {}).Renders);
	CHECK(engine::scene::ReflectCamera(pane, CFrame(Vector3{0.0f, 0.0f, 0.5f}), {}).Renders);
	CHECK(engine::scene::ReflectCamera(pane, CFrame(Vector3{0.0f, 0.0f, -0.5f}), {}).Renders);

	// **And nothing it hands back is a NaN or an infinity**, which is the half
	// that matters to a caller that renders anyway: a blank aim travels through
	// the rest of a pass, and a matrix full of infinities poisons every bound
	// derived from it.
	CHECK(std::isfinite(level.Frame.Position.X));
	CHECK(std::isfinite(level.Frame.Position.Y));
	CHECK(std::isfinite(level.Frame.Position.Z));
	CHECK(std::isfinite(level.Lens.Left));
	CHECK(std::isfinite(level.Lens.Right));
	CHECK(std::isfinite(level.Lens.Bottom));
	CHECK(std::isfinite(level.Lens.Top));
}

TEST_CASE("the fitted lens covers the whole pane it was fitted to", "[scene][surfacecameras]") {
	// **The property `FitExtents` says the whole feature depends on.** The image
	// is projected back onto the pane per fragment and `opaque.frag` tests the
	// projected coordinate against the texture's 0..1 rectangle, falling through
	// to the plain lit pane outside it - so a frustum that does not cover the
	// pane draws a hard-edged rectangle of reflection floating on a grey wall.
	//
	// Unclamped here, with no viewer frustum handed in, because that is the
	// condition being asserted: every corner of the pane, at every distance,
	// however far off to one side the viewer stands. The clamped form is what
	// `the frustum covers the whole pane` above measures against a real screen.
	const engine::scene::SurfacePane pane = PaneAt(Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f});

	const auto covers = [&](const Vector3 &from) {
		const engine::scene::MirrorEye eye = engine::scene::ReflectCamera(pane, CFrame(from), {});
		REQUIRE(eye.Renders);

		const glm::mat4 viewProjection = engine::scene::ResolveSurfaceCamera(
											 eye.Frame, engine::scene::SurfaceProjection(eye.Lens, eye.Frame)
		)
											 .ViewProjection;

		bool covered = true;
		for (const float alongFirst : {-1.0f, 1.0f}) {
			for (const float alongSecond : {-1.0f, 1.0f}) {
				const Vector3 corner = pane.Centre + pane.First * alongFirst + pane.Second * alongSecond;
				const glm::vec4 clip = viewProjection * glm::vec4(corner.X, corner.Y, corner.Z, 1.0f);
				INFO(
					"viewer " << from.X << ", " << from.Y << ", " << from.Z << " corner " << corner.X << ", "
							  << corner.Y << " has w " << clip.w
				);

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

	// Across the room, where no constant field of view is wrong yet.
	CHECK(covers(Vector3{0.0f, 0.0f, 40.0f}));

	// Two studs out, where the pane needs about 150 degrees and the authored 70
	// covered a third of it.
	CHECK(covers(Vector3{0.0f, 0.0f, 2.0f}));

	// Close *and* off to one side, which is the case a symmetric frustum has to
	// widen for rather than lean into - and where aiming at the pane's centre
	// instead of along its normal puts the nearest corner behind the camera.
	CHECK(covers(Vector3{7.0f, 4.0f, 1.0f}));
	CHECK(covers(Vector3{-7.0f, -4.0f, 0.6f}));

	// And from behind the glass, since a pane is looked at from either side.
	CHECK(covers(Vector3{5.0f, 0.0f, -3.0f}));
}

TEST_CASE("gathering finds the mirrors and leaves the linked portals alone", "[scene][surfacecameras]") {
	// **A linked portal is not a mirror, and the split is the point.** Its
	// camera is a warp rather than a reflection, so a recursion handed both
	// descriptions of one pane would have two answers for where the camera goes.
	// An *unlinked* portal is a mirror, by the same rule the aim pass applies: a
	// hole leading nowhere is a wall.
	Mirror mirror;

	std::vector<engine::scene::SurfacePane> panes;
	REQUIRE(engine::scene::GatherSurfacePanes(mirror.World, panes) == 1);

	// The face, measured the way the aim pass measures it: the pane faces -Z with
	// a half extent of 0.2, so its face is at z = -0.2 rather than at the part's
	// own centre.
	CHECK(panes[0].Camera == mirror.Reflection);
	CHECK(panes[0].Part == mirror.Pane);
	CHECK_THAT(panes[0].Centre.Z, Catch::Matchers::WithinAbs(-0.2f, TOLERANCE));
	CHECK_THAT(panes[0].Normal.Z, Catch::Matchers::WithinAbs(-1.0f, TOLERANCE));
	CHECK_THAT(panes[0].First.Magnitude(), Catch::Matchers::WithinAbs(8.0f, TOLERANCE));
	CHECK_THAT(panes[0].Second.Magnitude(), Catch::Matchers::WithinAbs(4.5f, TOLERANCE));

	// The author's lens, carried rather than replaced.
	CHECK_THAT(panes[0].NearPlane, Catch::Matchers::WithinAbs(Camera{}.NearPlane, TOLERANCE));
	CHECK_THAT(panes[0].FarPlane, Catch::Matchers::WithinAbs(Camera{}.FarPlane, TOLERANCE));

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	CHECK(engine::scene::GatherSurfacePanes(mirror.World, panes) == 0);

	// The other half of the split, so this is a hand-off rather than a pane
	// nobody claims.
	std::vector<engine::scene::PortalSeam> seams;
	CHECK(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);

	// A destination that was destroyed falls back to a mirror, and the gather has
	// to agree with the aim pass about that or a recursion would draw a hole
	// nothing places a camera for.
	mirror.World.Destroy(far);
	CHECK(engine::scene::GatherSurfacePanes(mirror.World, panes) == 1);
}

TEST_CASE("what the aim pass writes is what the rule says", "[scene][surfacecameras]") {
	// **One statement of what a mirror does to a camera, pinned.** The comments
	// in `AimSurfaceCameras` argue that a second derivation is a second chance to
	// disagree - this is that argument as an assertion, and it is what stops the
	// lift being undone by a well-meaning inline of it later.
	//
	// Compared **bitwise**, which is the right comparison rather than a lazy
	// one: the same inputs through the same code give the same bits, and a
	// tolerance would be answering a different question. It is the comparison
	// the pass itself makes before deciding whether to write.
	// **Close and off to one side, so the eye-corner clamp actually binds.** At
	// any ordinary distance the fit is already far tighter than the viewer's own
	// frustum and the intersection changes nothing - which would make the
	// comparison below pass while saying nothing about the corners at all.
	Mirror mirror;
	mirror.World.Set<Transform>(mirror.Eye, Transform{CFrame(Vector3{2.0f, 1.0f, 3.0f})});

	REQUIRE(AimSurfaceCameras(mirror.World) == 1);

	std::vector<engine::scene::SurfacePane> panes;
	REQUIRE(engine::scene::GatherSurfacePanes(mirror.World, panes) == 1);

	const Transform *eyePlaced = mirror.World.Get<Transform>(mirror.Eye);
	const Camera *eyeLens = mirror.World.Get<Camera>(mirror.Eye);
	REQUIRE(eyePlaced != nullptr);
	REQUIRE(eyeLens != nullptr);

	Vector3 corners[4];
	const size_t cornerCount =
		engine::scene::FrustumCorners(eyePlaced->Frame, eyeLens->FieldOfViewRadians, 16.0f / 9.0f, corners);
	REQUIRE(cornerCount == 4);

	const engine::scene::MirrorEye stated = engine::scene::ReflectCamera(
		panes[0], eyePlaced->Frame, std::span<const Vector3>(corners, cornerCount)
	);
	REQUIRE(stated.Renders);

	const Transform *placed = mirror.World.Get<Transform>(mirror.Reflection);
	const SurfaceLens *fitted = mirror.World.Get<SurfaceLens>(mirror.Reflection);
	REQUIRE(placed != nullptr);
	REQUIRE(fitted != nullptr);

	CHECK(std::memcmp(&placed->Frame, &stated.Frame, sizeof(CFrame)) == 0);
	CHECK(std::memcmp(fitted, &stated.Lens, sizeof(SurfaceLens)) == 0);

	// **And the clamp was actually exercised**, which the bitwise compare cannot
	// say on its own: an eye frustum passed to one side and dropped on the other
	// would still match if the fit never bound against it. A pane this close
	// fills far more than the screen, so the two answers differ.
	const engine::scene::MirrorEye unclamped = engine::scene::ReflectCamera(panes[0], eyePlaced->Frame, {});
	REQUIRE(unclamped.Renders);
	CHECK(std::memcmp(&unclamped.Lens, &stated.Lens, sizeof(SurfaceLens)) != 0);
}

TEST_CASE(
	"a fitted lens is a frustum the next level down can be clamped against", "[scene][surfacecameras]"
) {
	// **The overload that makes a recursion possible past its first level.** A
	// reflected camera has no field of view - its extents were fitted to a pane
	// - so a level that could only take an angle would drop the clamp exactly
	// where the pane is nearest and the texels scarcest.
	const engine::scene::SurfacePane pane = PaneAt(Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f});
	const engine::scene::MirrorEye eye =
		engine::scene::ReflectCamera(pane, CFrame(Vector3{0.0f, 0.0f, 6.0f}), {});
	REQUIRE(eye.Renders);

	Vector3 corners[4];
	REQUIRE(engine::scene::FrustumCorners(eye.Frame, eye.Lens, corners) == 4);

	// **One unit deep, which is the convention the two overloads share.** The
	// fit divides each direction by its own depth, so a scale disagreement shows
	// up only against the floor that keeps a corner swinging past the camera's
	// plane finite - the one place nobody would look.
	const Vector3 forward = eye.Frame.LookVector();
	for (const Vector3 &corner : corners) {
		CHECK_THAT(corner.Dot(forward), Catch::Matchers::WithinAbs(1.0f, TOLERANCE));
	}

	// And they are the corners of that lens rather than of some other one: each
	// projects to a corner of the clip box.
	const glm::mat4 viewProjection =
		engine::scene::ResolveSurfaceCamera(eye.Frame, engine::scene::SurfaceProjection(eye.Lens, eye.Frame))
			.ViewProjection;

	for (const Vector3 &corner : corners) {
		const Vector3 at = eye.Frame.Position + corner;
		const glm::vec4 clip = viewProjection * glm::vec4(at.X, at.Y, at.Z, 1.0f);
		REQUIRE(clip.w > 0.0f);
		CHECK_THAT(std::abs(clip.x / clip.w), Catch::Matchers::WithinAbs(1.0f, 1e-3f));
		CHECK_THAT(std::abs(clip.y / clip.w), Catch::Matchers::WithinAbs(1.0f, 1e-3f));
	}
}

namespace {
	// The renderer's mirror recursion, with the device and the culler taken out.
	//
	// **A model rather than a second implementation, and what it calls is the
	// difference.** Every camera below comes out of `ReflectCamera`, which is
	// the one statement of what a mirror does to a camera, so this cannot drift
	// from the pass by a sign - the same argument `AimSurfaceCameras` is checked
	// against. What is deliberately missing is `graph::VisiblePane`, which lives
	// a tier above this module: every pane counts as visible here, which is
	// exactly true of the two scenes these cases are about and is the direction
	// a cull errs in anyway.

	// `Renderer.cpp`'s `wouldDescend`: whether one more level would draw
	// anything at all.
	bool WouldDescend(std::span<const engine::scene::SurfacePane> panes, const CFrame &viewer, int16_t skip) {
		for (const engine::scene::SurfacePane &pane : panes) {
			if (pane.Surface == skip) {
				continue;
			}
			if (engine::scene::ReflectCamera(pane, viewer, {}).Renders) {
				return true;
			}
		}

		return false;
	}

	// `Renderer.cpp`'s `fillMirror`, keeping its inverted level index so the
	// depth arithmetic being checked is the one the pass runs.
	void Descend(
		std::span<const engine::scene::SurfacePane> panes,
		const CFrame &viewer,
		int16_t skip,
		uint32_t level,
		uint32_t levels,
		engine::scene::SurfaceBounceProbe &probe
	) {
		for (const engine::scene::SurfacePane &pane : panes) {
			if (pane.Surface == skip) {
				continue;
			}

			const engine::scene::MirrorEye eye = engine::scene::ReflectCamera(pane, viewer, {});
			if (!eye.Renders) {
				continue;
			}

			if (level > 0) {
				Descend(panes, eye.Frame, pane.Surface, level - 1, levels, probe);
			} else {
				probe.Deeper = probe.Deeper || WouldDescend(panes, eye.Frame, pane.Surface);
			}

			probe.Resolved = std::max(probe.Resolved, levels + 1 - level);
		}
	}

	// One frame at a stated depth, reporting what it reached.
	engine::scene::SurfaceBounceProbe
	DrawFrame(std::span<const engine::scene::SurfacePane> panes, const CFrame &eye, uint32_t bounces) {
		engine::scene::SurfaceBounceProbe probe;
		const uint32_t levels = bounces - 1;

		for (const engine::scene::SurfacePane &pane : panes) {
			const engine::scene::MirrorEye top = engine::scene::ReflectCamera(pane, eye, {});
			if (!top.Renders) {
				continue;
			}

			probe.Resolved = std::max(probe.Resolved, 1u);

			if (levels > 0) {
				Descend(panes, top.Frame, pane.Surface, levels - 1, levels, probe);
			} else {
				probe.Deeper = probe.Deeper || WouldDescend(panes, top.Frame, pane.Surface);
			}
		}

		return probe;
	}

	// One pane, on its own slot.
	engine::scene::SurfacePane SlotPane(int16_t surface, const Vector3 &centre, const Vector3 &normal) {
		engine::scene::SurfacePane pane = PaneAt(centre, normal);
		pane.Surface = surface;
		return pane;
	}

	// What `render::MAX_SURFACE_DEPTH` is, spelled here because `render` is five
	// tiers above this module and a test may not reach it. The ceiling is the
	// caller's argument for exactly that reason, so what is checked below is
	// that the rule respects whatever it is handed rather than that it agrees
	// with one number.
	constexpr uint32_t CEILING = 3;
}

TEST_CASE("a measured depth grows by one and stops", "[scene][surfacecameras]") {
	using engine::scene::NextSurfaceBounces;
	using engine::scene::SurfaceBounceProbe;

	// A frame that drew no surface at all measures nothing, and the next one has
	// to be allowed to draw the first level to find out. Zero levels would be no
	// surface pass, which has a clearer spelling.
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{}, CEILING) == 1);

	// **Resolved and satisfied is a fixed point**, which is the property the
	// whole rule turns on: a depth that was enough stays exactly where it is
	// rather than being probed up and down for ever.
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{1, false}, CEILING) == 1);
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{2, false}, CEILING) == 2);
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{3, false}, CEILING) == 3);

	// One deeper when there was somewhere left to go, and never two.
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{1, true}, CEILING) == 2);
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{2, true}, CEILING) == 3);

	// **The ceiling holds, and asking past it is not an error.** Adding a level
	// multiplies the passes, so the one thing this must never do is answer with
	// a depth the renderer has no pool for.
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{3, true}, CEILING) == CEILING);
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{9, true}, CEILING) == CEILING);
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{2, true}, 1) == 1);

	// A ceiling of nothing is still a frame that draws its mirrors once. The
	// caller's number is a ceiling on the recursion, not permission for it.
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{2, true}, 0) == 1);

	// **A frame that reached less than it was given comes back down**, which is
	// what a viewer turning away from a corridor costs: three levels of pool for
	// a picture nothing is looking at.
	CHECK(NextSurfaceBounces(SurfaceBounceProbe{1, false}, CEILING) == 1);
}

TEST_CASE("one mirror settles at one level and a corridor deepens itself", "[scene][surfacecameras]") {
	// **The two scenes the automatic depth exists to tell apart**, and they are
	// `examples/MirrorDepth.luau` and `examples/MirrorCorridor.luau` as
	// arithmetic. A single number served both until v0.15 and was wrong for
	// each: two levels bought a room with one mirror in it a pass that could
	// never show anything, and cut the corridor off one level into the effect.
	const engine::scene::SurfacePane alone[] = {
		SlotPane(0, Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f}),
	};

	const engine::scene::SurfacePane corridor[] = {
		SlotPane(0, Vector3::Zero, Vector3{0.0f, 0.0f, 1.0f}),
		SlotPane(1, Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.0f, 0.0f, -1.0f}),
	};

	const CFrame eye{Vector3{1.0f, 1.0f, 4.0f}};

	// A scene with one pane in it has nothing to descend into, so the first
	// frame is also the settled one.
	uint32_t depth = engine::scene::NextSurfaceBounces(engine::scene::SurfaceBounceProbe{}, CEILING);
	CHECK(depth == 1);

	for (int frame = 0; frame < 6; frame++) {
		depth = engine::scene::NextSurfaceBounces(DrawFrame(alone, eye, depth), CEILING);
		INFO("frame " << frame);
		CHECK(depth == 1);
	}

	// **The corridor climbs one level a frame and then pins**, which is both
	// halves of the claim: it gets there without being told, and it stops
	// without being clamped by luck.
	depth = 1;
	for (uint32_t expected = 2; expected <= CEILING; expected++) {
		depth = engine::scene::NextSurfaceBounces(DrawFrame(corridor, eye, depth), CEILING);
		INFO("expected " << expected);
		CHECK(depth == expected);
	}

	for (int frame = 0; frame < 6; frame++) {
		depth = engine::scene::NextSurfaceBounces(DrawFrame(corridor, eye, depth), CEILING);
		INFO("frame " << frame);
		CHECK(depth == CEILING);
	}

	// **And it comes back down the frame the second pane stops reflecting**,
	// which is the half a rule that only ever grew would get wrong: a viewer who
	// walks out of a corridor would keep paying for it until the world was
	// reloaded.
	depth = engine::scene::NextSurfaceBounces(DrawFrame(alone, eye, depth), CEILING);
	CHECK(depth == 1);
}

TEST_CASE("a world carries its own mirror depth, and a script sets it", "[scene][surfacecameras]") {
	// **Per world rather than per process, which is the other half of v0.15's
	// open item.** How deep a chain of mirrors goes is a fact about what the
	// scene was built out of - `panes × (panes - 1) ^ (levels - 1)` passes - and
	// a session-wide knob cannot express it for two worlds at once.
	Store store("surface_bounces_authored");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Entity workspace = engine::scene::InstallServices(store);
	REQUIRE(workspace != engine::ecs::NULL_ENTITY);

	// A world that has never said anything is automatic, which is what makes
	// every scene authored before this go on drawing.
	CHECK(engine::scene::SurfaceBouncesOf(store) == engine::scene::AUTOMATIC_SURFACE_BOUNCES);

	// **The declared type is checked and not only the round-trip**, for
	// `TagFilter`'s reason: writing raw bytes through `SetProperty` succeeds
	// whatever the descriptor claims, so a wrongly declared property passes
	// every test until the first script assigns to it.
	bool declared = false;
	for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(workspace)) {
		if (property.Name == engine::core::Name("SurfaceBounces")) {
			declared = true;
			CHECK(property.Type == engine::ecs::PropertyType::Int32);
		}
	}
	CHECK(declared);

	int32_t read = -1;
	REQUIRE(store.GetProperty(workspace, engine::core::Name("SurfaceBounces"), &read, sizeof(read)));
	CHECK(read == engine::scene::AUTOMATIC_SURFACE_BOUNCES);

	const int32_t three = 3;
	REQUIRE(store.SetProperty(workspace, engine::core::Name("SurfaceBounces"), &three, sizeof(three)));
	CHECK(engine::scene::SurfaceBouncesOf(store) == three);

	REQUIRE(store.GetProperty(workspace, engine::core::Name("SurfaceBounces"), &read, sizeof(read)));
	CHECK(read == three);

	// **A number above the renderer's ceiling is accepted here and clamped
	// there**, because this module cannot name `render::MAX_SURFACE_DEPTH` and a
	// world that asks for more than a device will allocate is drawn at what it
	// can rather than refused.
	const int32_t ambitious = 64;
	REQUIRE(store.SetProperty(workspace, engine::core::Name("SurfaceBounces"), &ambitious, sizeof(int32_t)));
	CHECK(engine::scene::SurfaceBouncesOf(store) == ambitious);

	// **Below zero is the one value refused**, because it cannot be a mistake
	// about the ceiling - it is a mistake about what the word means.
	const int32_t backwards = -1;
	CHECK_FALSE(
		store.SetProperty(workspace, engine::core::Name("SurfaceBounces"), &backwards, sizeof(int32_t))
	);
	CHECK(engine::scene::SurfaceBouncesOf(store) == ambitious);
}

TEST_CASE("a world states how many surface panes it draws", "[surfacecameras]") {
	// **The count, beside the depth, and they answer different questions.**
	// `SurfaceBounces` is how deep a chain of mirrors resolves; this is how many
	// panes are drawn at all. A hall of mirrors turns both and a room with one
	// mirror in it turns neither.
	//
	// **Per world rather than per process**, which is `SurfaceBounces`' argument
	// unchanged: how many mirrors a scene has is a fact about what it was built
	// out of, and a session-wide knob cannot express it for two worlds at once.
	Store store("surface_limit_authored");
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	const Entity workspace = engine::scene::InstallServices(store);
	REQUIRE(workspace != engine::ecs::NULL_ENTITY);

	// A world that has never said anything gets the default, which is what makes
	// every scene authored before this go on drawing - and it is thirty-two rather
	// than the sixteen that used to be compiled in.
	CHECK(engine::scene::SurfaceLimitOf(store) == engine::scene::DEFAULT_SURFACE_LIMIT);
	CHECK(engine::scene::DEFAULT_SURFACE_LIMIT == 32);

	// The declared type is checked and not only the round-trip, for
	// `SurfaceBounces`' reason: raw bytes through `SetProperty` succeed whatever
	// the descriptor claims, so a wrongly declared property passes every test
	// until the first script assigns to it.
	bool declared = false;
	for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(workspace)) {
		if (property.Name == engine::core::Name("MaxSurfaces")) {
			declared = true;
			CHECK(property.Type == engine::ecs::PropertyType::Int32);
		}
	}
	CHECK(declared);

	int32_t read = -1;
	REQUIRE(store.GetProperty(workspace, engine::core::Name("MaxSurfaces"), &read, sizeof(read)));
	CHECK(read == engine::scene::DEFAULT_SURFACE_LIMIT);

	const int32_t eight = 8;
	REQUIRE(store.SetProperty(workspace, engine::core::Name("MaxSurfaces"), &eight, sizeof(eight)));
	CHECK(engine::scene::SurfaceLimitOf(store) == eight);

	REQUIRE(store.GetProperty(workspace, engine::core::Name("MaxSurfaces"), &read, sizeof(read)));
	CHECK(read == eight);

	// **Zero is allowed and means none**, which is not the same statement as
	// "use the default" - a low-detail mode has to be able to switch mirrors off
	// without the world reading as though it never had an opinion.
	const int32_t none = 0;
	REQUIRE(store.SetProperty(workspace, engine::core::Name("MaxSurfaces"), &none, sizeof(none)));
	CHECK(engine::scene::SurfaceLimitOf(store) == 0);

	// **A number above what the renderer has storage for is accepted here and
	// clamped there**, matching the depth: this module cannot name
	// `scene::MAX_SURFACES` as a *policy* limit, only as the bound on its own
	// arrays, and a world that asks for more than a device will allocate is
	// drawn at what it can rather than refused.
	const int32_t ambitious = 4096;
	REQUIRE(store.SetProperty(workspace, engine::core::Name("MaxSurfaces"), &ambitious, sizeof(int32_t)));
	CHECK(engine::scene::SurfaceLimitOf(store) == ambitious);

	// Below zero is the one value refused, because it cannot be a mistake about
	// the ceiling - it is a mistake about what the word means.
	const int32_t backwards = -1;
	CHECK_FALSE(store.SetProperty(workspace, engine::core::Name("MaxSurfaces"), &backwards, sizeof(int32_t)));
	CHECK(engine::scene::SurfaceLimitOf(store) == ambitious);
}

TEST_CASE("a surface index is wide enough for the pane budget", "[surfacecameras]") {
	// **The ceiling used to live in the smallest field in the engine**, which is
	// the thing v0.17 changed: `Visual::Surface` was an `int8_t`, so a hundred
	// and twenty-seven was a hard limit nothing in the API said anything about.
	// This case is what stops it going back.
	CHECK(sizeof(engine::scene::Visual::Surface) >= sizeof(int16_t));
	CHECK(sizeof(engine::scene::DrawInstance::Surface) >= sizeof(int16_t));
	CHECK(sizeof(engine::scene::SurfaceCamera::Surface) >= sizeof(int16_t));

	// And the storage bound is comfortably above the default, so a world that
	// says nothing is never clamped.
	CHECK(engine::scene::MAX_SURFACES > engine::scene::DEFAULT_SURFACE_LIMIT);
}

TEST_CASE(
	"surface slot enumeration survives disabled cameras and the render cap", "[surfacecameras][surface-slots]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	Store store("surface-slots");
	const auto workspace = scene::InstallServices(store);
	std::vector<Entity> cameras;
	const auto cameraClass = ecs::Classes::Find(core::Name("SurfaceCamera"));
	for (size_t index = 0; index < scene::MAX_SURFACES + 2; ++index) {
		scene::PartDesc part;
		part.Frame.Position = {float(index) * 4, 0, -4};
		const auto pane = scene::MakePart(store, part);
		REQUIRE(store.SetParent(pane, workspace));
		const auto camera = store.CreateInstance(cameraClass, "Mirror");
		REQUIRE(store.SetParent(camera, pane));
		cameras.push_back(camera);
	}
	scene::Portal disabled;
	disabled.Enabled = false;
	store.Set(cameras.front(), disabled);
	std::vector<scene::SurfaceSlot> slots;
	REQUIRE(scene::GatherSurfaceSlots(store, slots) == cameras.size());
	for (size_t index = 0; index < slots.size(); ++index) {
		CHECK(slots[index].Camera == cameras[index]);
		CHECK(slots[index].Index == (index < scene::MAX_SURFACES ? int16_t(index) : int16_t{-1}));
	}
	const auto eye = store.Create();
	store.Set(eye, Transform{CFrame(Vector3(0, 0, 2))});
	store.Set(eye, Camera{});
	store.SetResource(ActiveCamera{eye});
	(void)AimSurfaceCameras(store);
	CHECK(store.Get<SurfaceCamera>(cameras.front())->Surface == -1);
	for (size_t index = 1; index < slots.size(); ++index) {
		CHECK(store.Get<SurfaceCamera>(cameras[index])->Surface == slots[index].Index);
	}
}

TEST_CASE(
	"authored portal face roll survives mapping and fitted camera placement",
	"[scene][surfacecameras][portal-roll]"
) {
	constexpr float HALF_TURN = 3.14159265359f;
	for (const NormalId face : {
			 NormalId::Front, NormalId::Back, NormalId::Left, NormalId::Right, NormalId::Top, NormalId::Bottom
		 }) {
		for (const bool sourceRolled : {false, true}) {
			INFO("face=" << static_cast<int>(face) << " source-rolled=" << sourceRolled);
			const CFrame sourceFrame =
				CFrame(Vector3{2, 3, 4}) * CFrame::Angles(0, 0, sourceRolled ? 0.37f : 0.0f);
			const CFrame farFrame = CFrame(Vector3{100, 7, -9}) * CFrame::Angles(0, 0, -0.63f);
			Mirror mirror(face, sourceFrame);
			mirror.World.Set(mirror.Pane, Bounds{Vector3{2, 2, 2}});
			const Entity far =
				mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
			mirror.World.Set(far, Transform{farFrame});
			mirror.World.Set(far, Bounds{Vector3{3, 3, 3}});
			mirror.World.Set(mirror.Reflection, engine::scene::Portal{far});
			const Vector3 normal = NormalOf(face);
			const Vector3 localUp = std::abs(normal.Y) > 0.5f ? Vector3::ZAxis : Vector3::YAxis;
			const CFrame localFace = CFrame::LookAt(Vector3{}, normal, localUp);
			const CFrame nearFace = sourceFrame * CFrame(normal * 2) * localFace;
			const CFrame exitFace = farFrame * CFrame(normal * 3) * localFace;
			const CFrame expectedRotation = exitFace * CFrame::Angles(0, HALF_TURN, 0) * nearFace.Inverse();
			const Vector3 eye = nearFace.PointToWorldSpace(Vector3{0.7f, 0.4f, 5});
			mirror.World.Set(mirror.Eye, Transform{CFrame(eye)});
			std::vector<engine::scene::PortalSeam> seams;
			REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
			const auto map = engine::scene::SeamMapping(seams.front());
			CHECK(map.Scale == 1.5f);
			for (const Vector3 axis : {Vector3::XAxis, Vector3::YAxis, Vector3::ZAxis}) {
				CHECK(
					(map.Frame.VectorToWorldSpace(axis) - expectedRotation.VectorToWorldSpace(axis))
						.Magnitude() < TOLERANCE
				);
			}
			const Vector3 expectedEye =
				exitFace.Position + expectedRotation.VectorToWorldSpace((eye - nearFace.Position) * 1.5f);
			CHECK((map.Point(eye) - expectedEye).Magnitude() < TOLERANCE);
			REQUIRE(AimSurfaceCameras(mirror.World) == 1);
			CHECK((mirror.Placed() - expectedEye).Magnitude() < TOLERANCE);
			const Entity returnCamera = mirror.World.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "Return"
			);
			SurfaceCamera returning;
			returning.Face = face;
			mirror.World.Set(returnCamera, returning);
			mirror.World.SetParent(returnCamera, far);
			mirror.World.Set(returnCamera, engine::scene::Portal{mirror.Pane});
			REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 2);
			const auto reverseSeam = std::find_if(seams.begin(), seams.end(), [&](const auto &seam) {
				return seam.Camera == returnCamera;
			});
			REQUIRE(reverseSeam != seams.end());
			const auto reverse = engine::scene::SeamMapping(*reverseSeam);
			CHECK((reverse.Point(map.Point(eye)) - eye).Magnitude() < TOLERANCE);
			for (const Vector3 axis : {Vector3::XAxis, Vector3::YAxis, Vector3::ZAxis})
				CHECK(
					(reverse.Frame.VectorToWorldSpace(map.Frame.VectorToWorldSpace(axis)) - axis)
						.Magnitude() < TOLERANCE
				);
		}
	}
}

TEST_CASE(
	"both portal faces cut a rig using its root ownership side", "[scene][surfacecameras][portal-cut]"
) {
	for (float side : {-1.0f, 1.0f}) {
		INFO("owning side=" << side);
		Mirror mirror;
		const Entity far =
			mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
		mirror.World.Set(far, Transform{CFrame(Vector3{100, 0, 0}) * CFrame::Angles(0, 3.14159265359f, 0)});
		mirror.World.Set(far, Bounds{Vector3{16, 9, .4f}});
		mirror.World.Set(mirror.Reflection, engine::scene::Portal{far});
		const Entity root = mirror.World.Create();
		mirror.World.Set(root, Transform{CFrame(Vector3{0, 0, -.2f + side * .15f})});
		std::vector<engine::scene::DrawInstance> rows(2);
		for (size_t index = 0; index < rows.size(); index++) {
			rows[index].Frame.Position = {0, static_cast<float>(index), -.2f + (index ? -.3f : .3f)};
			rows[index].HalfExtent = {.5f, .5f, .6f};
			rows[index].Rig = root.Id;
			rows[index].Source = index + 1;
		}
		REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, rows) == 2);
		REQUIRE(rows.size() == 4);
		const Vector3 expectedNormal{0, 0, side};
		for (size_t index = 0; index < 2; index++) {
			CHECK((rows[index].SeamNormal - expectedNormal).Magnitude() < TOLERANCE);
			CHECK_THAT(rows[index].SeamOffset, Catch::Matchers::WithinAbs(-.2f * side, TOLERANCE));
			CHECK((rows[index + 2].SeamNormal + expectedNormal).Magnitude() < TOLERANCE);
			CHECK(rows[index + 2].HalfExtent == rows[index].HalfExtent * 2);
		}
	}
}
