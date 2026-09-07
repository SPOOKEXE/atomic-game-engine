#include <engine/ecs/Store.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <cmath>
#include <limits>
#include <numbers>

TEST_SUITE_ID("engine.render.portalimagedemand")
TEST_DEPENDS("engine.scene.surfacecameras")
TEST_DEPENDS("engine.render.portalexchange")

TEST_CASE(
	"source-world eyes export their retained body with account identity",
	"[render][portal-eye-demand][body-origin]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	world::RegisterMailboxTypes();
	ecs::Store store("replica");
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "viewer", false, 91);
	const auto model = scene::LoadCharacter(store, player);
	const auto root = store.Get<scene::Character>(model)->Root;
	world::Replica replica;
	replica.Active = true;
	replica.Of = core::Name("authority");
	store.SetResource(replica);
	scene::CameraCharacterHold held;
	held.SourceRoot = root;
	held.Player = player;
	held.Active = true;
	store.SetResource(held);
	scene::CameraBodyPose pose;
	pose.SourceRoot = root;
	store.SetResource(pose);
	scene::DrawInstance row;
	row.Rig = root.Id;
	row.Source = root.Id;
	row.Frame.Position = {7, 8, 9};
	std::vector<scene::DrawInstance> rows(3, row);
	rows[1].SourceWorld = core::Name("foreign");
	rows[2].Variant = 1;
	store.DestroyInstance(root);
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(render::CollectPortalEyeGeometry(store, core::Name("authority"), rows, {}, bytes, error));
	render::PortalGeometry decoded;
	REQUIRE(render::DecodePortalGeometry(bytes, decoded, error));
	REQUIRE(decoded.Rows.size() == 1);
	CHECK(decoded.Rows[0].Player == "91");
	CHECK(decoded.Rows[0].Pose[0] == 7);
	CHECK(decoded.Rows[0].Pose[1] == 8);
	CHECK(decoded.Rows[0].Pose[2] == 9);
	store.RemoveResource<scene::CameraCharacterHold>();
	REQUIRE(render::CollectPortalEyeGeometry(store, core::Name("authority"), rows, {}, bytes, error));
	CHECK(bytes.empty());
	const auto replacementModel = scene::LoadCharacter(store, player);
	const auto replacementRoot = store.Get<scene::Character>(replacementModel)->Root;
	store.SetResource(scene::LocalPlayer{player});
	store.RemoveResource<scene::CameraBodyPose>();
	rows.assign(1, row);
	rows[0].Source = replacementRoot.Id;
	rows[0].Rig = replacementRoot.Id;
	REQUIRE(render::CollectPortalEyeGeometry(store, core::Name("authority"), rows, {}, bytes, error));
	REQUIRE(render::DecodePortalGeometry(bytes, decoded, error));
	REQUIRE(decoded.Rows.size() == 1);
	CHECK(decoded.Rows[0].Player == "91");
}

namespace {
	using namespace engine;
	using namespace engine::render;
	scene::PortalSeam Seam() {
		scene::PortalSeam seam;
		seam.Crosses = true;
		seam.DestinationWorld = core::Name("other-room");
		seam.Normal = {0, 0, 1};
		seam.First = {3, 0, 0};
		seam.Second = {0, 4, 0};
		seam.Up = {0, 1, 0};
		seam.Destination = core::CFrame(core::Vector3{20, 7, -30}) * core::CFrame::Angles(.2f, .8f, .4f);
		seam.Scale = 1.5f;
		seam.Surface = 3;
		return seam;
	}
	View Viewer() {
		View view;
		view.World = 7;
		view.WorldName = core::Name("near-room");
		view.CameraFrame = core::CFrame(core::Vector3{0, 0, 8});
		return view;
	}
	constexpr PortalImageDemandSettings SETTINGS{.Width = 1280, .Height = 720};
	glm::mat4 ReceivedProjection(const PortalImageRequest &request, const core::CFrame &frame) {
		scene::SurfaceLens lens;
		const auto &f = request.Frustum;
		lens.Left = f[0];
		lens.Right = f[1];
		lens.Bottom = f[2];
		lens.Top = f[3];
		lens.NearPlane = f[4];
		lens.FarPlane = f[5];
		lens.ClipNormal = {request.ClipPlane[0], request.ClipPlane[1], request.ClipPlane[2]};
		lens.ClipDistance = -request.ClipPlane[3];
		return scene::SurfaceProjection(lens, frame);
	}
}

TEST_CASE("whole-eye requests preserve off-axis cameras and bound pixels", "[render][portal-eye-demand]") {
	auto eye = Viewer();
	eye.CameraFrame = core::CFrame(core::Vector3{40, -7, 15}) * core::CFrame::Angles(.2f, -.6f, .4f);
	eye.Camera.NearPlane = .2f;
	eye.Camera.FarPlane = 150;
	eye.Projection = glm::frustumRH_ZO(-.3f, .5f, -.2f, .4f, .2f, 150.f);
	auto settings = SETTINGS;
	settings.MaximumExtent = 320;
	PortalImageDemand demand;
	auto &request = demand.Request;
	REQUIRE(
		BuildPortalEyeDemand(core::Name("player-eye"), eye, settings, demand) == PortalDemandStatus::Ready
	);
	CHECK(request.Projection == PortalImageProjection::Eye);
	CHECK_FALSE(request.Entrance.has_value());
	CHECK(request.Geometry.empty());
	CHECK(request.Width == 320);
	CHECK(request.Height == 180);
	CHECK(demand.Binding.World == eye.World);
	CHECK(demand.Binding.WorldName == eye.WorldName);
	CHECK(demand.Binding.ViewSlot == eye.Slot);
	CHECK(demand.Binding.Portal == core::Name("player-eye"));
	const auto sample = eye.CameraFrame.PointToWorldSpace({.5f, -.2f, -4});
	const glm::vec4 point{sample.X, sample.Y, sample.Z, 1};
	const auto expectedClip = *eye.Projection * eye.CameraFrame.Inverse().ToMatrix() * point;
	const auto capturedClip = demand.Binding.Sampling * point;
	CHECK(glm::length(capturedClip - expectedClip) < .0001f);
	CHECK(capturedClip.w == Catch::Approx(4.f).margin(.00001f));
	const auto reconstructed = ReceivedProjection(request, eye.CameraFrame);
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			CHECK(
				reconstructed[column][row] == Catch::Approx((*eye.Projection)[column][row]).margin(.00001f)
			);
	const auto original = request;
	eye.CameraFrame.Position.X += .1f;
	REQUIRE(
		BuildPortalEyeDemand(core::Name("player-eye"), eye, settings, demand) == PortalDemandStatus::Ready
	);
	CHECK(request.Key.CameraRevision != original.Key.CameraRevision);
	const auto moved = request;
	const auto movedSampling = demand.Binding.Sampling;
	settings.PixelBudget = 320 * 180 - 1;
	CHECK(
		BuildPortalEyeDemand(core::Name("player-eye"), eye, settings, demand) == PortalDemandStatus::Invalid
	);
	CHECK(request == moved);
	CHECK(demand.Binding.Sampling == movedSampling);
	settings.PixelBudget = SETTINGS.PixelBudget;
	(*eye.Projection)[0][2] = .1f;
	CHECK(
		BuildPortalEyeDemand(core::Name("player-eye"), eye, settings, demand) ==
		PortalDemandStatus::Unsupported
	);
	CHECK(request == moved);
	CHECK(demand.Binding.Sampling == movedSampling);
}

TEST_CASE(
	"portal image demand preserves both-face rays through rolled scaled seams", "[render][portal-demand]"
) {
	const auto seam = Seam();
	const auto through = scene::SeamMapping(seam);
	auto viewer = Viewer();
	for (const float elevation : {-30.0f, 0.0f, 30.0f}) {
		for (const float azimuth :
			 {-175.0f, -135.0f, -95.0f, -85.0f, -45.0f, -5.0f, 5.0f, 45.0f, 85.0f, 95.0f, 135.0f, 175.0f}) {
			CAPTURE(elevation, azimuth);
			const float vertical = elevation * std::numbers::pi_v<float> / 180;
			const float horizontal = azimuth * std::numbers::pi_v<float> / 180;
			const core::Vector3 eye =
				core::Vector3{
					std::cos(vertical) * std::sin(horizontal),
					std::sin(vertical),
					std::cos(vertical) * std::cos(horizontal)
				} *
				8;
			viewer.CameraFrame = core::CFrame::LookAt(eye, {}) * core::CFrame::Angles(0, 0, .3f);
			PortalImageDemand demand;
			REQUIRE(
				BuildPortalImageDemand(seam, core::Name("Door.View"), viewer, 17, SETTINGS, demand) ==
				PortalDemandStatus::Ready
			);
			CHECK(demand.Binding.World == viewer.World);
			CHECK(demand.Binding.ViewSlot == 17);
			CHECK(demand.Portal.ExternalImage);
			CHECK(demand.Portal.Index == seam.Surface);
			CHECK(demand.DestinationWorld == seam.DestinationWorld);
			CHECK(demand.Request.Width == 512);
			CHECK(demand.Request.Height == 288);
			const auto mappedEye = through.Point(eye);
			for (size_t axis = 0; axis < 3; ++axis) {
				const std::array<float, 3> expected{mappedEye.X, mappedEye.Y, mappedEye.Z};
				CHECK(demand.Request.Position[axis] == Catch::Approx(expected[axis]).margin(1e-5f));
			}
			const auto &q = demand.Request.Orientation;
			const auto frame = core::CFrame(mappedEye, glm::quat(q[3], q[0], q[1], q[2]));
			CHECK(frame.LookVector().FuzzyEq(through.Rotate(viewer.CameraFrame.LookVector()), 1e-5f));
			CHECK(frame.UpVector().FuzzyEq(through.Rotate(viewer.CameraFrame.UpVector()), 1e-5f));
			const auto captured =
				scene::ResolveSurfaceCamera(frame, ReceivedProjection(demand.Request, frame));
			const auto direct = scene::ResolveCamera(viewer.CameraFrame, viewer.Camera, 1280.0f / 720.0f);
			for (const auto sample : {core::Vector3{}, seam.First * .7f, seam.Second * -.6f}) {
				const auto mapped = through.Point(sample);
				const auto sourceClip = direct.ViewProjection * glm::vec4(sample.X, sample.Y, sample.Z, 1);
				const auto remoteClip = captured.ViewProjection * glm::vec4(mapped.X, mapped.Y, mapped.Z, 1);
				const auto bindingClip = demand.Binding.Sampling * glm::vec4(sample.X, sample.Y, sample.Z, 1);
				const float depth = (mapped - frame.Position).Dot(frame.LookVector());
				CHECK(bindingClip.w == Catch::Approx(depth).margin(.00003f));
				for (int axis = 0; axis < 2; ++axis) {
					CHECK(
						remoteClip[axis] / remoteClip.w ==
						Catch::Approx(sourceClip[axis] / sourceClip.w).margin(3e-5f)
					);
					CHECK(
						bindingClip[axis] / bindingClip.w ==
						Catch::Approx(remoteClip[axis] / remoteClip.w).margin(3e-5f)
					);
				}
			}
			std::vector<std::byte> wire;
			std::string error;
			demand.Request.Key.RequestId = 1;
			REQUIRE(EncodePortalImageRequest(demand.Request, wire, error));
			PortalImageRequest decoded;
			REQUIRE(DecodePortalImageRequest(wire, decoded, error));
			CHECK(decoded == demand.Request);
		}
	}
}

TEST_CASE(
	"portal image request identity changes only with effective view or seam inputs", "[render][portal-demand]"
) {
	auto seam = Seam();
	auto viewer = Viewer();
	PortalImageDemand first, current;
	REQUIRE(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, first) ==
		PortalDemandStatus::Ready
	);
	REQUIRE(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, current) ==
		PortalDemandStatus::Ready
	);
	CHECK(current.Request == first.Request);
	viewer.CameraFrame.Position.X += .25f;
	REQUIRE(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, current) ==
		PortalDemandStatus::Ready
	);
	CHECK(current.Request.Key.CameraRevision != first.Request.Key.CameraRevision);
	CHECK(current.Request.Key.SeamRevision == first.Request.Key.SeamRevision);
	viewer = Viewer();
	seam.Destination.Position.X += .5f;
	REQUIRE(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, current) ==
		PortalDemandStatus::Ready
	);
	CHECK(current.Request.Key.CameraRevision == first.Request.Key.CameraRevision);
	CHECK(current.Request.Key.SeamRevision != first.Request.Key.SeamRevision);
	seam = Seam();
	viewer.Projection = glm::frustum(-.08f, .12f, -.03f, .07f, .1f, 500.0f);
	REQUIRE(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, current) ==
		PortalDemandStatus::Ready
	);
	CHECK(current.Request.Frustum[0] == Catch::Approx(-.08f));
	CHECK(current.Request.Frustum[1] == Catch::Approx(.12f));
	CHECK(current.Request.Frustum[2] == Catch::Approx(-.03f));
	CHECK(current.Request.Frustum[3] == Catch::Approx(.07f));
	CHECK(current.Request.Key.CameraRevision != first.Request.Key.CameraRevision);
}

TEST_CASE(
	"portal demand leaves prior output untouched when hidden or unsupported", "[render][portal-demand]"
) {
	auto seam = Seam();
	auto viewer = Viewer();
	PortalImageDemand demand;
	REQUIRE(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
		PortalDemandStatus::Ready
	);
	const auto previous = demand.Request;
	viewer.CameraFrame = core::CFrame::LookAt({0, 0, 8}, {0, 0, 9});
	CHECK(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
		PortalDemandStatus::Hidden
	);
	CHECK(demand.Request == previous);
	viewer = Viewer();
	viewer.Projection = glm::ortho(-4.0f, 4.0f, -3.0f, 3.0f, .1f, 500.0f);
	CHECK(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
		PortalDemandStatus::Unsupported
	);
	CHECK(demand.Request == previous);
	viewer = Viewer();
	seam.Scale = std::numeric_limits<float>::quiet_NaN();
	CHECK(
		BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
		PortalDemandStatus::Invalid
	);
	CHECK(demand.Request == previous);
}

TEST_CASE("portal demand rejects degenerate seam inputs before culling", "[render][portal-demand]") {
	for (int input = 0; input < 7; ++input) {
		CAPTURE(input);
		auto seam = Seam();
		auto viewer = Viewer();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		switch (input) {
		case 0:
			seam.Normal.X = nan;
			break;
		case 1:
			seam.Centre.Y = nan;
			break;
		case 2:
			seam.First = {};
			break;
		case 3:
			seam.Second = seam.First;
			break;
		case 4:
			seam.Up.Z = nan;
			break;
		case 5:
			viewer.CameraFrame = core::CFrame({0, 0, 8}, glm::quat(0, 0, 0, 0));
			break;
		case 6:
			seam.Destination = core::CFrame({20, 7, -30}, glm::quat(0, 0, 0, 0));
			break;
		}
		PortalImageDemand demand;
		demand.Request.Key.PortalKey = "unchanged";
		CHECK(
			BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
			PortalDemandStatus::Invalid
		);
		CHECK(demand.Request.Key.PortalKey == "unchanged");
	}
}

TEST_CASE("shrinking exits keep the visible aperture during close approach", "[render][portal-demand]") {
	for (const float scale : {.025f, .05f, .5f, 2.0f}) {
		for (const float side : {-.02f, .02f}) {
			CAPTURE(scale, side);
			auto seam = Seam();
			seam.Scale = scale;
			seam.Destination = core::CFrame{};
			auto viewer = Viewer();
			viewer.CameraFrame = core::CFrame::LookAt({0, 0, side}, {});
			PortalImageDemand demand;
			REQUIRE(
				BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
				PortalDemandStatus::Ready
			);
			const auto through = scene::SeamMapping(seam);
			const auto frame = through.Place(viewer.CameraFrame);
			const auto &plane = demand.Request.ClipPlane;
			const core::Vector3 normal{plane[0], plane[1], plane[2]};
			CHECK(normal.Dot(frame.Position) + plane[3] < 0);
			CHECK(normal.Dot(through.Point(seam.Centre)) + plane[3] > 0);
			const auto matrices =
				scene::ResolveSurfaceCamera(frame, ReceivedProjection(demand.Request, frame));
			const auto centre = through.Point(seam.Centre);
			const auto clip = matrices.ViewProjection * glm::vec4(centre.X, centre.Y, centre.Z, 1);
			REQUIRE(clip.w > 0);
			CHECK(clip.z >= 0);
			CHECK(clip.z <= clip.w);
		}
	}
}

TEST_CASE("default portal lens keeps a two millimetre approach visible", "[render][portal-demand]") {
	for (const float scale : {.5f, 1.0f, 2.0f}) {
		for (const float side : {-.002f, .002f}) {
			auto seam = Seam();
			seam.Scale = scale;
			seam.Destination = {};
			auto viewer = Viewer();
			viewer.CameraFrame = core::CFrame::LookAt({0, 0, side}, {});
			PortalImageDemand demand;
			CAPTURE(scale, side);
			CHECK(
				BuildPortalImageDemand(seam, core::Name("Door"), viewer, 0, SETTINGS, demand) ==
				PortalDemandStatus::Ready
			);
		}
	}
}

#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Part.hpp>

TEST_CASE("authored portal demand claims mouths without mutating world cameras", "[render][portal-demand]") {
	scene::RegisterSceneClasses();
	ecs::Store store("near-room");
	const auto workspace = scene::InstallServices(store);
	scene::PartDesc part;
	part.Frame.Position = {0, 0, -4};
	part.Size = {4, 4, .1f};
	const auto entrance = scene::MakePart(store, part);
	REQUIRE(store.SetParent(entrance, workspace));
	part.Frame.Position = {20, 0, -4};
	const auto exit = scene::MakePart(store, part);
	REQUIRE(store.SetParent(exit, workspace));
	const auto camera = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Door");
	REQUIRE(store.SetParent(camera, entrance));
	auto link = *store.Get<scene::Portal>(camera);
	link.Destination = exit;
	link.DestinationWorld = core::Name("other-room");
	store.Set(camera, link);
	std::vector<scene::SurfaceSlot> slots;
	scene::GatherSurfaceSlots(store, slots);
	REQUIRE(slots.size() == 1);
	const auto before = *store.Get<scene::SurfaceCamera>(camera);
	const auto beforePose = store.Get<scene::Transform>(camera)->Frame;
	auto viewer = Viewer();
	viewer.Slot = 2;
	std::vector<PortalImageDemand> demands;
	std::vector<PortalView> portals;
	const auto gathered = CollectPortalImageDemands(store, viewer, SETTINGS, demands, portals, slots);
	REQUIRE(gathered.Ready == 1);
	REQUIRE(demands.size() == 1);
	REQUIRE(portals.size() == 1);
	CHECK(demands[0].Binding.ViewSlot == 2);
	CHECK(demands[0].Request.Key.PortalKey == store.GetFullName(camera));
	CHECK(portals[0].Index == slots[0].Index);
	CHECK(portals[0].ExternalImage);
	CHECK(store.Get<scene::SurfaceCamera>(camera)->Surface == before.Surface);
	CHECK(store.Get<scene::Transform>(camera)->Frame.Position == beforePose.Position);
	CHECK(store.Resource<scene::ActiveCamera>() == nullptr);
	std::vector<SurfaceView> surfaces;
	CHECK(CollectSurfaceViews(store, surfaces, portals, &viewer, slots) == 0);

	std::vector<scene::PortalSeam> seams;
	scene::GatherPortalSeams(store, seams);
	REQUIRE(seams.size() == 1);
	scene::PartDesc bodyPart;
	bodyPart.Frame.Position = seams[0].Centre;
	const auto body = scene::MakePart(store, bodyPart);
	scene::DrawInstance bodyRow;
	bodyRow.Source = body.Id;
	bodyRow.Frame = bodyPart.Frame;
	bodyRow.Texture = core::Name("body/colour");
	bodyRow.SkinFirst = 1;
	bodyRow.SkinCount = 1;
	std::vector<scene::DrawInstance> rows{bodyRow};
	std::vector<core::CFrame> joints{core::CFrame{}, core::CFrame(core::Vector3{1, 2, 3})};
	viewer.Instances = rows;
	viewer.JointFrames = joints;
	portals.clear();
	REQUIRE(CollectPortalImageDemands(store, viewer, SETTINGS, demands, portals, slots).Ready == 1);
	REQUIRE(demands.size() == 1);
	std::vector<scene::DrawInstance> received;
	std::vector<core::CFrame> receivedJoints;
	std::string error;
	REQUIRE(
		AppendPortalDraws(demands[0].Request.Geometry, viewer.WorldName, received, receivedJoints, error)
	);
	REQUIRE(received.size() == 1);
	CHECK(
		received[0].Frame.Position.FuzzyEq(scene::SeamMapping(seams[0]).Point(bodyRow.Frame.Position), 1e-5f)
	);
	CHECK(received[0].SeamNormal.Magnitude() == Catch::Approx(1));
	CHECK(received[0].Texture == bodyRow.Texture);
	CHECK(received[0].Source == 0);
	REQUIRE(receivedJoints.size() == 1);
	CHECK(receivedJoints[0].Position == joints[1].Position);
	CHECK(rows[0].Frame.Position == bodyRow.Frame.Position);
	CHECK(rows[0].SeamNormal == core::Vector3{});
	CHECK(store.Get<scene::SurfaceCamera>(camera)->Surface == before.Surface);

	std::vector<std::byte> eyeGeometry;
	REQUIRE(CollectPortalEyeGeometry(store, core::Name("other-room"), rows, joints, eyeGeometry, error));
	CHECK(eyeGeometry == demands[0].Request.Geometry);
	const auto baselineGeometry = eyeGeometry;
	// A standalone scene may collect images before a universe is constructed.
	world::RegisterMailboxTypes();
	CHECK(ecs::Components::Describe(ecs::Components::Of<world::Replica>()).Name.Text() == "world.Replica");
	std::vector<scene::DrawInstance> mixed = rows;
	mixed.push_back(bodyRow);
	mixed.back().SourceWorld = core::Name("unrelated-world");
	mixed.push_back(bodyRow);
	mixed.back().Variant = 99;
	REQUIRE(CollectPortalEyeGeometry(store, core::Name("other-room"), mixed, joints, eyeGeometry, error));
	CHECK(eyeGeometry == baselineGeometry);
	std::rotate(mixed.begin(), mixed.begin() + 1, mixed.end());
	REQUIRE(CollectPortalEyeGeometry(store, core::Name("other-room"), mixed, joints, eyeGeometry, error));
	CHECK(eyeGeometry == baselineGeometry);
	REQUIRE(CollectPortalEyeGeometry(store, core::Name("elsewhere"), rows, joints, eyeGeometry, error));
	CHECK(eyeGeometry.empty());
	REQUIRE(CollectPortalEyeGeometry(store, core::Name(store.Name()), rows, joints, eyeGeometry, error));
	CHECK(eyeGeometry.empty());
	CHECK_FALSE(CollectPortalEyeGeometry(store, {}, rows, joints, eyeGeometry, error));
	CHECK(eyeGeometry.empty());
	std::vector<scene::DrawInstance> crowded(MAX_PORTAL_GEOMETRY_ROWS + 1, bodyRow);
	eyeGeometry = baselineGeometry;
	CHECK_FALSE(
		CollectPortalEyeGeometry(store, core::Name("other-room"), crowded, joints, eyeGeometry, error)
	);
	CHECK(eyeGeometry == baselineGeometry);

	auto layeredSettings = SETTINGS;
	layeredSettings.ComposePlayerBody = true;
	const auto player = scene::AddPlayer(store, "viewer", false, 91);
	store.SetResource(scene::LocalPlayer{player});
	portals.clear();
	REQUIRE(CollectPortalImageDemands(store, viewer, layeredSettings, demands, portals, slots).Ready == 1);
	REQUIRE(demands.size() == 1);
	CHECK(demands[0].Request.OrderedLayers);
	CHECK(demands[0].Request.EyePlayer == "91");
	CHECK(demands[0].Request.Scope == PortalImageScope::OpaqueLighting);
	CHECK(demands[0].Request.RecursionDepth == 0);
	CHECK(demands[0].Request.Width <= 256);
	CHECK(demands[0].Request.Height <= 256);
	CHECK(
		uint64_t(demands[0].Request.Width) * demands[0].Request.Height * 4 <= demands[0].Request.PixelBudget
	);
	CHECK(demands[0].Request.Geometry == baselineGeometry);
	store.RemoveResource<scene::LocalPlayer>();

	rows[0].SkinCount = 2;
	portals.clear();
	CHECK(CollectPortalImageDemands(store, viewer, SETTINGS, demands, portals, slots).Invalid == 1);
	CHECK(demands.empty());
	CHECK_FALSE(CollectPortalEyeGeometry(store, core::Name("other-room"), rows, joints, eyeGeometry, error));
	CHECK(eyeGeometry == baselineGeometry);
	REQUIRE(portals.size() == 1);
	CHECK(portals[0].ExternalImage);
	rows[0].SkinCount = 1;

	auto filtered = before;
	filtered.TagFilter = 1;
	store.Set(camera, filtered);
	portals.clear();
	CHECK(CollectPortalImageDemands(store, viewer, SETTINGS, demands, portals, slots).Unsupported == 1);
	CHECK(demands.empty());
	REQUIRE(portals.size() == 1);
	CHECK(portals[0].ExternalImage);
	CHECK(CollectSurfaceViews(store, surfaces, portals, &viewer, slots) == 0);

	store.Set(camera, before);
	const auto duplicate = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Door");
	REQUIRE(store.SetParent(duplicate, entrance));
	store.Set(duplicate, link);
	scene::GatherSurfaceSlots(store, slots);
	portals.clear();
	CHECK(CollectPortalImageDemands(store, viewer, SETTINGS, demands, portals, slots).Invalid == 2);
	CHECK(demands.empty());
	CHECK(portals.size() == 2);
	CHECK_FALSE(CollectPortalEyeGeometry(store, core::Name("other-room"), rows, joints, eyeGeometry, error));
	CHECK(eyeGeometry == baselineGeometry);
	CHECK(CollectSurfaceViews(store, surfaces, portals, &viewer, slots) == 0);
}

TEST_CASE(
	"whole-eye player changes invalidate the requested camera", "[render][portal-eye-demand][eye-body]"
) {
	auto eye = Viewer();
	PortalImageDemand initial, selected;
	REQUIRE(BuildPortalEyeDemand(core::Name("eye"), eye, SETTINGS, initial) == PortalDemandStatus::Ready);
	eye.EyePlayer = 91;
	REQUIRE(BuildPortalEyeDemand(core::Name("eye"), eye, SETTINGS, selected) == PortalDemandStatus::Ready);
	CHECK(selected.Request.EyePlayer == "91");
	CHECK(selected.Request.Key.CameraRevision != initial.Request.Key.CameraRevision);
	eye.EyePlayer = 92;
	PortalImageDemand changed;
	REQUIRE(BuildPortalEyeDemand(core::Name("eye"), eye, SETTINGS, changed) == PortalDemandStatus::Ready);
	CHECK(changed.Request.Key.CameraRevision != selected.Request.Key.CameraRevision);
}
