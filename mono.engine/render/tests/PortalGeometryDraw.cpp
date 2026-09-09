#include <engine/ecs/Store.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.portalgeometrydraw")
TEST_DEPENDS("engine.render.portalgeometry")

TEST_CASE("current body halves retain their side through scaled mappings", "[render][portal-body-split]") {
	using namespace engine;
	const float scale = GENERATE(.25f, 1.f, 4.f);
	const float side = GENERATE(-1.f, 1.f);
	CAPTURE(scale, side);
	scene::SeamTransform through;
	through.Frame = core::CFrame(core::Vector3{7, -3, 11}) * core::CFrame::Angles(.2f, .7f, -.3f);
	through.Origin = {2, 1, -4};
	through.Scale = scale;
	const core::Vector3 normal{0, 0, side};
	const float offset = -2 * side;
	std::vector<core::CFrame> joints(9);
	joints[4].Position = {1, 2, 3};
	joints[5].Position = {4, 5, 6};
	std::vector<scene::DrawInstance> body(2);
	for (size_t i = 0; i < body.size(); ++i) {
		auto &row = body[i];
		row.Source = 10 + i;
		row.Rig = 99;
		row.SourceWorld = core::Name("source");
		row.Frame.Position = {float(i), 1, -2};
		row.HalfExtent = {.4f, .8f, .3f};
		row.SkinFirst = 4;
		row.SkinCount = 2;
		row.TagMask = 17;
		row.SeamLight = {1, 0, 0};
	}
	body[1].SeamNormal = normal;
	body[1].SeamOffset = offset;
	render::PortalBodyDraws out;
	for (const float z : {-3.f, -1.f, -3.f}) {
		body[0].Frame.Position.Z = z;
		REQUIRE(render::SplitPortalBodyDraws(body, joints, through, normal, offset, out));
		REQUIRE(out.Near.size() == 2);
		REQUIRE(out.Far.size() == 2);
		REQUIRE(out.Joints.size() == 2);
		CHECK(out.Joints[0].Position == joints[4].Position);
		CHECK(out.Joints[1].Position == joints[5].Position);
		for (size_t i = 0; i < body.size(); ++i) {
			CHECK(out.Near[i].Frame.Position == body[i].Frame.Position);
			CHECK((out.Far[i].Frame.Position - through.Point(body[i].Frame.Position)).Magnitude() < .0001f);
			CHECK(out.Far[i].HalfExtent == body[i].HalfExtent * scale);
			CHECK(out.Near[i].SeamNormal == normal);
			CHECK(out.Near[i].SeamOffset == offset);
			CHECK(out.Far[i].TagMask == 0);
			CHECK(out.Near[i].TagMask == 17);
			CHECK(out.Far[i].Rig == 99);
			CHECK(out.Far[i].SourceWorld == core::Name("source"));
			CHECK(out.Far[i].SkinFirst == 0);
			CHECK(out.Near[i].SkinFirst == 0);
			CHECK((out.Far[i].SeamLight - through.Rotate(body[i].SeamLight)).Magnitude() < .0001f);
			for (const float distance : {-.2f, .2f}) {
				const core::Vector3 point{0, 0, -2 + distance};
				const float nearDistance = normal.Dot(point) - offset;
				const float farDistance =
					out.Far[i].SeamNormal.Dot(through.Point(point)) - out.Far[i].SeamOffset;
				CHECK(std::abs(farDistance + scale * nearDistance) < .0001f);
			}
		}
	}
	const auto *nearStorage = out.Near.data(), *farStorage = out.Far.data();
	const auto *jointStorage = out.Joints.data();
	REQUIRE(render::SplitPortalBodyDraws(body, joints, through, normal, offset, out));
	CHECK(out.Near.data() == nearStorage);
	CHECK(out.Far.data() == farStorage);
	CHECK(out.Joints.data() == jointStorage);
	const auto retainedPosition = out.Near.front().Frame.Position;
	SECTION("invalid palette range preserves previous output") {
		body.back().SkinCount = 30;
	}
	SECTION("an unrelated existing cut is refused") {
		body.back().SeamNormal = {1, 0, 0};
	}
	SECTION("nonfinite geometry is refused") {
		body.back().Frame.Position.X = std::numeric_limits<float>::infinity();
	}
	SECTION("nonpositive scale is refused") {
		through.Scale = 0;
	}
	SECTION("synthetic copies cannot duplicate the selected body") {
		body.back().Variant = 1;
	}
	SECTION("row count is bounded") {
		body.resize(render::MAX_PORTAL_GEOMETRY_ROWS + 1);
	}
	SECTION("referenced palette count is bounded") {
		joints.resize(render::MAX_PORTAL_GEOMETRY_JOINTS + 1);
		body.back().SkinFirst = 0;
		body.back().SkinCount = joints.size();
	}
	SECTION("nonfinite skin frames are refused") {
		joints[4].Position.X = std::numeric_limits<float>::quiet_NaN();
	}
	SECTION("scale cannot collapse the mapped geometry") {
		through.Scale = std::numeric_limits<float>::denorm_min();
	}
	CHECK_FALSE(render::SplitPortalBodyDraws(body, joints, through, normal, offset, out));
	CHECK(out.Near.front().Frame.Position == retainedPosition);
	CHECK(out.Near.data() == nearStorage);
}

TEST_CASE("body split refuses aliased storage and clears absent body", "[render][portal-body-split]") {
	using namespace engine;
	std::vector<scene::DrawInstance> body(1);
	render::PortalBodyDraws out;
	REQUIRE(render::SplitPortalBodyDraws(body, {}, {}, {0, 0, 1}, 0, out));
	CHECK_FALSE(render::SplitPortalBodyDraws(out.Near, {}, {}, {0, 0, 1}, 0, out));
	CHECK_FALSE(render::SplitPortalBodyDraws(out.Far, {}, {}, {0, 0, 1}, 0, out));
	body.front().SkinCount = 1;
	const std::array<core::CFrame, 1> joints{};
	REQUIRE(render::SplitPortalBodyDraws(body, joints, {}, {0, 0, 1}, 0, out));
	CHECK_FALSE(render::SplitPortalBodyDraws(body, out.Joints, {}, {0, 0, 1}, 0, out));
	REQUIRE(render::SplitPortalBodyDraws({}, {}, {}, {0, 0, 1}, 0, out));
	CHECK(out.Near.empty());
	CHECK(out.Far.empty());
	CHECK(out.Joints.empty());
}

TEST_CASE(
	"incoming body replaces native rows without borrowing foreign handles",
	"[render][portal-geometry][body-replacement]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("destination");
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "viewer", false, 91);
	const auto model = scene::LoadCharacter(store, player);
	const auto root = store.Get<scene::Character>(model)->Root;
	scene::DrawInstance native;
	native.Rig = root.Id;
	native.Source = root.Id;
	native.Frame.Position.X = 100;
	std::vector<scene::DrawInstance> rows(3, native);
	rows[1].SourceWorld = core::Name("foreign");
	rows[2].Variant = 8;
	render::PortalGeometry incoming;
	incoming.Rows.emplace_back();
	incoming.Rows.back().Player = "91";
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(render::EncodePortalGeometry(incoming, bytes, error));
	std::vector<core::CFrame> joints;
	render::PortalDrawSelection selection{"91", {}};
	SECTION("matching native body is replaced") {
		REQUIRE(
			render::AppendPortalDraws(bytes, core::Name("source"), rows, joints, error, &selection, &store)
		);
		REQUIRE(rows.size() == 3);
		CHECK(rows[0].SourceWorld == core::Name("foreign"));
		CHECK(rows[1].Variant == 8);
		CHECK(rows[2].Source == 0);
		CHECK(selection.Hidden == std::vector<uint32_t>{2});
		CHECK(selection.Appended == 1);
		CHECK(selection.Replaced == 1);
	}
	SECTION("malformed payload preserves native rows") {
		bytes.pop_back();
		CHECK_FALSE(
			render::AppendPortalDraws(bytes, core::Name("source"), rows, joints, error, &selection, &store)
		);
		CHECK(rows.size() == 3);
		CHECK(rows[0].Source == root.Id);
		CHECK(selection.Appended == 0);
		CHECK(selection.Replaced == 0);
	}
	SECTION("a held native pose uses its retained account after retirement") {
		store.DestroyInstance(root);
		scene::CameraCharacterHold held;
		held.SourceRoot = root;
		held.Player = player;
		held.Active = true;
		store.SetResource(held);
		REQUIRE(
			render::AppendPortalDraws(bytes, core::Name("source"), rows, joints, error, &selection, &store)
		);
		CHECK(rows.size() == 3);
		CHECK(selection.Replaced == 1);
	}
	SECTION("unknown account cannot remove the native body") {
		incoming.Rows[0].Player = "92";
		REQUIRE(render::EncodePortalGeometry(incoming, bytes, error));
		REQUIRE(
			render::AppendPortalDraws(bytes, core::Name("source"), rows, joints, error, &selection, &store)
		);
		CHECK(rows.size() == 4);
		CHECK(selection.Replaced == 0);
		CHECK(selection.Hidden.empty());
	}
}

TEST_CASE(
	"copied body geometry follows a child mouth without losing identity",
	"[render][portal-geometry][portal-forward]"
) {
	using namespace engine;
	scene::PortalSeam seam;
	seam.Crosses = true;
	seam.DestinationWorld = core::Name("child");
	seam.Normal = {0, 0, 1};
	seam.First = {2, 0, 0};
	seam.Second = {0, 3, 0};
	seam.Up = {0, 1, 0};
	seam.Scale = 2;
	seam.Destination = core::CFrame(core::Vector3{10, 20, 30}) * core::CFrame::Angles(0, .6f, 0);
	render::PortalGeometry source;
	source.Rows.emplace_back();
	auto &body = source.Rows.back();
	body.Player = "91";
	body.Name = "retired/body";
	body.Pose[2] = -4;
	body.SeamPlane = {0, 0, 1, 0};
	body.JointCount = 1;
	source.Joints.push_back({1, 2, 3, 0, 0, 0, 1});
	render::PortalGeometry original;
	original.Rows.resize(2);
	original.Rows[0].Player = "91";
	original.Rows[0].Pose[0] = 100;
	original.Rows[1].Player = "92";
	std::vector<std::byte> incoming, child;
	std::string error;
	REQUIRE(render::EncodePortalGeometry(source, incoming, error));
	REQUIRE(render::EncodePortalGeometry(original, child, error));
	const auto before = child;
	SECTION("fully crossed cut keeps mapped pose and compact palette") {
		REQUIRE(render::ForwardPortalDraws(incoming, seam, child, error));
		render::PortalGeometry result;
		REQUIRE(render::DecodePortalGeometry(child, result, error));
		REQUIRE(result.Rows.size() == 2);
		CHECK(result.Rows[0].Player == "92");
		const auto &copy = result.Rows[1];
		CHECK(copy.Player == "91");
		CHECK(copy.Name == body.Name);
		CHECK(copy.HalfExtent == std::array<float, 3>{1, 1, 1});
		const auto expected = scene::SeamMapping(seam).Point({0, 0, -4});
		CHECK(core::Vector3(copy.Pose[0], copy.Pose[1], copy.Pose[2]).FuzzyEq(expected, .00001f));
		CHECK(expected.Dot({copy.SeamPlane[0], copy.SeamPlane[1], copy.SeamPlane[2]}) >= copy.SeamPlane[3]);
		CHECK(result.Joints == source.Joints);
		CHECK(copy.FirstJoint == 0);
	}
	SECTION("a different clipping mouth cannot borrow the continuation") {
		body.SeamPlane = {0, 1, 0, 0};
		REQUIRE(render::EncodePortalGeometry(source, incoming, error));
		REQUIRE(render::ForwardPortalDraws(incoming, seam, child, error));
		CHECK(child == before);
	}
	SECTION("unclipped geometry must still straddle the mouth") {
		body.SeamPlane = {};
		REQUIRE(render::EncodePortalGeometry(source, incoming, error));
		REQUIRE(render::ForwardPortalDraws(incoming, seam, child, error));
		CHECK(child == before);
	}
	SECTION("merged row budget refuses without changing the child") {
		original.Rows.assign(render::MAX_PORTAL_GEOMETRY_ROWS, {});
		REQUIRE(render::EncodePortalGeometry(original, child, error));
		const auto full = child;
		CHECK_FALSE(render::ForwardPortalDraws(incoming, seam, child, error));
		CHECK(child == full);
	}
	SECTION("overlapping source palettes cannot amplify beyond the joint budget") {
		body.JointCount = render::MAX_PORTAL_GEOMETRY_JOINTS;
		source.Joints.resize(body.JointCount, {0, 0, 0, 0, 0, 0, 1});
		source.Rows.push_back(body);
		REQUIRE(render::EncodePortalGeometry(source, incoming, error));
		CHECK_FALSE(render::ForwardPortalDraws(incoming, seam, child, error));
		CHECK(child == before);
	}
	SECTION("malformed parent preserves the child") {
		incoming.pop_back();
		CHECK_FALSE(render::ForwardPortalDraws(incoming, seam, child, error));
		CHECK(child == before);
	}
}

TEST_CASE(
	"portal draw conversion carries clipping and compacts skin poses without source ids",
	"[render][portal-geometry]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store source("near");
	const auto part = scene::MakePart(source, {});
	scene::DrawInstance draw;
	draw.Source = part.Id;
	draw.Rig = part.Id;
	draw.Surface = 4;
	draw.TagMask = 7;
	draw.Frame = core::CFrame::Angles(.2f, .4f, .6f);
	draw.Texture = core::Name("character/colour");
	draw.NormalMap = core::Name("character/normal");
	draw.Tint = {.3f, .4f, .5f};
	draw.SeamNormal = {0, 1, 0};
	draw.SeamOffset = 2;
	draw.SkinFirst = 1;
	draw.SkinCount = 1;
	std::vector<core::CFrame> sourceJoints{core::CFrame{}, core::CFrame(core::Vector3{1, 2, 3})};
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(render::EncodePortalDraws(source, std::span(&draw, 1), sourceJoints, bytes, error));
	render::PortalGeometry geometry;
	REQUIRE(render::DecodePortalGeometry(bytes, geometry, error));
	CHECK(geometry.Rows[0].Name == source.GetFullName(part));
	CHECK(geometry.Joints.size() == 1);
	std::vector<scene::DrawInstance> rows(1);
	std::vector<core::CFrame> joints(2);
	REQUIRE(render::AppendPortalDraws(bytes, core::Name("near"), rows, joints, error));
	REQUIRE(rows.size() == 2);
	const auto &received = rows.back();
	CHECK(received.Source == 0);
	CHECK(received.Rig == 0);
	CHECK(received.Surface == -1);
	CHECK(received.TagMask == 0);
	CHECK(received.SourceWorld == core::Name("near"));
	CHECK(received.Texture == draw.Texture);
	CHECK(received.NormalMap == draw.NormalMap);
	CHECK(received.Tint == draw.Tint);
	CHECK(received.SeamNormal == draw.SeamNormal);
	CHECK(received.SeamOffset == draw.SeamOffset);
	CHECK(received.SkinFirst == 2);
	CHECK(received.SkinCount == 1);
	CHECK(joints.back().Position == sourceJoints.back().Position);
	CHECK(received.Frame.LookVector().FuzzyEq(draw.Frame.LookVector(), 1e-6f));
	bytes.pop_back();
	CHECK_FALSE(render::AppendPortalDraws(bytes, core::Name("near"), rows, joints, error));
	CHECK(rows.size() == 2);
	CHECK(joints.size() == 3);
}

TEST_CASE(
	"portal draw selection survives source retirement without importing handles",
	"[render][portal-geometry][eye-body]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store source("near");
	scene::InstallServices(source);
	const auto player = scene::AddPlayer(source, "viewer", false, 91);
	const auto model = scene::LoadCharacter(source, player);
	const auto rig = *source.Get<scene::Character>(model);
	scene::DrawInstance draw;
	draw.Source = rig.Root.Id;
	draw.Rig = rig.Root.Id;
	const auto retainedPlayer = scene::AddPlayer(source, "held", false, 91);
	scene::CameraCharacterHold hold;
	hold.SourceRoot = rig.Root;
	hold.Player = retainedPlayer;
	hold.Active = false;
	source.SetResource(hold);
	for (const bool retired : {false, true}) {
		if (retired) {
			source.DestroyInstance(model);
			source.ResourceMutable<scene::CameraCharacterHold>()->Active = true;
		}
		std::vector<std::byte> bytes;
		std::string error;
		REQUIRE(render::EncodePortalDraws(source, std::span(&draw, 1), {}, bytes, error));
		render::PortalGeometry geometry;
		REQUIRE(render::DecodePortalGeometry(bytes, geometry, error));
		REQUIRE(geometry.Rows.size() == 1);
		CHECK(geometry.Rows[0].Player == "91");
		CHECK(geometry.Rows[0].Name.empty() == retired);
		for (const auto selected : {"", "91", "92"}) {
			render::PortalDrawSelection selection{selected, {99}};
			std::vector<scene::DrawInstance> rows(3);
			std::vector<core::CFrame> joints;
			REQUIRE(render::AppendPortalDraws(bytes, core::Name("near"), rows, joints, error, &selection));
			CHECK(rows.back().Source == 0);
			CHECK(rows.back().Rig == 0);
			if (selection.Player == "91")
				CHECK(selection.Hidden == std::vector<uint32_t>{3});
			else
				CHECK(selection.Hidden.empty());
			bytes.pop_back();
			const auto hidden = selection.Hidden;
			CHECK_FALSE(
				render::AppendPortalDraws(bytes, core::Name("near"), rows, joints, error, &selection)
			);
			CHECK(selection.Hidden == hidden);
			REQUIRE(render::EncodePortalGeometry(geometry, bytes, error));
		}
	}
	// A foreign row can carry a coincident local handle. It cannot claim that owner.
	draw.SourceWorld = core::Name("another-world");
	std::vector<std::byte> foreign;
	std::string foreignError;
	REQUIRE(render::EncodePortalDraws(source, std::span(&draw, 1), {}, foreign, foreignError));
	render::PortalGeometry otherWorld;
	REQUIRE(render::DecodePortalGeometry(foreign, otherWorld, foreignError));
	CHECK(otherWorld.Rows[0].Player.empty());
	draw.SourceWorld = {};
	source.ResourceMutable<scene::CameraCharacterHold>()->Active = false;
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(render::EncodePortalDraws(source, std::span(&draw, 1), {}, bytes, error));
	render::PortalGeometry anonymous;
	REQUIRE(render::DecodePortalGeometry(bytes, anonymous, error));
	CHECK(anonymous.Rows[0].Player.empty());
}

TEST_CASE(
	"retained body removal separates camera hiding from caster ownership",
	"[render][portal-geometry][retained-body]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	ecs::Store store("destination");
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "current-body", false, 91);
	const auto model = scene::LoadCharacter(store, player);
	const auto root = store.Get<scene::Character>(model)->Root;
	std::vector<scene::DrawInstance> rows(2);
	rows[0].Rig = rows[1].Rig = root.Id;
	rows[0].SourceWorld = core::Name("foreign");
	rows[1].Variant = 8;
	SECTION("native rig and its seam clone use owning account") {}
	SECTION("held rig keeps its account after the live source root retires") {
		scene::CameraCharacterHold held;
		held.Active = true;
		held.Player = player;
		held.SourceRoot = ecs::Entity{9000};
		store.SetResource(held);
		rows[1].Rig = held.SourceRoot.Id;
	}
	render::PortalGeometry incoming;
	incoming.Rows.resize(3);
	incoming.Rows[0].Player = "91";
	incoming.Rows[1].Player = "17";
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(render::EncodePortalGeometry(incoming, bytes, error));
	render::PortalDrawSelection selected{"17", {}};
	selected.RetainedBodyPlayer = "91";
	std::vector<core::CFrame> joints;
	REQUIRE(render::AppendPortalDraws(bytes, core::Name("source"), rows, joints, error, &selected, &store));
	CHECK(selected.Hidden == std::vector<uint32_t>{3});
	CHECK(selected.RetainedBodyRows == std::vector<uint32_t>{2});
	REQUIRE(
		render::RemoveRetainedPortalBody(store, "91", rows, selected.RetainedBodyRows, selected.Hidden, error)
	);
	REQUIRE(rows.size() == 3);
	CHECK(rows.front().SourceWorld == core::Name("foreign"));
	CHECK(selected.Hidden == std::vector<uint32_t>{1});
	CHECK(rows[1].CastShadow);
	CHECK(rows[2].CastShadow);
	const auto previousCount = rows.size();
	CHECK_FALSE(render::RemoveRetainedPortalBody(store, "091", rows, {}, selected.Hidden, error));
	CHECK(rows.size() == previousCount);
	CHECK(selected.Hidden == std::vector<uint32_t>{1});
}
