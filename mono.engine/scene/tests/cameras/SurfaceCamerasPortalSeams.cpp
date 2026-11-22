// Portal seams: near planes and clips at a hole, scaled holes, landing
// clearance, which hole is crossed, and rows cut and cloned through a seam.

#include "SurfaceCamerasFixture.hpp"

TEST_SUITE_ID("engine.scene.surfacecamerasseams")
TEST_DEPENDS("engine.scene.surfacecameras")

TEST_CASE("a portal in the plane of the viewer keeps drawing", "[scene][surfacecameras]") {
	// **The band is a mirror's fix and it was being applied to holes.** Which
	// way a *reflected* camera looks depends on which side of the plane the
	// viewer is, both answers are right, and nothing joins them - so a mirror
	// blanks across `EDGE_ON_MARGIN` rather than flashing. A linked portal has
	// no such discontinuity: the frame the viewer's side flips is the frame the
	// viewer is carried through the pane, and the two cancel.
	//
	// What the band cost there is the whole of the feature. It is 0.3 metres
	// either side of the plane, which is exactly where somebody walking through
	// a hole spends the crossing - so the picture went dark on the one frame it
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
	// matrix finite at the plane - `MINIMUM_DEPTH` and `FIT_MINIMUM_SPAN` -
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
	// in some particular place but that it never *jumps* - and the interesting
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
	// one tick of jitter is all it takes - which reads as the portal throwing
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
	// depth of its own to be put down at - a character brought to a halt in a
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
	// change sign through it on a step smaller than the clearance - which is
	// exactly the jitter that used to send a crosser back through the hole it
	// had just come out of, once per tick.
	mirror.World.Set<engine::scene::PreviousTransform>(
		walker, engine::scene::PreviousTransform{CFrame(landed)}
	);
	mirror.World.Set<Transform>(walker, Transform{CFrame(landed - Vector3{0.0f, 0.0f, 0.005f})});
	CHECK(engine::scene::CrossPortals(mirror.World) == 0);

	// **And it is hysteresis rather than a wall.** There is only one pane in
	// this world and the body is now beside the far end of it, so walking back
	// through is the round-trip case rather than this one - see "going back
	// through a scaled hole", which does it with both halves of a pair.
}

TEST_CASE("a hole between panes of different sizes changes what goes through it", "[scene][surfacecameras]") {
	// **The difference between a room bigger on the inside and a picture of
	// one.** A rigid map puts the camera at the far end and leaves everything
	// the size it was, so a doorway twice as big shows the same room through a
	// bigger frame and a body walks out of it unchanged. Carrying the scale is
	// what makes the simulation agree with the claim.
	//
	// Pane A is 16 by 9 across its face - the fixture's half-extents are 8 and
	// 4.5 - and B is twice that on both axes, so the ratio of the areas is four
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
	// size and twice as fast - a speed is a length, and a crosser that kept its
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

TEST_CASE("portal crossing preserves full seam paths after scene edits", "[scene][surfacecameras]") {
	Mirror mirror;
	auto &world = mirror.World;
	const Entity far = world.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	world.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	world.Set<Bounds>(far, Bounds{Vector3{16.0f, 9.0f, 0.2f}});
	world.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	const Vector3 from{0.0f, 0.0f, 1.0f};
	const Vector3 to{0.0f, 0.0f, -1.0f};
	const auto checkCrossing = [&] {
		std::vector<engine::scene::PortalSeam> seams;
		REQUIRE(engine::scene::GatherPortalSeams(world, seams) == 1);
		engine::scene::PortalHop expected;
		size_t index = 0;
		REQUIRE(engine::scene::NearestPortalCrossing(seams, from, to, false, expected, index));
		engine::scene::PortalHop actual;
		REQUIRE(engine::scene::PortalCrossing(world, from, to, actual));
		CHECK(actual.Pane == expected.Pane);
		CHECK(actual.Far == expected.Far);
		CHECK(actual.Share == expected.Share);
		CHECK(actual.Through.Point(from) == expected.Through.Point(from));
		CHECK(actual.Through.Rotate(to - from) == expected.Through.Rotate(to - from));
		CHECK(actual.Through.Scale == expected.Through.Scale);
	};
	checkCrossing();

	const Entity parent =
		world.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "RenamedParent");
	REQUIRE(world.SetParent(mirror.Pane, parent));
	REQUIRE(world.SetInstanceName(mirror.Pane, "RenamedPane"));
	REQUIRE(world.SetInstanceName(far, "RenamedFar"));
	checkCrossing();
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(world, seams) == 1);
	CHECK(seams[0].PanePath == "RenamedParent.RenamedPane");
	CHECK(seams[0].FarPath == "RenamedFar");
}

TEST_CASE("a matched pair of panes changes nothing about what crosses", "[scene][surfacecameras]") {
	// **The regression guard for every world already built.** The scale is
	// derived from two measurements rather than authored, so the ordinary pair
	// has to come out at exactly one - not nearly one - or every portal in the
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
	// construction - but "by construction" is exactly the sort of claim that
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
	// the *first one gathered* - which is archetype order, and archetype order
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
	// `engine::render::CollectParticleBatches` is the caller this exists for - a torch
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
	// four and a half, so nine studs across is outside it - and unlike
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
	// in every hole - no clone in the far room, no half a body in the picture,
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

	// Contact with the rim is still inside the aperture. The character fixture
	// below puts its feet exactly here, and rejecting equality drops only those
	// limbs while the rest of the rig crosses normally.
	const engine::scene::SeamCut touching = engine::scene::CutOfSeam(
		narrow, through, CFrame(Vector3{0.0f, -0.5f, -0.2f}), Vector3{1.0f, 2.0f, 1.0f}
	);
	CHECK(touching.Fits);

	// Tolerance at the rim must not turn an actual overhang into a valid cut.
	const engine::scene::SeamCut overhanging = engine::scene::CutOfSeam(
		narrow, through, CFrame(Vector3{0.0f, -0.51f, -0.2f}), Vector3{1.0f, 2.0f, 1.0f}
	);
	CHECK_FALSE(overhanging.Fits);
}

TEST_CASE("a one-way portal refuses the way it does not face", "[scene][surfacecameras]") {
	// **A door you can walk into and not out of.** `Portal::Bidirectional` off
	// leaves the pane drawing, cutting and lighting exactly as it was and
	// refuses one thing: a crossing that starts behind the face. In front means
	// the side the face's normal points at, which is the side the pane shows its
	// image on and the side `SeamCarries` calls "not yet through".
	Mirror mirror;

	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

	// The fixture's pane has its `Front` face outward along -Z, so a body
	// walking from `z = +1` to `z = -1` is entering the *back* of it. That is
	// the case a one-way mouth exists to refuse.
	const auto walk = [&] {
		const Entity walker =
			mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
		mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, -1.0f})});
		mirror.World.Set<engine::scene::PreviousTransform>(
			walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, 1.0f})}
		);
		mirror.World.Set<engine::scene::Motion>(
			walker, engine::scene::Motion{Vector3{0.0f, 0.0f, -16.0f}, Vector3::Zero}
		);
		return walker;
	};

	SECTION("both ways by default") {
		mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});
		(void)walk();
		CHECK(engine::scene::CrossPortals(mirror.World) == 1);
	}

	SECTION("one way refuses the back") {
		engine::scene::Portal portal{far};
		portal.Bidirectional = false;
		mirror.World.Set<engine::scene::Portal>(mirror.Reflection, portal);

		const Entity walker = walk();
		CHECK(engine::scene::CrossPortals(mirror.World) == 0);

		// Left where it was, rather than stopped at the plane. A one-way mouth
		// is not a wall - it is a hole this body is not going through.
		const auto *placed = mirror.World.Get<Transform>(walker);
		REQUIRE(placed != nullptr);
		CHECK_THAT(placed->Frame.Position.Z, Catch::Matchers::WithinAbs(-1.0f, 1e-4f));
	}

	SECTION("one way still admits the front") {
		engine::scene::Portal portal{far};
		portal.Bidirectional = false;
		mirror.World.Set<engine::scene::Portal>(mirror.Reflection, portal);

		// The same walk, the other way round.
		const Entity walker =
			mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Walker");
		mirror.World.Set<Transform>(walker, Transform{CFrame(Vector3{0.0f, 0.0f, 1.0f})});
		mirror.World.Set<engine::scene::PreviousTransform>(
			walker, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, -1.0f})}
		);
		mirror.World.Set<engine::scene::Motion>(
			walker, engine::scene::Motion{Vector3{0.0f, 0.0f, 16.0f}, Vector3::Zero}
		);

		CHECK(engine::scene::CrossPortals(mirror.World) == 1);
		const auto *placed = mirror.World.Get<Transform>(walker);
		REQUIRE(placed != nullptr);
		CHECK(std::abs(placed->Frame.Position.X - 100.0f) < 15.0f);
	}
}

TEST_CASE("a rig is cut in one piece or not at all", "[scene][surfacecameras]") {
	// **The reported "character split in half in a portal".** Whether a row may
	// be cut is "does it fit the aperture", and a character is a dozen drawn
	// rows: standing in an opening its torso is comfortably inside the rectangle
	// and its feet sit on the bottom edge, where the box overhangs. Asked per
	// row that is yes for the torso and no for the feet, and a row that answers
	// no is drawn *whole* rather than cut - so the body is clipped at the plane
	// from the waist up and pushed through the wall from the ankles down.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	// The pane is eight by four and a half. A torso in the middle of it fits; a
	// foot on the bottom edge reaches four-nine and does not.
	const auto torso = [] {
		engine::scene::DrawInstance row;
		row.Frame = CFrame(Vector3{0.0f, 1.0f, -0.2f});
		row.HalfExtent = Vector3{0.6f, 1.2f, 0.4f};
		return row;
	};
	const auto foot = [] {
		engine::scene::DrawInstance row;
		row.Frame = CFrame(Vector3{0.0f, -4.5f, -0.2f});
		row.HalfExtent = Vector3{0.3f, 0.4f, 0.5f};
		return row;
	};

	SECTION("two loose bodies keep their own answers") {
		std::vector<engine::scene::DrawInstance> drawn{torso(), foot()};

		// One copy, one cut: exactly the split, and it is correct for two rows
		// that are two different objects.
		CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
		CHECK(drawn[0].SeamNormal != Vector3{});
		CHECK(drawn[1].SeamNormal == Vector3{});
	}

	SECTION("two limbs of one rig are cut together") {
		std::vector<engine::scene::DrawInstance> drawn{torso(), foot()};
		drawn[0].Rig = 7;
		drawn[1].Rig = 7;

		// Both, because one part of the body is through the opening and the
		// body is one thing. The foot's few centimetres past the rim are worth
		// less than the rig being in one piece.
		CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 2);
		CHECK(drawn[0].SeamNormal != Vector3{});
		CHECK(drawn[1].SeamNormal != Vector3{});
	}

	SECTION("a rig with nothing through the hole is refused whole") {
		std::vector<engine::scene::DrawInstance> drawn{foot(), foot()};
		drawn[0].Rig = 9;
		drawn[1].Rig = 9;
		drawn[1].Frame = CFrame(Vector3{0.0f, -4.6f, -0.2f});

		CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 0);
		CHECK(drawn[0].SeamNormal == Vector3{});
		CHECK(drawn[1].SeamNormal == Vector3{});
	}

	SECTION("two rigs do not answer for each other") {
		std::vector<engine::scene::DrawInstance> drawn{torso(), foot()};
		drawn[0].Rig = 1;
		drawn[1].Rig = 2;

		// The torso's rig fits and the foot's does not, which is the loose case
		// again - a grouping that leaked between rigs would cut both.
		CHECK(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
		CHECK(drawn[0].SeamNormal != Vector3{});
		CHECK(drawn[1].SeamNormal == Vector3{});
	}
}

TEST_CASE("the near plane and the clip follow the eye into a hole", "[scene][surfacecameras]") {
	// **What makes the last hand's width of an approach seamless.** A near plane
	// is a floor on how close geometry may be drawn, so walking up to a pane with
	// an authored one slices it open and the wall beside the doorway disappears -
	// on the one frame the whole feature is judged. CodeParade's demo drives the
	// near plane down from the nearest hole's distance and pulls the oblique clip
	// back by the same measure; these are those two numbers.
	constexpr float FAR_AWAY = std::numeric_limits<float>::infinity();

	// **The rectangle and not its plane.** An eye level with a doorway but well
	// to the side of it is standing in a wall, and a plane says zero for both -
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
	// the one hole there is, both draw with exactly what the scene asked for -
	// so the depth range is only ever spent when something needs it.
	CHECK_THAT(engine::scene::PortalNearPlane(0.1f, FAR_AWAY), Catch::Matchers::WithinAbs(0.1f, TOLERANCE));
	CHECK_THAT(engine::scene::PortalNearPlane(0.1f, 5.0f), Catch::Matchers::WithinAbs(0.1f, TOLERANCE));

	// Half the distance once half the distance is the smaller number, so the
	// pane is never inside the near plane.
	CHECK_THAT(engine::scene::PortalNearPlane(0.1f, 0.08f), Catch::Matchers::WithinAbs(0.04f, TOLERANCE));
	for (const float authored : {0.0001f, 0.001f, engine::scene::PORTAL_NEAR_MIN}) {
		for (const float distance : {0.0f, 0.00001f, 0.002f, 1.0f, FAR_AWAY}) {
			CAPTURE(authored, distance);
			const float near = engine::scene::PortalNearPlane(authored, distance);
			CHECK(near > 0.0f);
			CHECK(near <= authored);
			if (distance > 0.0f && std::isfinite(distance)) {
				engine::scene::Camera camera;
				camera.NearPlane = near;
				const auto matrices = engine::scene::ResolveCamera(CFrame{}, camera, 1.0f);
				const auto clip = matrices.ViewProjection * glm::vec4(0, 0, -distance, 1);
				CHECK(clip.z >= 0);
				CHECK(clip.z <= clip.w);
			}
		}
	}

	// And a floor, because an eye pressed against the glass would otherwise ask
	// for a near plane of zero and get a projection of infinities.
	CHECK_THAT(
		engine::scene::PortalNearPlane(0.1f, 0.0f),
		Catch::Matchers::WithinAbs(engine::scene::PORTAL_NEAR_MIN, TOLERANCE)
	);

	// The clip bias is the same halving, capped by the width inside which a
	// pane's construction is degenerate anyway, and nothing at all when there is
	// no hole to bias against.
	CHECK_THAT(engine::scene::PortalClipBias(FAR_AWAY), Catch::Matchers::WithinAbs(0.0f, TOLERANCE));
	CHECK_THAT(engine::scene::PortalClipBias(0.08f), Catch::Matchers::WithinAbs(0.04f, TOLERANCE));
	CHECK_THAT(engine::scene::PortalClipBias(50.0f), Catch::Matchers::WithinAbs(0.3f, TOLERANCE));
}

TEST_CASE("a crossing reports the complete cumulative body map", "[scene][surfacecameras]") {
	// A tilted exit exercises pitch and roll as well as yaw. The replicated
	// map must reconstruct both the body's look and up axes.
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

	const CFrame expected = went->Frame * CFrame(Vector3::Zero, started.Rotation());
	const CFrame actual = mirror.World.Get<Transform>(walker)->Frame;
	CHECK((actual.LookVector() - expected.LookVector()).Magnitude() < 1.0e-4f);
	CHECK((actual.UpVector() - expected.UpVector()).Magnitude() < 1.0e-4f);
	CHECK(went->Scale == 1.0f);
	CHECK(went->Serial == 1);
}

TEST_CASE("a viewpoint is never left standing in a pane", "[scene][surfacecameras]") {
	// **The band a camera cannot render from.** A surface camera built from an
	// eye in its own pane's plane has no half-space for the oblique clip to keep
	// and no bounded fit, and the pane comes out as a vertical smear of
	// stretched texels - which reads as a corrupt texture rather than as an eye
	// standing somewhere it should not. A body already has this rule: it is put
	// down clear of whatever plane it crossed.
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	SECTION("same-world mouth") {}
	SECTION("cross-world mouth") {
		engine::scene::Portal crossing{far};
		crossing.DestinationWorld = engine::core::Name("somewhere else");
		mirror.World.Set<engine::scene::Portal>(mirror.Reflection, crossing);
	}

	// The pane's face is at `z = -0.2` and its normal is `-Z`, so the side the
	// normal points to - the side `SeamOffset` calls positive - is the smaller
	// `z`. A point a little that way is a point that belongs that way.
	//
	// **A hair rather than a hand's width, and that is the whole seamlessness
	// argument.** A same-world hole is drawn by the recursive pass, whose only
	// construction is an oblique clip - it needs the eye off the plane and
	// nothing else - and `PortalNearPlane` shrinks the near plane to meet
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
	// whatever stands beside them - two rooms laid out adjacent with the hole
	// between them agreeing with the geometry - and every copy then arrives on
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

	// The map takes this body onto itself, so there is nothing to add - and
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
	// walk and not the moving-body one - and a size rule stated against the
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
	// pass reads. Built here by hand because `engine::render::CollectInstances` is a
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
	// pass, which puts the far half in the far room - but somebody looking *at*
	// the pane sees the surface texture, and if the copy never reached the
	// surface pass the picture in the hole would show a room with nobody in it
	// while the body is visibly standing in the doorway.
	//
	// What decides that is where `OrderScene` puts it. The surface pass draws
	// the `Reflected` run - the opaque part of the scene range that is not
	// itself a mirror - so a copy is shown by the hole exactly when it lands
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
	// pass submits - not in the mirror run after it, and not in the blended
	// tail.
	const auto copy = static_cast<uint32_t>(drawn.size() - 1);
	bool inReflected = false;
	for (uint32_t at = 0; at < plan.Reflected; at++) {
		inReflected = inReflected || order[at] == copy;
	}

	CHECK(inReflected);
	CHECK(plan.Reflected < plan.Opaque);
}

TEST_CASE("a static prop stays whole through a local portal", "[scene][surfacecameras]") {
	Mirror mirror;
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	const Entity prop =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "StaticProp");
	engine::scene::DrawInstance row;
	row.Source = prop.Id;
	row.Frame = CFrame(Vector3{0.0f, 0.0f, -0.1f});
	row.HalfExtent = Vector3{0.5f, 1.0f, 0.5f};

	std::vector<engine::scene::DrawInstance> drawn{row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	REQUIRE(drawn.size() == 2);
	CHECK(drawn[0].SeamNormal == Vector3{});
	CHECK(drawn[1].SeamNormal == Vector3{});

	// A simulated body transfers at a fixed tick, so only its transition keeps
	// complementary halves. The static branch must not widen that path.
	mirror.World.Set<engine::scene::Simulated>(prop, {});
	drawn = {row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[0].SeamNormal != Vector3{});
	CHECK(drawn[1].SeamNormal != Vector3{});

	// Anchored scripts can move a Transform without making a physics body. Their
	// presentation still has a tick-to-tick path, so it keeps the same cut while
	// it crosses a pane.
	mirror.World.Remove<engine::scene::Simulated>(prop);
	mirror.World.Set<Transform>(prop, Transform{CFrame(Vector3{0.0f, 0.0f, -0.1f})});
	mirror.World.Set<engine::scene::PreviousTransform>(
		prop, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, -0.2f})}
	);
	drawn = {row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[0].SeamNormal != Vector3{});
	CHECK(drawn[1].SeamNormal != Vector3{});

	// A sideways scripted move at the pane's front thickness keeps the authored
	// front half. Centre-based ownership would put that half behind the pane.
	row.Frame = CFrame(Vector3{0.0f, 0.0f, 0.0f});
	row.HalfExtent = Vector3{0.5f, 1.0f, 5.0f};
	mirror.World.Set<Transform>(prop, Transform{row.Frame});
	mirror.World.Set<engine::scene::PreviousTransform>(
		prop, engine::scene::PreviousTransform{CFrame(Vector3{-0.1f, 0.0f, 0.0f})}
	);
	drawn = {row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[0].SeamNormal == Vector3{0.0f, 0.0f, -1.0f});
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	CHECK(
		(drawn[1].SeamNormal - engine::scene::SeamMapping(seams[0]).Rotate(-seams[0].Normal)).Magnitude() <
		TOLERANCE
	);
	row.Frame = CFrame(Vector3{0.0f, 0.0f, -0.3f});
	mirror.World.Set<Transform>(prop, Transform{row.Frame});
	mirror.World.Set<engine::scene::PreviousTransform>(
		prop, engine::scene::PreviousTransform{CFrame(Vector3{-0.1f, 0.0f, -0.3f})}
	);
	drawn = {row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[0].SeamNormal == Vector3{0.0f, 0.0f, -1.0f});

	row.Frame = CFrame(Vector3{0.0f, 0.0f, 0.0f});
	mirror.World.Set<Transform>(prop, Transform{row.Frame});
	mirror.World.Set<engine::scene::PreviousTransform>(
		prop, engine::scene::PreviousTransform{CFrame(Vector3{0.0f, 0.0f, -0.5f})}
	);
	drawn = {row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[0].SeamNormal == Vector3{0.0f, 0.0f, 1.0f});

	mirror.World.Set<engine::scene::Simulated>(prop, {});
	drawn = {row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	CHECK(drawn[0].SeamNormal == Vector3{0.0f, 0.0f, 1.0f});
}

TEST_CASE("a rotated side pane uses its face depth for a tangent part", "[scene][surfacecameras]") {
	const CFrame paneFrame = CFrame::Angles(0.0f, 0.45f, 0.0f);
	Mirror mirror(NormalId::Right, paneFrame);
	mirror.World.Set<Bounds>(mirror.Pane, Bounds{Vector3{0.2f, 4.5f, 8.0f}});
	const Entity far =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Far");
	mirror.World.Set<Transform>(far, Transform{CFrame(Vector3{100.0f, 0.0f, 0.0f})});
	mirror.World.Set<Bounds>(far, Bounds{Vector3{0.2f, 4.5f, 8.0f}});
	mirror.World.Set<engine::scene::Portal>(mirror.Reflection, engine::scene::Portal{far});

	const Entity prop =
		mirror.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "TangentProp");
	const Vector3 tangent = paneFrame.VectorToWorldSpace(Vector3{0.0f, 1.0f, 0.0f});
	engine::scene::DrawInstance row;
	row.Source = prop.Id;
	row.Frame = CFrame(Vector3::Zero);
	row.HalfExtent = Vector3{5.0f, 1.0f, 5.0f};
	mirror.World.Set<Transform>(prop, Transform{row.Frame});
	mirror.World.Set<engine::scene::PreviousTransform>(
		prop, engine::scene::PreviousTransform{CFrame(-tangent * 0.1f)}
	);
	std::vector<engine::scene::DrawInstance> drawn{row};
	REQUIRE(engine::scene::CutAndCloneSeams(mirror.World, drawn) == 1);
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(mirror.World, seams) == 1);
	CHECK((drawn[0].SeamNormal - seams[0].Normal).Magnitude() < TOLERANCE);
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
	// `AimSurfaceCameras` composed its own instead - out of a *source frame built
	// from which side the viewer is on*. That is the pre-v0.15 shape, and it is
	// side-dependent by construction: the same pane yields one map for a viewer in
	// front and a different one for a viewer behind.
	//
	// **A same-world hole hid it**, because the recursive portal pass took over
	// the picture and this camera stopped reaching a screen. A **cross-world**
	// pane is the one that still draws from here - and the immersive scene spawns
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
	// image - and what that looks like is a pane drawing its own material, flat,
	// which reads as "the other world does not render" rather than as a matrix.
	//
	// The three matrices come from one place and must therefore agree: the fit
	// and the oblique clip are built against the *mapped* rectangle, and the
	// mapping carries the source pane onto exactly that rectangle. This case is
	// the assertion that they do, stated where a suite can reach it - the
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
