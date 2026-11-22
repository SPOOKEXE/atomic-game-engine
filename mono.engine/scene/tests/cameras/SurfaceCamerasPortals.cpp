// Portal cameras and crossings: where a portal camera stands, bodies and
// cameras carried through a hole, and panes that stop solving contacts.

#include "SurfaceCamerasFixture.hpp"

TEST_SUITE_ID("engine.scene.surfacecamerasportals")
TEST_DEPENDS("engine.scene.surfacecameras")

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
	// viewer's side* - "`facing` is -1, so the source looks along `+Z`" - which
	// is two maps for one pane and is exactly what `SeamMapping` stopped doing at
	// v0.15, because two maps that are not each other's inverse send a body that
	// walks in the back of a hole to where one that walked in the front goes.
	// `AimSurfaceCameras` was left composing the old one, and a **cross-world**
	// pane is the only thing still drawn from it - so the immersive scene, which
	// spawns you behind its pane, got a camera forty studs from where a body
	// crossing lands and showed the half of the far room with nothing in it.
	//
	// So: the map is `destination · half-turn · source⁻¹` with `source` built
	// from **the pane's own normal**. It carries A's back hemisphere to B's
	// front, so an eye 20.2 behind A's face lands 20.2 in front of B's - at
	// `(100, 0, -20.4)` - and turns to look back through B along `+Z`.
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

TEST_CASE("a disabled portal is a solid pane and retains its link", "[scene][surfacecameras]") {
	Mirror portal;

	const Entity far =
		portal.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	portal.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	portal.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	// **Built and then written to, rather than brace-initialised.** A
	// `Collider` grew a `Geometry` field at v0.17, and an aggregate initialiser
	// naming only the extent is a `-Wmissing-field-initializers` error - while
	// filling the rest with `{}` would replace `Layer` and `Mask` with
	// default-constructed masks, which `spatial::LayerMask` documents as *empty
	// and matching nothing*. That compiles, passes, and quietly stops the pane
	// colliding.
	engine::scene::Collider pane;
	pane.Extent = Vector3{8.0f, 4.5f, 0.2f};
	portal.World.Set<engine::scene::Collider>(portal.Pane, pane);

	portal.World.Set<engine::scene::Portal>(portal.Reflection, engine::scene::Portal{far});
	REQUIRE(AimSurfaceCameras(portal.World) == 1);
	REQUIRE(engine::scene::OpenPortals(portal.World) == 1);

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(portal.World, seams) == 1);
	REQUIRE(portal.World.Get<Visual>(portal.Pane)->Surface >= 0);
	REQUIRE(portal.World.Get<engine::scene::Collider>(portal.Pane)->Trigger);

	engine::scene::Portal disabled = *portal.World.Get<engine::scene::Portal>(portal.Reflection);
	disabled.Enabled = false;
	portal.World.Set(portal.Reflection, disabled);

	// One switch owns the visual capture, the seam and the opening in the wall.
	CHECK(AimSurfaceCameras(portal.World) == 0);
	CHECK(portal.World.Get<SurfaceCamera>(portal.Reflection)->Surface == -1);
	CHECK(portal.World.Get<Visual>(portal.Pane)->Surface == -1);
	CHECK(engine::scene::GatherPortalSeams(portal.World, seams) == 0);
	CHECK(engine::scene::OpenPortals(portal.World) == 0);
	CHECK_FALSE(portal.World.Get<engine::scene::Collider>(portal.Pane)->Trigger);

	engine::scene::SeamTransform through;
	CHECK_FALSE(
		engine::scene::PortalCrossing(
			portal.World, Vector3{0.0f, 0.0f, 1.0f}, Vector3{0.0f, 0.0f, -1.0f}, through
		)
	);

	std::vector<engine::scene::SurfacePane> mirrors;
	CHECK(engine::scene::GatherSurfacePanes(portal.World, mirrors) == 0);

	disabled.Enabled = true;
	portal.World.Set(portal.Reflection, disabled);
	CHECK(AimSurfaceCameras(portal.World) == 1);
	CHECK(engine::scene::GatherPortalSeams(portal.World, seams) == 1);
	CHECK(engine::scene::OpenPortals(portal.World) == 1);
	CHECK(portal.World.Get<engine::scene::Portal>(portal.Reflection)->Destination == far);
}

TEST_CASE("a portal pair need not describe one space", "[scene][surfacecameras]") {
	// **The non-Euclidean claim, as an assertion.** Nothing constrains the two
	// frames to be consistent: turning the destination turns what comes out of
	// the hole, which is how a corridor gets to bend more than the room it is in
	// allows. `docs/retired/NON-EUCLIDEAN.md` is the investigation, and this is the one
	// line of it that the engine actually had to gain.
	//
	// A destination yawed by 90° maps the eye's 20.2 units of clearance along
	// the far pane's rotated normal instead of along `+Z`, so the camera lands
	// on a different axis entirely - with no separate feature and no maths past
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
	// X - the same portal, the same eye, a different space on the other side.
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
	// CodeParade's `Connect(a->front, b->back)` written as a matrix - so a body
	// that ends its step eight tenths in front of A arrives eight tenths behind
	// B, and the sub-camera the pass builds for the same eye is placed by the
	// same product and looks at exactly that spot.
	//
	// **Exactly where the step ended, mapped - the clearance is a floor and not
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
	// of nothing, so a second pass moves nobody - a crossing is a change of
	// side, not a place.
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
}

TEST_CASE("an active free camera flies through the portal it crosses", "[scene][surfacecameras]") {
	// A scripted camera has a transform but no velocity. The body-only query
	// therefore left it in the source corridor even though the pane already
	// showed the destination, which made a fly-through stop at the doorway.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// A controller with no subject is the presented free/scripted-camera path.
	// A subject camera is placed later by `PlaceCamera` and must not be carried
	// a second time here.
	mirror.World.SetResource(engine::scene::CameraController{});
	mirror.World.Set<engine::scene::PreviousTransform>(
		mirror.Eye, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<Transform>(
		mirror.Eye, Transform{CFrame::LookAt(Vector3{0.0f, 0.0f, -1.0f}, Vector3{0.0f, 0.0f, -2.0f})}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	const CFrame &arrived = mirror.World.Get<Transform>(mirror.Eye)->Frame;
	CHECK_THAT(arrived.Position.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(arrived.Position.Z, Catch::Matchers::WithinAbs(0.6f, TOLERANCE));
	CHECK(arrived.LookVector().Z > 0.9f);

	// Both ends of the tick landed in the destination frame, so asking again
	// cannot bounce the eye back through the partner pane.
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
}

TEST_CASE(
	"an active free camera carries pose through rotated scaled portal round trips", "[scene][surfacecameras]"
) {
	const float scale = GENERATE(.5f, 1.0f, 2.0f);
	const bool rotated = GENERATE(false, true);
	CAPTURE(scale, rotated);
	const CFrame sourceFrame =
		CFrame(Vector3{3.0f, 2.0f, -4.0f}) * (rotated ? CFrame::Angles(.2f, -.4f, .1f) : CFrame{});
	Mirror mirror(NormalId::Front, sourceFrame);

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	const CFrame farFrame =
		CFrame(Vector3{18.0f, -6.0f, 9.0f}) * (rotated ? CFrame::Angles(-.25f, .6f, .15f) : CFrame{});
	mirror.World.Set<Transform>(far, Transform{farFrame});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f * scale, 4.5f * scale, .2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	const Entity returnPortal = mirror.World.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("SurfaceCamera")), "Return"
	);
	SurfaceCamera returnSurface;
	returnSurface.Face = NormalId::Front;
	mirror.World.Set(returnPortal, returnSurface);
	REQUIRE(mirror.World.SetParent(returnPortal, far));
	mirror.World.Set(returnPortal, engine::scene::Portal{mirror.Pane});
	mirror.World.SetResource(engine::scene::CameraController{});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 2);
	const auto source =
		std::find_if(seams.begin(), seams.end(), [&](const auto &seam) { return seam.Pane == mirror.Pane; });
	const auto destination =
		std::find_if(seams.begin(), seams.end(), [&](const auto &seam) { return seam.Pane == far; });
	REQUIRE(source != seams.end());
	REQUIRE(destination != seams.end());

	const auto flight = [](const engine::scene::PortalSeam &seam) {
		const Vector3 previousPosition = seam.Centre - seam.Normal;
		const Vector3 currentPosition = seam.Centre + seam.Normal;
		return std::pair{
			CFrame::LookAt(previousPosition, currentPosition),
			CFrame::LookAt(currentPosition, currentPosition + seam.Normal)
		};
	};
	const auto fly = [&](const engine::scene::PortalSeam &seam) {
		const auto [before, now] = flight(seam);
		const engine::scene::SeamTransform through = engine::scene::SeamMapping(seam);
		mirror.World.Set(mirror.Eye, engine::scene::PreviousTransform{before});
		mirror.World.Set(mirror.Eye, Transform{now});

		REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);
		CHECK(mirror.World.Get<Transform>(mirror.Eye)->Frame.FuzzyEq(through.Place(now), TOLERANCE));
		const auto &carriedPrevious = mirror.World.Get<engine::scene::PreviousTransform>(mirror.Eye)->Frame;
		CHECK((carriedPrevious.LookVector() - through.Rotate(before.LookVector())).Magnitude() < TOLERANCE);
		CHECK(
			(mirror.World.Get<Transform>(mirror.Eye)->Frame.Position - carriedPrevious.Position).Magnitude() >
			0.0f
		);
		CHECK(engine::scene::CrossPortals(mirror.World) == 0);
	};

	// Missing the aperture must leave both ends of the tick in their source chart.
	const auto [missedBefore, missedNow] = flight(*source);
	const CFrame apertureMissBefore{missedBefore.Position + source->First * 2.0f, missedBefore.Rotation()};
	const CFrame apertureMissNow{missedNow.Position + source->First * 2.0f, missedNow.Rotation()};
	mirror.World.Set(mirror.Eye, engine::scene::PreviousTransform{apertureMissBefore});
	mirror.World.Set(mirror.Eye, Transform{apertureMissNow});
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);
	CHECK(mirror.World.Get<Transform>(mirror.Eye)->Frame.FuzzyEq(apertureMissNow, TOLERANCE));
	CHECK(mirror.World.Get<engine::scene::PreviousTransform>(mirror.Eye)
			  ->Frame.FuzzyEq(apertureMissBefore, TOLERANCE));

	// Two complete passages exercise orientation, endpoint history, scale, and
	// the no-bounce guard on every active free-camera crossing.
	for (int round = 0; round < 2; ++round) {
		fly(*source);
		fly(*destination);
	}
}

TEST_CASE(
	"a scriptable camera crosses while a player remains its controller subject", "[scene][surfacecameras]"
) {
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	const Entity subject =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Subject");
	engine::scene::CameraController controller;
	mirror.World.Set(mirror.Eye, engine::scene::CameraSubject{.Target = subject});
	controller.Mode = engine::scene::CameraMode::Scriptable;
	mirror.World.SetResource(controller);
	mirror.World.Set<engine::scene::PreviousTransform>(
		mirror.Eye, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<Transform>(
		mirror.Eye, Transform{CFrame::LookAt(Vector3{0.0f, 0.0f, -1.0f}, Vector3{0.0f, 0.0f, -2.0f})}
	);

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);
	const CFrame &arrived = mirror.World.Get<Transform>(mirror.Eye)->Frame;
	CHECK_THAT(arrived.Position.X, Catch::Matchers::WithinAbs(100.0f, TOLERANCE));
	CHECK_THAT(arrived.Position.Z, Catch::Matchers::WithinAbs(0.6f, TOLERANCE));
	CHECK(arrived.LookVector().Z > 0.9f);
}

TEST_CASE("a spinning body keeps its spin in the room it arrives in", "[scene][surfacecameras]") {
	// **The second half of `Motion`, which the crossing carried for four
	// versions without.** `Motion` is a `Linear` and an `Angular`, the solver
	// writes both and `physics::Advanced` integrates both - so a pair of panes
	// that turns the room turns the axis a body is spinning about, exactly as it
	// turns the direction the body is travelling in. Mapping one and not the
	// other is the bug that reads as physics rather than as a portal: a crate
	// tumbling end over end through a corner comes out tumbling about an axis
	// the far room has no reason to name.
	//
	// The far pane is a hole in the **floor** rather than one more door in a
	// wall, because a wall is the one arrangement that hides this. A seam's
	// destination is `LookAt(centre, centre + normal, UpFor(normal))` and the
	// half-turn is about `Y`, so any two upright panes - however they are yawed
	// or rolled - compose to a map that leaves `Y` alone, and a body spinning
	// about `Y` comes out looking correct however the pass is written. Point the
	// far pane's face at the sky and the near room's up is the far room's
	// sideways, which is the case that tells the two implementations apart.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(
		far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f}) * CFrame::Angles(1.5707963f, 0.0f, 0.0f)}
	);
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	const engine::scene::SeamTransform through = engine::scene::SeamMapping(seams[0]);

	// A top, spinning about world up at four radians a second, walking along -Z
	// through pane A exactly as the case above does.
	const Vector3 spin{0.0f, 4.0f, 0.0f};

	const Entity top =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Top");
	mirror.World.Set<Transform>(top, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		top, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
	);
	mirror.World.Set<engine::scene::Motion>(top, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, spin});

	REQUIRE(engine::scene::CrossPortals(mirror.World) == 1);

	const Vector3 spinning = mirror.World.Get<engine::scene::Motion>(top)->Angular;
	const Vector3 expected = through.Rotate(spin);

	CHECK_THAT(spinning.X, Catch::Matchers::WithinAbs(expected.X, TOLERANCE));
	CHECK_THAT(spinning.Y, Catch::Matchers::WithinAbs(expected.Y, TOLERANCE));
	CHECK_THAT(spinning.Z, Catch::Matchers::WithinAbs(expected.Z, TOLERANCE));

	// **And it is a different axis**, which is what makes the three lines above
	// an assertion rather than a restatement. A pass that leaves `Angular` alone
	// passes them the moment the pair stops turning anything, so the case has to
	// say that this pair does turn something.
	CHECK((spinning - spin).Magnitude() > 1.0f);

	// **The same rate, because a spin is not a length.** `Rotate` and not
	// `Carry`: radians per second have no distance in them, so a hole that
	// halves a body halves the radius it spins at and halves the speed of every
	// point on it, and the rate those two divide to is unchanged. `Carry` here
	// would make a shrinking pair spin a body down to nothing over a few
	// crossings, and a growing one spin it up without bound.
	CHECK_THAT(spinning.Magnitude(), Catch::Matchers::WithinAbs(4.0f, TOLERANCE));
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
	// is infinite and a hole is not - which is a wall you fall through fifty
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
	// passable - the case below is what makes it so - so a body may straddle
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
	// body as well as copy it - see `CutAndCloneSeams`. A row is a frame and a
	// box, so a test builds one directly.
	const auto row = [](const Vector3 &at, const Vector3 &half, int16_t surface) {
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
	// - so the two would fight over one texture from frame to frame.
	CHECK(drawn[2].Surface == -1);

	// The body centre is z=-.1, on the back side of the z=-.2 face.
	// Its original therefore keeps +Z and the clone keeps the mapped complement.
	CHECK_THAT(drawn[0].SeamNormal.Z, Catch::Matchers::WithinAbs(1.0f, TOLERANCE));
	CHECK_THAT(drawn[0].SeamOffset, Catch::Matchers::WithinAbs(-0.2f, TOLERANCE));
	const auto keptBy = [](const engine::scene::DrawInstance &half, const Vector3 &at) {
		return at.Dot(half.SeamNormal) >= half.SeamOffset;
	};
	const Vector3 nearHalf{0.0f, 0.0f, -0.1f};
	const Vector3 farHalf{0.0f, 0.0f, -0.3f};
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

	// **A cross-world pane gets the cut and not the copy**, and both halves of
	// that matter. `Portal::DestinationWorld` makes `Destination` a stand-in
	// telling the camera where to look, so a copy through one would appear a
	// stand-in's distance behind the pane the body is walking into rather than
	// in the world it is walking to - the host answers that half. The *cut* is
	// this pass's either way: the body poking out of the back of the glass is a
	// row right here, and a whole one is a body drawn twice over.
	engine::scene::Portal crossing{far};
	crossing.DestinationWorld = engine::core::Name("somewhere else");
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, crossing);

	drawn.clear();
	drawn.push_back(row(Vector3{0.0f, 0.0f, -0.1f}, BODY, -1));
	CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);
	CHECK(drawn.size() == 1);
	CHECK_FALSE(drawn[0].SeamNormal == Vector3{});

	// The host asks for that one by name and gets it, out of the rows this world
	// drew - which is where the interpolated frame is, and which is what makes
	// an anchored crate in a cross-world seam as much a straddler as a body that
	// walked there.
	std::vector<engine::scene::DrawInstance> foreign;
	CHECK(engine::scene::AppendPortalClones(mirror.World, 0, drawn, foreign) == 1);

	// **And the copy is cut too, to the complementary half.** Whole copies on
	// both sides is a body joined nowhere, which is what a cross-world hole drew
	// while it was argued as a window.
	//
	// **Stated as "the two halves partition the body" rather than as a sign**,
	// because the sign depends on how the pair happens to be laid out and the
	// partition does not. Take a point the near cut throws away, carry it
	// through the seam, and the far cut must keep it - and the other way round.
	// That is the whole contract, and it holds for a pair turned, moved or
	// resized.
	REQUIRE(foreign.size() == 1);
	CHECK_FALSE(foreign[0].SeamNormal == Vector3{});

	std::vector<engine::scene::PortalSeam> crossed;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, crossed) == 1);
	const engine::scene::SeamTransform crossedMap = engine::scene::SeamMapping(crossed[0]);

	const auto keptByNear = [&drawn](const Vector3 &at) {
		return at.Dot(drawn[0].SeamNormal) >= drawn[0].SeamOffset;
	};
	const auto keptByFar = [&foreign](const Vector3 &at) {
		return at.Dot(foreign[0].SeamNormal) >= foreign[0].SeamOffset;
	};

	// A stud past the plane, which is the half that has pushed through.
	const Vector3 beyond = drawn[0].SeamNormal * (drawn[0].SeamOffset - 1.0f);
	CHECK_FALSE(keptByNear(beyond));
	CHECK(keptByFar(crossedMap.Point(beyond)));

	// And a stud short of it, which is the half still in this room.
	const Vector3 behind = drawn[0].SeamNormal * (drawn[0].SeamOffset + 1.0f);
	CHECK(keptByNear(behind));
	CHECK_FALSE(keptByFar(crossedMap.Point(behind)));

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
	// segment a body covered changes sign through the plane - a body stopped
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
	engine::scene::Collider pane;
	pane.Extent = Vector3{8.0f, 4.5f, 0.2f};
	mirror.World.Set<engine::scene::Collider>(mirror.Pane, pane);
	REQUIRE_FALSE(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);

	// Not a portal yet, so it is a mirror - and a mirror is a wall. Nothing is
	// opened, which is the half that keeps this from being "make every pane
	// passable".
	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
	CHECK_FALSE(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);

	engine::scene::Portal linked{far};
	linked.RimThickness = .5f;
	mirror.World.Set(mirror.Reflection, linked);

	CHECK(engine::scene::OpenPortals(mirror.World) == 1);

	// **A trigger and not a removed collider.** Contacts are still reported, so
	// a script can tell somebody is in the hole, and `physics::GroundCharacters`
	// still gets an answer out of `Raycast` - a pane that stopped answering
	// queries is a portal you fall through the floor beside.
	const engine::scene::Collider *opened = mirror.World.Get<engine::scene::Collider>(mirror.Pane);
	REQUIRE(opened != nullptr);
	CHECK(opened->Trigger);
	CHECK(opened->Extent.X == 8.0f);
	const auto *rim = mirror.World.Get<engine::scene::PortalRim>(mirror.Pane);
	REQUIRE(rim != nullptr);
	for (const Entity edge : rim->Parts) {
		REQUIRE(mirror.World.Alive(edge));
		CHECK_FALSE(mirror.World.Get<engine::scene::Collider>(edge)->Trigger);
	}
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	CHECK(seams[0].First.Magnitude() == Catch::Approx(7.5f));
	CHECK(seams[0].Second.Magnitude() == Catch::Approx(4.0f));
	CHECK(seams[0].RimThickness == Catch::Approx(.5f));
	CHECK(mirror.World.Get<engine::scene::Collider>(rim->Parts[0])->Extent.X == Catch::Approx(.25f));

	// Idempotent: every tick after the first writes nothing at all.
	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
	engine::scene::Portal closed{far};
	closed.Enabled = false;
	mirror.World.Set(mirror.Reflection, closed);
	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
	CHECK_FALSE(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);
	CHECK_FALSE(mirror.World.Has<engine::scene::PortalRim>(mirror.Pane));
}

TEST_CASE("a pane already authored passable is left alone", "[scene][surfacecameras]") {
	// A scene that set `CanCollide = false` on its pane has already said what
	// this pass says, and writing over it would stamp the row every tick for the
	// life of the world - which is what `SyncBroadphase` reads to decide the
	// static index needs rebuilding.
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	engine::scene::Collider passable;
	passable.Extent = Vector3{8.0f, 4.5f, 0.2f};
	passable.Trigger = true;
	mirror.World.Set<engine::scene::Collider>(mirror.Pane, passable);

	CHECK(engine::scene::OpenPortals(mirror.World) == 0);
	CHECK(mirror.World.Get<engine::scene::Collider>(mirror.Pane)->Trigger);
}

TEST_CASE("a third-person camera goes through the hole its subject went through", "[scene][surfacecameras]") {
	// **The arm is metres long and the body is a point, which is the whole
	// bug.** A character crosses on the tick its own step changes side; the eye
	// behind it does not, so unless the arm is put through the same map the
	// frame after a crossing is the far room watched from the near one - the
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

	// The subject half a metre through the hole, walking away from it - where a
	// character is on the frame after it crossed.
	const Entity body =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Body");
	mirror.World.Set<Transform>(body, Transform{CFrame(Vector3{0.0f, 0.0f, -0.7f})});

	engine::scene::CameraController arm;
	mirror.World.Set(mirror.Eye, engine::scene::CameraSubject{.Target = body});

	// A yaw of zero looks along -Z, so the arm reaches back along +Z - straight
	// into the pane the subject just came out of.
	arm.Angles = Vector2{0.0f, 0.0f};
	arm.Distance = 12.0f;
	mirror.World.SetResource(arm);

	REQUIRE(engine::scene::PlaceCamera(mirror.World));

	const CFrame placed = mirror.World.Get<Transform>(mirror.Eye)->Frame;

	// **Out in front of the far pane, as far from it as the arm wanted to be
	// from the near one, looking back at it.** The head is at `y = 1.5` and
	// `z = -0.7`, so the arm reaches eleven and a half metres past A's face -
	// and what is through A is what is in front of B, which is where the eye
	// comes out: `z = -0.2 - 11.5`.
	//
	// **Looking back through the hole is the point rather than a side effect.**
	// The player is looking away from the pane, the camera is behind them, and
	// behind them is the other side - so the pane fills the frame and what the
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

TEST_CASE(
	"a humanoid camera follows continuous movement through a turned portal and back",
	"[scene][surfacecameras][humanoid-camera]"
) {
	for (const auto mode : {engine::scene::CameraMode::LockFirstPerson, engine::scene::CameraMode::Classic}) {
		for (const float side : {-1.0f, 1.0f}) {
			INFO("mode=" << static_cast<int>(mode) << " entry side=" << side);
			Mirror portal;
			Store &store = portal.World;
			engine::scene::InstallServices(store);
			const Entity far = store.CreateInstance(engine::scene::PartClass(), "FarPane");
			store.Set(
				far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f}) * CFrame::Angles(0.0f, 0.65f, 0.0f)}
			);
			store.Set(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
			store.Set(portal.Reflection, engine::scene::Portal{far});
			const Entity reverse = store.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("Portal")), "ReturnPortal"
			);
			store.SetParent(reverse, far);
			store.Set(reverse, engine::scene::Portal{portal.Pane});
			std::vector<engine::scene::PortalSeam> seams;
			REQUIRE(engine::scene::GatherPortalSeams(store, seams) == 2);
			const auto found = std::find_if(seams.begin(), seams.end(), [&](const auto &seam) {
				return seam.Pane == portal.Pane;
			});
			REQUIRE(found != seams.end());
			const engine::scene::SeamTransform through = engine::scene::SeamMapping(*found);

			const Entity player = engine::scene::AddPlayer(store, "Walker", true);
			const Entity model = engine::scene::LoadCharacter(store, player);
			REQUIRE(model != engine::ecs::NULL_ENTITY);
			const auto rig = *store.Get<engine::scene::Character>(model);
			REQUIRE_FALSE(store.Has<Transform>(rig.Humanoid));
			REQUIRE(store.SetProperty(
				portal.Eye, engine::core::Name("CameraSubject"), &rig.Humanoid, sizeof(rig.Humanoid)
			));
			engine::scene::CameraController controller;
			controller.Mode = mode;
			controller.Distance = 3.0f;
			controller.HeadHeight = 1.5f;
			controller.Angles = Vector2{0.2f, side > 0.0f ? 0.0f : std::numbers::pi_v<float>};
			store.SetResource(controller);
			CHECK_FALSE(engine::scene::FollowOwnCharacter(store));
			const Vector3 start{0.0f, 0.0f, -0.2f + side * 4.125f};
			const Vector3 direction{0.0f, 0.0f, -side};
			store.Set(rig.Root, Transform{CFrame::LookAt(start, start + direction)});
			store.Set(rig.Root, engine::scene::PreviousTransform{store.Get<Transform>(rig.Root)->Frame});
			store.GetMutable<engine::scene::Motion>(rig.Root)->Linear = direction;
			CHECK_FALSE(engine::scene::FollowPortalTransit(store));
			const float initialYaw = controller.Angles.Y;
			unsigned crossings = 0;
			for (int step = 0; step <= 80; ++step) {
				INFO("step=" << step);
				if (step == 41) {
					auto *motion = store.GetMutable<engine::scene::Motion>(rig.Root);
					motion->Linear = -motion->Linear;
					auto *root = store.GetMutable<Transform>(rig.Root);
					root->Frame = CFrame::LookAt(root->Frame.Position, root->Frame.Position + motion->Linear);
					store.ResourceMutable<engine::scene::CameraController>()->Angles.Y +=
						std::numbers::pi_v<float>;
				}
				if (step > 0) {
					const CFrame previous = store.Get<Transform>(rig.Root)->Frame;
					store.Set(rig.Root, engine::scene::PreviousTransform{previous});
					store.GetMutable<Transform>(rig.Root)->Frame.Position =
						previous.Position + store.Get<engine::scene::Motion>(rig.Root)->Linear * 0.25f;
				}
				const auto crossed = engine::scene::CrossPortals(store);
				crossings += static_cast<unsigned>(crossed);
				CHECK(engine::scene::FollowPortalTransit(store) == (crossed == 1));
				CHECK_FALSE(engine::scene::FollowPortalTransit(store));
				if (step == 20) {
					store.ResourceMutable<engine::scene::CameraController>()->Angles.Y += 0.15f;
				}
				REQUIRE(engine::scene::PlaceCamera(store));
				const auto unfoldPoint = [&](Vector3 point) {
					return point.X > 50.0f ? through.Frame.PointToObjectSpace(point) : point;
				};
				const CFrame eye = store.Get<Transform>(portal.Eye)->Frame;
				const Vector3 actualLook = eye.Position.X > 50.0f
											   ? through.Frame.VectorToObjectSpace(eye.LookVector())
											   : eye.LookVector();
				const float yaw = initialYaw + (step >= 20 ? 0.15f : 0.0f) +
								  (step >= 41 ? std::numbers::pi_v<float> : 0.0f);
				const Vector3 expectedLook{
					-std::sin(yaw) * std::cos(0.2f), std::sin(0.2f), -std::cos(yaw) * std::cos(0.2f)
				};
				const Vector3 body =
					start + direction * (0.25f * static_cast<float>(step <= 40 ? step : 80 - step));
				const Vector3 head = body + Vector3{0.0f, 1.5f, 0.0f};
				const Vector3 expectedEye =
					mode == engine::scene::CameraMode::LockFirstPerson ? head : head - expectedLook * 3.0f;
				CHECK(
					(unfoldPoint(store.Get<Transform>(rig.Root)->Frame.Position) - body).Magnitude() < 0.001f
				);
				CHECK((unfoldPoint(eye.Position) - expectedEye).Magnitude() < 0.001f);
				CHECK((actualLook - expectedLook).Magnitude() < 0.001f);
				CHECK(engine::scene::CameraSubjectRoot(store, portal.Eye) == rig.Root);
			}
			CHECK(crossings == 2);
		}
	}
}
