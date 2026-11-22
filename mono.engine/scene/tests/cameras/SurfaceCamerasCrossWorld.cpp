// Cross-world portals: holes leading to another world, the bodies cut and
// drawn on both sides of them, and rig crossing histories.

#include "SurfaceCamerasFixture.hpp"

TEST_SUITE_ID("engine.scene.surfacecamerascrossworld")
TEST_DEPENDS("engine.scene.surfacecameras")

namespace {
	// A pane that leads to another world, with a stand-in saying where.
	//
	// **The stand-in sits on the pane rather than beside it**, which is how a
	// cross-world pair is authored and is the arrangement the guard used to
	// refuse: the two rooms are laid out the same way, so the map is a half-turn
	// about the pane's own axis and a body standing dead centre in the doorway
	// comes out at its own coordinates. `ImmersivePortals.luau` does exactly
	// this and so does every scene built from one file for two worlds.
	struct Window {
		Mirror Room;
		Entity StandIn;

		explicit Window(const Vector3 &standAt = Vector3{0.0f, 0.0f, 0.0f}) {
			StandIn =
				Room.World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "StandIn");
			Room.World.Set<Transform>(StandIn, Transform{CFrame(standAt)});
			Room.World.Set<Bounds>(StandIn, Bounds{Vector3{8.0f, 4.5f, 0.2f}});

			engine::scene::Portal crossing{StandIn};
			crossing.DestinationWorld = engine::core::Name("the other place");
			Room.World.Set<engine::scene::Portal>(Room.Reflection, crossing);
		}

		// One row as a collector would publish it.
		static engine::scene::DrawInstance Row(const Vector3 &at, const Vector3 &half) {
			engine::scene::DrawInstance instance;
			instance.Frame = CFrame(at);
			instance.HalfExtent = half;
			instance.Surface = -1;
			return instance;
		}
	};
}

TEST_CASE(
	"a predicted rig keeps its far side after clearing a cross-world mouth",
	"[scene][surfacecameras][body-view]"
) {
	Window window(Vector3{0, 0, -20});
	auto &store = window.Room.World;
	const float direction = GENERATE(-1.f, 1.f);
	const float scale = GENERATE(1.f, 2.f);
	const bool overlappingWorlds = GENERATE(false, true);
	CAPTURE(direction, scale, overlappingWorlds);
	store.Set(window.StandIn, Bounds{Vector3{8.f, 4.5f, .2f} * scale});
	if (overlappingWorlds) {
		store.Set(
			window.StandIn,
			Transform{CFrame(Vector3{0, 0, -.2f * (1 + scale)}, CFrame::Angles(0, 3.14159265f, 0).Rotation())}
		);
	}
	const auto &descriptor =
		engine::ecs::Components::Describe(engine::ecs::Components::Of<engine::scene::PortalBodyView>());
	CHECK_FALSE(descriptor.RawSerialisation);
	const Entity root = store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Root");
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(store, seams) == 1);
	const auto seam = seams.front();
	const auto near = seam.Centre + seam.Normal * (4 * direction);
	const auto far = seam.Centre - seam.Normal * (4 * direction);
	store.Set(root, Transform{CFrame(near)});
	std::vector drawn{Window::Row(far, Vector3{.5f, 1, .5f})};
	drawn.front().Rig = root.Id;
	drawn.front().Source = root.Id;
	std::vector<engine::scene::DrawInstance> picture;
	CHECK(engine::scene::AppendPortalClones(store, seam, drawn, picture) == 0);
	engine::scene::UpdatePortalBodyView(store, root, near, drawn);
	engine::scene::UpdatePortalBodyView(store, root, far, drawn);
	REQUIRE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
	REQUIRE(engine::scene::AppendPortalClones(store, seam, drawn, picture) == 1);
	const auto mapped = engine::scene::SeamMapping(seam).Point(far);
	CHECK((picture.front().Frame.Position - mapped).Magnitude() < TOLERANCE);
	CHECK(picture.front().HalfExtent == drawn.front().HalfExtent * scale);
	CHECK(mapped.Dot(picture.front().SeamNormal) >= picture.front().SeamOffset);
	CHECK(engine::scene::CutAndCloneSeams(store, drawn) == 0);
	CHECK(far.Dot(drawn.front().SeamNormal) < drawn.front().SeamOffset);

	SECTION("a fully crossed rig keeps its whole far row outside the aperture") {
		const auto farNormal = picture.front().SeamNormal;
		const float farOffset = picture.front().SeamOffset;
		const auto nearNormal = drawn.front().SeamNormal;
		const float nearOffset = drawn.front().SeamOffset;
		const auto lateral = far + seam.First * 2;
		drawn = {Window::Row(lateral, Vector3{.5f, 1, .5f})};
		drawn.front().Frame = CFrame(lateral, CFrame::Angles(.3f, .4f, .2f).Rotation());
		drawn.front().Rig = root.Id;
		drawn.front().Source = root.Id;
		engine::scene::UpdatePortalBodyView(store, root, lateral, drawn);
		REQUIRE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
		picture.clear();
		REQUIRE(engine::scene::AppendPortalClones(store, seam, drawn, picture) == 1);
		const auto expected = engine::scene::SeamMapping(seam).Place(drawn.front().Frame);
		CHECK((picture.front().Frame.Position - expected.Position).Magnitude() < TOLERANCE);
		CHECK(picture.front().Frame.Rotation() == expected.Rotation());
		CHECK(picture.front().HalfExtent == drawn.front().HalfExtent * scale);
		CHECK(picture.front().SeamNormal == farNormal);
		CHECK(picture.front().SeamOffset == Catch::Approx(farOffset));
		CHECK(picture.front().SeamMask == 0);
		CHECK(engine::scene::CutAndCloneSeams(store, drawn) == 0);
		CHECK(drawn.front().SeamNormal == nearNormal);
		CHECK(drawn.front().SeamOffset == Catch::Approx(nearOffset));
		CHECK(drawn.front().SeamMask == 0);
	}
	SECTION("a straddling row retains both finite aperture masks") {
		drawn = {Window::Row(seam.Centre - seam.Normal * (.25f * direction), Vector3{.5f, 1, .5f})};
		drawn.front().Rig = root.Id;
		drawn.front().Source = root.Id;
		picture.clear();
		REQUIRE(engine::scene::AppendPortalClones(store, seam, drawn, picture) == 1);
		CHECK(picture.front().SeamMask == 2);
		CHECK(engine::scene::CutAndCloneSeams(store, drawn) == 0);
		CHECK(drawn.front().SeamMask == 1);
	}
	SECTION("returning restores source ownership") {
		engine::scene::UpdatePortalBodyView(store, root, near, drawn);
		CHECK_FALSE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
	}
	SECTION("snapshots reset the derived local history") {
		engine::core::ByteWriter saved;
		REQUIRE(store.Save(saved));
		Store restored("restored");
		engine::core::ByteReader reader(saved.Bytes());
		REQUIRE(restored.Load(reader));
		const auto *view = restored.Resource<engine::scene::PortalBodyView>();
		REQUIRE(view);
		CHECK(view->Root == engine::ecs::NULL_ENTITY);
		CHECK_FALSE(view->Crossing.has_value());
	}
	SECTION("moving a mouth invalidates the saved crossing") {
		store.Set(window.Room.Pane, Transform{CFrame(Vector3{3, 0, 0})});
		engine::scene::UpdatePortalBodyView(store, root, far, drawn);
		CHECK_FALSE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
	}
	SECTION("a held root keeps the retired body's crossing") {
		const auto retainedRoot = store.CreateInstance(engine::scene::PartClass(), "retained-root");
		engine::scene::CameraCharacterHold held;
		held.SourceRoot = root;
		held.Root = retainedRoot;
		held.Active = true;
		store.SetResource(held);
		store.DestroyInstance(root);
		engine::scene::UpdatePortalBodyView(store, root, far, drawn);
		REQUIRE(store.Resource<engine::scene::PortalBodyView>());
		CHECK(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
		picture.clear();
		CHECK(engine::scene::AppendPortalClones(store, seam, drawn, picture) == 1);
		store.DestroyInstance(retainedRoot);
		engine::scene::UpdatePortalBodyView(store, root, far, drawn);
		CHECK(store.Resource<engine::scene::PortalBodyView>() == nullptr);
	}
	SECTION("foreign handles cannot borrow the local crossing") {
		drawn.front().SourceWorld = engine::core::Name("another world");
		picture.clear();
		CHECK(engine::scene::AppendPortalClones(store, seam, drawn, picture) == 0);
	}
	SECTION("retiring the source releases its history") {
		store.DestroyInstance(root);
		engine::scene::UpdatePortalBodyView(store, root, far, drawn);
		CHECK(store.Resource<engine::scene::PortalBodyView>() == nullptr);
	}
	SECTION("an oversized rig does not acquire a crossing") {
		engine::scene::UpdatePortalBodyView(store, root, near, drawn);
		drawn.front().HalfExtent = Vector3{100, 100, 100};
		engine::scene::UpdatePortalBodyView(store, root, far, drawn);
		CHECK_FALSE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
	}
	SECTION("a sweep outside the aperture does not acquire a crossing") {
		const auto offset = seam.First * 3;
		engine::scene::UpdatePortalBodyView(store, root, near + offset, drawn);
		engine::scene::UpdatePortalBodyView(store, root, far + offset, drawn);
		CHECK_FALSE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
	}
	SECTION("a one-way mouth refuses entry from behind") {
		auto portal = *store.Get<engine::scene::Portal>(window.Room.Reflection);
		portal.Bidirectional = false;
		store.Set(window.Room.Reflection, portal);
		engine::scene::UpdatePortalBodyView(store, root, seam.Centre - seam.Normal * 4, drawn);
		engine::scene::UpdatePortalBodyView(store, root, seam.Centre + seam.Normal * 4, drawn);
		CHECK_FALSE(store.Resource<engine::scene::PortalBodyView>()->Crossing.has_value());
	}
	SECTION("invalid input clears the local history") {
		engine::scene::UpdatePortalBodyView(
			store, root, Vector3{std::numeric_limits<float>::quiet_NaN(), 0, 0}, drawn
		);
		CHECK(store.Resource<engine::scene::PortalBodyView>() == nullptr);
	}
}

TEST_CASE(
	"a presented rig owns its cut side while its root snapshot lags", "[scene][surfacecameras][body-view]"
) {
	Window window(Vector3{0, 0, -20});
	auto &store = window.Room.World;
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(store, seams) == 1);
	const auto &seam = seams.front();
	const float side = GENERATE(-1.f, 1.f);
	CAPTURE(side);
	const Vector3 presented = seam.Centre + seam.Normal * (0.5f * side);
	const Vector3 snapshot = seam.Centre - seam.Normal * (4.f * side);
	const Entity root = store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Root");
	store.Set(root, Transform{CFrame(snapshot)});
	std::vector drawn{Window::Row(presented, Vector3{0.5f, 1.f, 2.f})};
	drawn.front().Rig = root.Id;
	drawn.front().Source = root.Id;
	engine::scene::UpdatePortalBodyView(store, root, presented, drawn);
	const auto *view = store.Resource<engine::scene::PortalBodyView>();
	REQUIRE(view);
	CHECK((view->Previous - presented).Magnitude() < TOLERANCE);
	CHECK_FALSE(view->Crossing.has_value());
	REQUIRE(engine::scene::CutAndCloneSeams(store, drawn) == 0);
	REQUIRE(drawn.front().SeamMask == 1);
	CHECK(presented.Dot(drawn.front().SeamNormal) >= drawn.front().SeamOffset);
	CHECK(snapshot.Dot(drawn.front().SeamNormal) < drawn.front().SeamOffset);

	engine::scene::UpdatePortalBodyView(store, engine::ecs::NULL_ENTITY, presented, drawn);
	CHECK(store.Resource<engine::scene::PortalBodyView>() == nullptr);
	drawn = {Window::Row(presented, Vector3{0.5f, 1.f, 2.f})};
	drawn.front().Rig = root.Id;
	drawn.front().Source = root.Id;
	REQUIRE(engine::scene::CutAndCloneSeams(store, drawn) == 0);
	REQUIRE(drawn.front().SeamMask == 1);
	CHECK(snapshot.Dot(drawn.front().SeamNormal) >= drawn.front().SeamOffset);
}

TEST_CASE("a cross-world hole cuts both halves of what stands in it", "[scene][surfacecameras]") {
	// **The report this case was written for: "it renders full cubes through the
	// portal".** A cross-world pane was argued as a window rather than a hole -
	// "a body does not straddle a window, it is teleported through one" - so the
	// copy was built uncut and the original was skipped by the cut pass
	// entirely. A body in the doorway was therefore drawn *whole* in the room it
	// was leaving and *whole* again in the room it was entering, overlapping the
	// picture in the glass and joined nowhere.
	//
	// The two halves have to partition the body, exactly as they do for a
	// same-world pair, and the picture is the only place the far one can go.
	Window window;

	std::vector<engine::scene::DrawInstance> drawn;
	drawn.push_back(Window::Row(Vector3{0.0f, 0.0f, -0.1f}, Vector3{0.5f, 1.0f, 0.5f}));

	// **The cut is this pass's even though the copy is not.** The body poking
	// out of the back of the glass is a row right here; the copy belongs beside
	// another world's rows, which only a host holding the universe can assemble.
	CHECK(engine::scene::CutAndCloneSeams(window.Room.World, drawn) == 0);
	REQUIRE(drawn.size() == 1);
	REQUIRE_FALSE(drawn[0].SeamNormal == Vector3{});

	std::vector<engine::scene::DrawInstance> picture;
	REQUIRE(engine::scene::AppendPortalClones(window.Room.World, 0, drawn, picture) == 1);

	// **The partition, stated as a partition.** A sign would depend on how the
	// pair happens to be laid out; this does not. Carry a point the near cut
	// throws away through the seam and the far cut must keep it, and the other
	// way round - which is what "one body, two halves, no overlap and no gap"
	// means.
	std::vector<engine::scene::PortalSeam> seams;
	REQUIRE(engine::scene::GatherPortalSeams(window.Room.World, seams) == 1);
	const engine::scene::SeamTransform through = engine::scene::SeamMapping(seams[0]);

	const auto near = [&drawn](const Vector3 &at) {
		return at.Dot(drawn[0].SeamNormal) >= drawn[0].SeamOffset;
	};
	const auto far = [&picture](const Vector3 &at) {
		return at.Dot(picture[0].SeamNormal) >= picture[0].SeamOffset;
	};

	const Vector3 pushedThrough = drawn[0].SeamNormal * (drawn[0].SeamOffset - 1.0f);
	const Vector3 stillHere = drawn[0].SeamNormal * (drawn[0].SeamOffset + 1.0f);

	CHECK_FALSE(near(pushedThrough));
	CHECK(far(through.Point(pushedThrough)));
	CHECK(near(stillHere));
	CHECK_FALSE(far(through.Point(stillHere)));

	// **Lit by the sun this side of the hole sees.** The copy's normals are the
	// original's turned by the seam's rotation, so shading them with the far
	// room's own light gives one body lit by two suns a quarter apart - a bright
	// face meeting an olive one down the middle of a crate.
	CHECK_FALSE(picture[0].SeamLight == Vector3{});

	// Never a surface of its own, or a copied pane would claim the slot its
	// original writes and the two would fight over one texture.
	CHECK(picture[0].Surface == -1);
}

TEST_CASE("a high portal slot keeps its cross-world body clone", "[scene][surfacecameras][surface-wide]") {
	Window window;
	auto camera = *window.Room.World.Get<SurfaceCamera>(window.Room.Reflection);
	camera.Surface = 319;
	window.Room.World.Set(window.Room.Reflection, camera);
	std::vector<engine::scene::DrawInstance> drawn{Window::Row(Vector3{0, 0, -.1f}, Vector3{.5f, 1, .5f})};
	std::vector<engine::scene::DrawInstance> picture;
	REQUIRE(engine::scene::AppendPortalClones(window.Room.World, int16_t{319}, drawn, picture) == 1);
	REQUIRE(picture.size() == 1);
	CHECK(picture[0].SeamNormal != Vector3{});
	picture.clear();
	CHECK(engine::scene::AppendPortalClones(window.Room.World, int16_t{63}, drawn, picture) == 0);
	CHECK(picture.empty());
}

TEST_CASE("what crosses a hole is what is drawn, not what can move", "[scene][surfacecameras]") {
	// **One rule for both halves of the file, which is what this case pins.**
	// The cross-world copy was an entity walk over bodies carrying `Motion` or
	// `CharacterLimb` - "what goes through a portal is what can move" - a rule
	// the same-world side had already retired, because an anchored crate resting
	// in a seam is as much a thing standing in the hole as anything that walked
	// there. So an anchored part had no far half through a cross-world pane, and
	// neither did anything drawn from a row rather than from an entity: a mesh
	// carries its geometry in a name, and no walk over `Motion` will find one
	// that nobody made movable.
	//
	// Reading rows also takes the interpolated frame the world is actually drawn
	// with, rather than re-deriving it and landing a half-body a tick away from
	// its other half.
	Window window;

	std::vector<engine::scene::DrawInstance> drawn;

	// An anchored crate, and a mesh beside it - neither of which any walk over
	// movable bodies would have found.
	drawn.push_back(Window::Row(Vector3{-2.0f, 0.0f, -0.1f}, Vector3{0.5f, 1.0f, 0.5f}));

	engine::scene::DrawInstance mesh = Window::Row(Vector3{2.0f, 0.0f, -0.1f}, Vector3{0.5f, 1.0f, 0.5f});
	mesh.Mesh = engine::core::Name("a.mesh");
	mesh.Texture = engine::core::Name("a.texture");
	mesh.Tint = engine::core::Color3{0.25f, 0.5f, 0.75f};
	mesh.TagMask = 0b1010u;
	mesh.Transparency = 0.25f;
	mesh.Source = 41;
	drawn.push_back(mesh);

	std::vector<engine::scene::DrawInstance> picture;
	REQUIRE(engine::scene::AppendPortalClones(window.Room.World, 0, drawn, picture) == 2);

	// **A copy is the row it was copied from**, appearance and all. Building the
	// ghost field by field is how a copy quietly loses its normal map the day
	// somebody adds one, which is why it is a copy and then four edits.
	const engine::scene::DrawInstance &copied = picture[1];
	CHECK(copied.Mesh == engine::core::Name("a.mesh"));
	CHECK(copied.Texture == engine::core::Name("a.texture"));
	CHECK(copied.TagMask == 0b1010u);
	CHECK(copied.Source == 41);
	CHECK(copied.Variant != 0);
	CHECK_THAT(copied.Transparency, Catch::Matchers::WithinAbs(0.25f, TOLERANCE));
	CHECK_THAT(copied.Tint.B, Catch::Matchers::WithinAbs(0.75f, TOLERANCE));
}

TEST_CASE("a hole does not copy its own furniture, or anything with no far half", "[scene][surfacecameras]") {
	// **Four things that all sit in a seam and none of which have a far half**,
	// and every one of them was a row the draw-list walk would otherwise have
	// copied. The entity walk it replaced could refuse the first two by name;
	// reading rows means refusing them by what they are, which is better -
	// a world composited from another world's list has no names in it at all,
	// and a world with no camera in it has no surface slots either.
	Window window;

	std::vector<engine::scene::PortalSeam> seam;
	REQUIRE(engine::scene::GatherPortalSeams(window.Room.World, seam) == 1);
	const Vector3 centre = seam[0].Centre;
	const Vector3 normal = seam[0].Normal;

	const auto crosses = [&window](const engine::scene::DrawInstance &instance) {
		std::vector<engine::scene::DrawInstance> one{instance};
		std::vector<engine::scene::DrawInstance> picture;
		return engine::scene::AppendPortalClones(window.Room.World, 0, one, picture) == 1;
	};

	// **The pane is the hole rather than a thing in it.** Its row is exactly the
	// rectangle the seam was measured from, so an inclusive fit says every pane
	// fits through itself - and the copy lands on the far pane, z-fighting a
	// wall with a picture on it.
	engine::scene::DrawInstance pane = Window::Row(centre, Vector3{8.0f, 4.5f, 0.2f});
	CHECK_FALSE(crosses(pane));

	// **A face marker is a decal, not a straddler.** Three hundredths of a stud
	// thick, lying on the pane's own face: it passes a bounding-sphere straddle
	// test and fits the rectangle easily, and cutting it in two yields nothing
	// and a copy of nothing. It is also appended *after* the same-world cut
	// pass, so a list walk is the first thing ever to see one.
	CHECK_FALSE(crosses(Window::Row(centre + normal * 0.03f, Vector3{0.17f, 0.03f, 0.03f})));

	// **Nothing invisible without attached graph work**, and a cross-world pair
	// puts exactly such a row in the seam: the destination stand-in is pane-sized,
	// centred on the plane and authored invisible.
	engine::scene::DrawInstance ghostly = Window::Row(centre, Vector3{0.5f, 1.0f, 0.5f});
	ghostly.Transparency = 1.0f;
	CHECK_FALSE(crosses(ghostly));
	ghostly.Source = 77;
	ghostly.Variant = 9;
	ghostly.Effects.Count = 1;
	ghostly.Effects.Attachments[0].Node = engine::core::Name("portal.effect");
	ghostly.Effects.Attachments[0].SelectionMask = 0x3u;
	ghostly.Effects.Attachments[0].Order = 4;
	ghostly.Effects.Attachments[0].Revision = 7;
	ghostly.Effects.Attachments[0].Enabled = false;
	CHECK(crosses(ghostly));
	std::vector<engine::scene::DrawInstance> picture;
	REQUIRE(engine::scene::AppendPortalClones(window.Room.World, 0, std::span(&ghostly, 1), picture) == 1);
	CHECK(picture.front().Effects.Count == 1);
	CHECK(picture.front().Effects.Attachments[0].Node == engine::core::Name("portal.effect"));
	CHECK(picture.front().Effects.Attachments[0].SelectionMask == 0x3u);
	CHECK(picture.front().Effects.Attachments[0].Order == 4);
	CHECK(picture.front().Effects.Attachments[0].Revision == 7);
	CHECK_FALSE(picture.front().Effects.Attachments[0].Enabled);
	CHECK(picture.front().Variant == engine::scene::PortalVariant(ghostly.Variant, seam[0].Pane.Id));

	// **And nothing bigger than the hole**, which cannot be cut by a single
	// plane without slicing the part of it that hangs past the rim, where the
	// hole is not. A room straddles a doorway and keeps its own floor.
	CHECK_FALSE(crosses(Window::Row(centre, Vector3{50.0f, 0.5f, 50.0f})));

	// The control, so the four refusals above are refusals rather than a pass
	// that never worked: the same row, a little smaller than the hole, crosses.
	CHECK(crosses(Window::Row(centre, Vector3{0.5f, 1.0f, 0.5f})));
}

TEST_CASE("a body on the map's own axis crosses worlds and not one room", "[scene][surfacecameras]") {
	// **Two worlds laid out the same way is what makes a hole read as an opening
	// rather than as a painting**, and it makes the seam's map a half-turn about
	// the pane's own axis. A body standing dead centre in the doorway is *on*
	// that axis: it comes out at its own coordinates, turned right around.
	//
	// Inside one list that copy is on top of its original - two coplanar
	// surfaces at one depth, which is a stripe of flickering colour - and it has
	// to go. Across two worlds the same coordinates are a different space, and
	// it is the most important crossing there is. Asking one question for both
	// is what cut a body off at the plane with nothing beyond it.
	Window window;

	// **The pane's own face centre, which is the fixed point of the map.** The
	// stand-in sits on the pane, so the far frame is the near one and the
	// product is a half-turn about the pane's axis - a point on the plane and on
	// that axis comes back to itself, moved by nothing and turned by half a
	// revolution. That is a body standing dead centre in a doorway.
	std::vector<engine::scene::PortalSeam> here_seams;
	REQUIRE(engine::scene::GatherPortalSeams(window.Room.World, here_seams) == 1);
	const Vector3 axis = here_seams[0].Centre;

	std::vector<engine::scene::DrawInstance> drawn;
	drawn.push_back(Window::Row(axis, Vector3{0.5f, 1.0f, 0.5f}));

	std::vector<engine::scene::DrawInstance> picture;
	CHECK(engine::scene::AppendPortalClones(window.Room.World, 0, drawn, picture) == 1);
	REQUIRE(picture.size() == 1);
	CHECK_THAT((picture[0].Frame.Position - axis).Magnitude(), Catch::Matchers::WithinAbs(0.0f, TOLERANCE));

	// And the same arrangement inside one world refuses it, which is the rule
	// the cross-world path had borrowed.
	Mirror same;
	same.World.Set<engine::scene::Portal>(same.Reflection, engine::scene::Portal{same.Pane});

	std::vector<engine::scene::DrawInstance> here;
	here.push_back(Window::Row(Vector3{0.0f, 0.0f, -0.2f}, Vector3{0.5f, 1.0f, 0.5f}));
	CHECK(engine::scene::CutAndCloneSeams(same.World, here) == 0);
	CHECK(here.size() == 1);
}
