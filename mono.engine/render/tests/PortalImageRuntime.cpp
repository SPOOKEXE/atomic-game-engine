#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <cstdlib>
#include <limits>

TEST_SUITE_ID("engine.render.portalimageruntime")
TEST_DEPENDS("engine.render.portalimageinbox")
TEST_DEPENDS("engine.world.presentationbus")

TEST_CASE("captured lighting prepares a view without partial updates", "[render][capture-lighting]") {
	using namespace engine;
	render::PortalCaptureLighting light;
	light.Direction = {0, 0, -1};
	light.Ambient = {.1f, .2f, .3f};
	light.OutdoorAmbient = {.4f, .5f, .6f};
	light.Direct = {2, 3, 4};
	light.FogColour = {.7f, .8f, .9f};
	light.FogStart = 5;
	light.FogEnd = 20;
	light.LightCount = 2;
	light.Lights[0].Position = {1, 2, 3};
	light.Lights[0].Colour = {4, 5, 6};
	light.Lights[1].ConeCosine = .5f;
	std::array<render::SceneLight, 3> storage;
	storage[2].Range = 71;
	render::View view;
	view.World = 91;
	view.Slot = 3;
	view.CameraFrame.Position = {7, 8, 9};
	view.Lighting.VolumeCount = 1;
	REQUIRE(render::ResolvePortalCaptureLighting(light, view, storage));
	CHECK(view.OverrideLighting);
	CHECK(view.World == 91);
	CHECK(view.Slot == 3);
	CHECK(view.CameraFrame.Position == core::Vector3{7, 8, 9});
	CHECK(view.Lighting.VolumeCount == 1);
	CHECK(view.Lighting.Direction == core::Vector3{0, 0, -1});
	CHECK(view.Lighting.Ambient == core::Color3{.1f, .2f, .3f});
	CHECK(view.Lighting.OutdoorAmbient == core::Color3{.4f, .5f, .6f});
	CHECK(view.Lighting.Direct == core::Color3{2, 3, 4});
	CHECK(view.Lighting.FogColor == core::Color3{.7f, .8f, .9f});
	CHECK(view.Lighting.FogStart == 5);
	CHECK(view.Lighting.FogEnd == 20);
	REQUIRE(view.Lights.size() == 2);
	CHECK(view.Lights.data() == storage.data());
	CHECK(view.Lights[0].Position == core::Vector3{1, 2, 3});
	CHECK(view.Lights[0].Colour == core::Color3{4, 5, 6});
	CHECK(view.Lights[1].ConeCosine == .5f);
	CHECK(storage[2].Range == 71);
	for (const bool shortStorage : {false, true}) {
		auto invalid = light;
		if (!shortStorage) invalid.Lights[1].Range = -1;
		view.OverrideLighting = false;
		view.Lighting.Direct = {9, 8, 7};
		storage[0].Range = 73;
		CHECK_FALSE(
			render::ResolvePortalCaptureLighting(
				invalid, view, std::span(storage).first(shortStorage ? 1 : storage.size())
			)
		);
		CHECK_FALSE(view.OverrideLighting);
		CHECK(view.Lighting.Direct == core::Color3{9, 8, 7});
		CHECK(view.Lights.data() == storage.data());
		CHECK(view.Lights.size() == 2);
		CHECK(storage[0].Range == 73);
	}
	light.Lights = {};
	light.LightCount = 0;
	REQUIRE(render::ResolvePortalCaptureLighting(light, view, {}));
	CHECK(view.Lights.empty());
}

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr PortalImageInbox::Time START{};
	struct RuntimeWorlds {
		world::Universe Universe;
		world::WorldId Source = Universe.Create({.Name = core::Name("portal-source")});
		world::WorldId Destination = Universe.Create({.Name = core::Name("portal-destination")});
		world::PresentationAddress Replies;
		world::PresentationAddress Requests;
		RuntimeWorlds(uint32_t perEndpoint = 8) {
			world::PresentationLimits limits;
			limits.MessagesPerEndpoint = perEndpoint;
			REQUIRE(Universe.ConfigurePresentation(700, limits));
			Replies = Universe.OpenPresentation(Source, core::Name(PORTAL_REPLY_CHANNEL)).Address;
			Requests = Universe.OpenPresentation(Destination, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
		}
	};
	PortalImageRequest Request() {
		PortalImageRequest request;
		request.Key.PortalKey = "Door";
		request.Key.CameraRevision = 1;
		request.Width = request.Height = 8;
		request.PixelBudget = 64;
		request.Frustum = {-0.1f, 0.1f, -0.1f, 0.1f, 0.1f, 100};
		request.ClipPlane = {0, 0, -1, -1};
		return request;
	}
	PortalImageBinding Binding() {
		PortalImageBinding binding;
		binding.World = 0;
		binding.WorldName = core::Name("portal-source");
		binding.Portal = core::Name("Door");
		return binding;
	}
}

TEST_CASE("portal requests restart without accepting old replies", "[render][portal-transport-restart]") {
	RuntimeWorlds worlds;
	Renderer sourceRenderer, destinationRenderer;
	PortalImageSource source(worlds.Universe, sourceRenderer, worlds.Source, worlds.Replies);
	PortalImageProducer producer(worlds.Universe, destinationRenderer, worlds.Destination, worlds.Requests);
	const auto first = source.Issue(worlds.Requests, Request(), Binding(), START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	CHECK(source.Issue(worlds.Requests, Request(), Binding(), START).Status == PortalInboxStatus::Busy);
	source.RestartRequests();
	const auto next = source.Issue(worlds.Requests, Request(), Binding(), START);
	REQUIRE(next.Status == PortalInboxStatus::Issued);
	CHECK(next.RequestId != first.RequestId);
	CHECK(producer.Pump(0, 0, START).Requests == 2);
	const auto replies = source.Poll(START);
	REQUIRE(replies.size() == 1);
	CHECK(replies.front().RequestId == next.RequestId);
}

TEST_CASE(
	"portal runtime reports unsupported request profiles and unavailable devices", "[render][portal-runtime]"
) {
	RuntimeWorlds worlds;
	Renderer sourceRenderer, destinationRenderer;
	PortalImageSource source(worlds.Universe, sourceRenderer, worlds.Source, worlds.Replies);
	PortalImageProducer producer(worlds.Universe, destinationRenderer, worlds.Destination, worlds.Requests);
	const auto issued = source.Issue(worlds.Requests, Request(), Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	CHECK(issued.Transport == world::PresentationStatus::Ok);
	CHECK(source.Poll(START).empty());
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
	const auto produced = producer.Pump(0, 0, START);
	CHECK(produced.Requests == 1);
	CHECK(produced.Rendered == 0);
	CHECK(produced.Sent == 1);
	const auto replies = source.Poll(START);
	REQUIRE(replies.size() == 1);
	CHECK(replies[0].RequestId == issued.RequestId);
	CHECK(replies[0].Status == PortalImageStatus::Unavailable);
	CHECK_FALSE(replies[0].Diagnostic.empty());
	CHECK(source.Image("Door") == 0);
	CHECK(source.Poll(START).empty());
	auto recursive = Request();
	recursive.Scope = PortalImageScope::OpaqueLighting;
	recursive.RecursionDepth = 1;
	recursive.Key.CameraRevision++;
	REQUIRE(source.Issue(worlds.Requests, recursive, Binding(), START).Status == PortalInboxStatus::Issued);
	CHECK(producer.Pump(0, 0, START).Rendered == 0);
	REQUIRE(source.Poll(START).front().Status == PortalImageStatus::Unsupported);
}

TEST_CASE(
	"portal runtime rolls back refused transport and binds endpoint incarnations", "[render][portal-runtime]"
) {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	auto stale = worlds.Requests;
	++stale.Generation;
	const auto refused = source.Issue(stale, Request(), Binding(), START);
	CHECK(refused.Status == PortalInboxStatus::Invalid);
	CHECK(refused.Transport == world::PresentationStatus::StaleEndpoint);
	const auto accepted = source.Issue(worlds.Requests, Request(), Binding(), START);
	REQUIRE(accepted.Status == PortalInboxStatus::Issued);
	CHECK(accepted.RequestId > refused.RequestId);
	CHECK(source.Issue(worlds.Requests, Request(), Binding(), START).Status == PortalInboxStatus::Busy);
	source.InvalidateEndpoint(worlds.Requests);
	CHECK(source.Issue(worlds.Requests, Request(), Binding(), START).Status == PortalInboxStatus::Issued);
	source.InvalidatePortal("Door");
	CHECK(source.Image("Door") == 0);
	auto wrongChannel = worlds.Requests;
	wrongChannel.Channel = PORTAL_REPLY_CHANNEL;
	CHECK(source.Issue(wrongChannel, Request(), Binding(), START).Status == PortalInboxStatus::Invalid);
	auto wrongOwner = Binding();
	wrongOwner.WorldName = core::Name("different-world");
	CHECK(source.Issue(worlds.Requests, Request(), wrongOwner, START).Status == PortalInboxStatus::Invalid);
}

TEST_CASE(
	"portal producer validates correlation and returns unavailable without a device",
	"[render][portal-runtime]"
) {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	auto request = Request();
	request.Scope = PortalImageScope::OpaqueLighting;
	request.Key.RequestId = 7;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 8, wire) ==
		world::PresentationStatus::Ok
	);
	CHECK(producer.Pump(0, 0, START).Refused == 1);
	CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 7, wire) ==
		world::PresentationStatus::Ok
	);
	const auto progress = producer.Pump(0, 0, START);
	CHECK(progress.Rendered == 0);
	const auto messages = worlds.Universe.TakePresentation(worlds.Replies);
	REQUIRE(messages.size() == 1);
	PortalImageReply reply;
	REQUIRE(DecodePortalImageReply(messages.front().Payload, reply, error));
	CHECK(reply.Key == request.Key);
	CHECK(reply.Scope == PortalImageScope::OpaqueLighting);
	CHECK(reply.Status == PortalImageStatus::Unavailable);
}

TEST_CASE("portal producer retries a refused failure reply without recapturing", "[render][portal-runtime]") {
	RuntimeWorlds worlds(1);
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	const std::array junk{std::byte{0}};
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Destination, worlds.Requests, worlds.Replies, 999, junk) ==
		world::PresentationStatus::Ok
	);
	const auto request = source.Issue(worlds.Requests, Request(), Binding(), START);
	REQUIRE(request.Status == PortalInboxStatus::Issued);
	const auto refused = producer.Pump(0, 0, START);
	CHECK(refused.Rendered == 0);
	CHECK(refused.Sent == 0);
	CHECK(refused.Refused == 1);
	CHECK(source.Poll(START).empty());
	SECTION("mailbox drains before deadline") {
		const auto retry = producer.Pump(0, 0, START + std::chrono::milliseconds(1));
		CHECK(retry.Requests == 0);
		CHECK(retry.Rendered == 0);
		CHECK(retry.Sent == 1);
		const auto replies = source.Poll(START + std::chrono::milliseconds(1));
		REQUIRE(replies.size() == 1);
		CHECK(replies.front().RequestId == request.RequestId);
		CHECK(replies.front().Status == PortalImageStatus::Unavailable);
		CHECK(producer.Pump(0, 0, START + std::chrono::milliseconds(2)).Sent == 0);
	}
	SECTION("deadline releases the retained failure") {
		const auto expired = producer.Pump(0, 0, START + std::chrono::seconds(1));
		CHECK(expired.Sent == 0);
		CHECK(expired.Refused == 1);
		CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());
		CHECK(producer.Pump(0, 0, START + std::chrono::seconds(2)).Refused == 0);
	}
	SECTION("clear releases the retained failure") {
		producer.Clear();
		CHECK(producer.Pump(0, 0, START + std::chrono::milliseconds(1)).Sent == 0);
		CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());
	}
	SECTION("replacement endpoint receives no stale failure") {
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Replies) == world::PresentationStatus::Ok);
		const auto replacement =
			worlds.Universe.OpenPresentation(worlds.Source, core::Name(PORTAL_REPLY_CHANNEL));
		const auto retry = producer.Pump(0, 0, START + std::chrono::milliseconds(1));
		CHECK(retry.Sent == 0);
		CHECK(retry.Refused == 1);
		CHECK(worlds.Universe.TakePresentation(replacement.Address).empty());
		CHECK(producer.Pump(0, 0, START + std::chrono::milliseconds(2)).Refused == 0);
	}
}

TEST_CASE("moving portal cameras keep one request in flight", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	auto request = Request();
	REQUIRE(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Issued);
	for (uint64_t camera = 2; camera <= 100; ++camera) {
		request.Key.CameraRevision = camera;
		auto movingBinding = Binding();
		movingBinding.Sampling[3][0] = static_cast<float>(camera) * .001f;
		REQUIRE(
			source.Issue(worlds.Requests, request, movingBinding, START).Status == PortalInboxStatus::Busy
		);
	}
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
	CHECK(producer.Pump(0, 0, START).Requests == 1);
	REQUIRE(source.Poll(START).size() == 1);
	REQUIRE(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Issued);
	const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(messages.size() == 1);
	PortalImageRequest latest;
	std::string error;
	REQUIRE(DecodePortalImageRequest(messages.front().Payload, latest, error));
	CHECK(latest.Key.CameraRevision == 100);
	request.Key.SeamRevision++;
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Issued);
	REQUIRE(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
	const auto replacement =
		worlds.Universe.OpenPresentation(worlds.Destination, core::Name(PORTAL_REQUEST_CHANNEL));
	REQUIRE(replacement.Status == world::PresentationStatus::Ok);
	CHECK(source.Issue(replacement.Address, request, Binding(), START).Status == PortalInboxStatus::Issued);
}

TEST_CASE("portal source bounds configured expiry at steady clock maximum", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalInboxLimits limits;
	limits.Timeout = std::chrono::minutes(2);
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies, limits);
	const auto nearMaximum = PortalImageInbox::Time::max() - std::chrono::minutes(1);
	CHECK(
		source.Issue(worlds.Requests, Request(), Binding(), nearMaximum).Status == PortalInboxStatus::Invalid
	);
	CHECK(source.Poll(nearMaximum).empty());
	const auto valid = nearMaximum - std::chrono::minutes(2);
	CHECK(source.Issue(worlds.Requests, Request(), Binding(), valid).Status == PortalInboxStatus::Issued);
	CHECK(source.Poll(nearMaximum).empty());
	CHECK(source.Image("Door") == 0);
}

TEST_CASE(
	"source expiry retires its resident reservation at the configured deadline", "[render][portal-runtime]"
) {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalResidentImages images(renderer);
	PortalInboxLimits limits;
	limits.Timeout = std::chrono::milliseconds(20);
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies, limits, &images);
	auto request = Request();
	const auto issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	request.Key.RequestId = issued.RequestId;
	CHECK(images.Contains(worlds.Replies, worlds.Requests, request, START));
	CHECK(source.Poll(START + limits.Timeout).empty());
	CHECK_FALSE(images.Contains(worlds.Replies, worlds.Requests, request, START + limits.Timeout));
}

TEST_CASE("portal reply endpoints isolate views of the same world and mouth", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	CHECK(PortalReplyChannel().Text() == PORTAL_REPLY_CHANNEL);
	CHECK(PortalReplyChannel(17).Text() == "portal-image-replies/17");
	const auto second = worlds.Universe.OpenPresentation(worlds.Source, PortalReplyChannel(17));
	REQUIRE(second.Status == world::PresentationStatus::Ok);
	PortalImageSource firstSource(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	PortalImageSource secondSource(worlds.Universe, renderer, worlds.Source, second.Address);
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	auto secondBinding = Binding();
	secondBinding.ViewSlot = 17;
	const auto firstIssue = firstSource.Issue(worlds.Requests, Request(), Binding(), START);
	const auto secondIssue = secondSource.Issue(worlds.Requests, Request(), secondBinding, START);
	REQUIRE(firstIssue.Status == PortalInboxStatus::Issued);
	REQUIRE(secondIssue.Status == PortalInboxStatus::Issued);
	CHECK(firstIssue.RequestId == secondIssue.RequestId);
	auto moved = Request();
	moved.Key.CameraRevision++;
	CHECK(
		firstSource.Issue(worlds.Requests, moved, secondBinding, START).Status == PortalInboxStatus::Invalid
	);
	CHECK(secondSource.Issue(worlds.Requests, moved, Binding(), START).Status == PortalInboxStatus::Invalid);
	const auto progress = producer.Pump(0, 0, START);
	CHECK(progress.Requests == 2);
	CHECK(progress.Sent == 2);
	const auto secondReply = secondSource.Poll(START);
	REQUIRE(secondReply.size() == 1);
	CHECK(secondReply.front().RequestId == secondIssue.RequestId);
	CHECK(secondReply.front().Status == PortalImageStatus::Unavailable);
	CHECK(secondSource.Poll(START).empty());
	const auto firstReply = firstSource.Poll(START);
	REQUIRE(firstReply.size() == 1);
	CHECK(firstReply.front().RequestId == firstIssue.RequestId);
	CHECK(firstReply.front().Status == PortalImageStatus::Unavailable);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
}

TEST_CASE("capture camera reconstruction preserves off-axis and oblique rays", "[render][capture-camera]") {
	using namespace engine;
	const float scale = GENERATE(.25f, 1.f, 4.f);
	const bool oblique = GENERATE(false, true);
	const auto frame = core::CFrame(core::Vector3{7, -3, 2}) * core::CFrame::Angles(.2f, -.4f, .1f);
	const auto rotation = frame.Rotation();
	render::PortalCaptureCamera camera;
	camera.Position = {frame.Position.X, frame.Position.Y, frame.Position.Z};
	camera.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
	camera.Frustum = {-.12f * scale, .18f * scale, -.07f * scale, .13f * scale, .1f * scale, 80 * scale};
	if (oblique) {
		const auto normal = frame.VectorToWorldSpace(core::Vector3{.2f, 0, -1}.Unit());
		const auto plane = frame.PointToWorldSpace({0, 0, -scale});
		camera.ClipPlane = {normal.X, normal.Y, normal.Z, -normal.Dot(plane)};
	}
	render::View view;
	view.World = 91;
	view.Slot = 7;
	view.EyeImage = 42;
	REQUIRE(
		render::ResolvePortalCaptureCamera(
			camera, oblique ? render::PortalImageProjection::Seam : render::PortalImageProjection::Eye, view
		)
	);
	REQUIRE(view.Projection.has_value());
	CHECK(view.World == 91);
	CHECK(view.Slot == 7);
	CHECK(view.EyeImage == 42);
	CHECK(view.CameraFrame.Position == frame.Position);
	CHECK(view.Camera.NearPlane == camera.Frustum[4]);
	CHECK(view.Camera.FarPlane == camera.Frustum[5]);
	const auto matrices = scene::ResolveSurfaceCamera(view.CameraFrame, *view.Projection);
	for (size_t horizontal = 0; horizontal < 2; ++horizontal) {
		for (size_t vertical = 0; vertical < 2; ++vertical) {
			const auto point = frame.PointToWorldSpace(
				{camera.Frustum[horizontal] * 20, camera.Frustum[2 + vertical] * 20, -2 * scale}
			);
			const auto clip = matrices.ViewProjection * glm::vec4(point.X, point.Y, point.Z, 1);
			CHECK(std::abs(clip.x / clip.w - (horizontal ? 1.f : -1.f)) < .0001f);
			CHECK(std::abs(clip.y / clip.w - (vertical ? 1.f : -1.f)) < .0001f);
			CHECK(std::abs(clip.w - 2 * scale) < .0001f);
			CHECK(clip.z > 0);
			CHECK(clip.z < clip.w);
		}
	}
	if (oblique) {
		const auto point = frame.PointToWorldSpace({0, 0, -.5f * scale});
		const auto clip = matrices.ViewProjection * glm::vec4(point.X, point.Y, point.Z, 1);
		CHECK(clip.z < 0);
	}
}

TEST_CASE("invalid retained camera leaves its view unchanged", "[render][capture-camera]") {
	using namespace engine;
	render::PortalCaptureCamera camera;
	camera.Frustum = {-.1f, .1f, -.1f, .1f, .1f, 100};
	auto projection = render::PortalImageProjection::Eye;
	SECTION("nonfinite position") {
		camera.Position[0] = std::numeric_limits<float>::infinity();
	}
	SECTION("nonunit rotation") {
		camera.Orientation[3] = 2;
	}
	SECTION("reversed horizontal bounds") {
		std::swap(camera.Frustum[0], camera.Frustum[1]);
	}
	SECTION("zero near plane") {
		camera.Frustum[4] = 0;
	}
	SECTION("reversed depth bounds") {
		camera.Frustum[5] = .01f;
	}
	SECTION("eye plane cannot clip") {
		camera.ClipPlane = {0, 0, -1, -1};
	}
	SECTION("seam plane on the eye") {
		projection = render::PortalImageProjection::Seam;
		camera.ClipPlane = {0, 0, -1, 0};
	}
	SECTION("seam plane behind the eye") {
		projection = render::PortalImageProjection::Seam;
		camera.ClipPlane = {0, 0, -1, 1};
	}
	SECTION("nonunit seam normal") {
		projection = render::PortalImageProjection::Seam;
		camera.ClipPlane = {0, 0, -2, -1};
	}
	render::View view;
	view.CameraFrame.Position = {1, 2, 3};
	view.Camera.NearPlane = .7f;
	view.Projection = glm::mat4(2);
	CHECK_FALSE(render::ResolvePortalCaptureCamera(camera, projection, view));
	CHECK(view.CameraFrame.Position == core::Vector3(1, 2, 3));
	CHECK(view.Camera.NearPlane == .7f);
	REQUIRE(view.Projection.has_value());
	CHECK(*view.Projection == glm::mat4(2));
}

#include "RenderFixture.hpp"

#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Visibility.hpp>

TEST_CASE(
	"two renderers exchange destination radiance through owned world presentation traffic",
	"[render][gpu][portal-runtime][.]"
) {
	RuntimeWorlds worlds(1);
	bool pressure = false, expiry = false, complete = false, glass = false, spatialOnly = false,
		 cadence = false, beamOnly = false, particleOnly = false, billboardOnly = false, mirrorOnly = false,
		 mixedSurface = false, shortSurfaceBudget = false, childGlass = false, resident = false,
		 carriedBody = false, authoredBody = false, reciprocalMouth = false, publishedReplica = false,
		 alreadyPresented = false, renewal = false, cameraMotion = false, nestedWorld = false,
		 cancelNested = false, replaceNested = false, timeoutNested = false, moveNested = false,
		 appearNested = false, foregroundFloor = false, floorDefaultMaterial = false,
		 floorDefaultLighting = false, productEye = false, forwardBody = false, retireProducer = false,
		 replaceProducer = false, retirePending = false;
	int seamChange = 0;
	core::Vector3 authoredEye{0, 0, 4};
	const std::array<core::Vector3, 10> authoredEyes{
		{{0, 0, 4},
		 {-1, .5f, 4},
		 {1, -.5f, 4},
		 {0, 0, -4},
		 {-1, .5f, -4},
		 {1, -.5f, -4},
		 {0, 0, .0011f},
		 {0, 0, -.0111f},
		 {0, 0, -.003f},
		 {0, 0, -.007f}}
	};
	double expectedRed = .5;
	double expectedBlue = 0;
	bool eyeView = false;
	SECTION("full eye retains foreground floor beside a cross-world child") {
		foregroundFloor = eyeView = nestedWorld = complete = true;
		resident = GENERATE(false, true);
		floorDefaultMaterial = GENERATE(false, true);
		floorDefaultLighting = GENERATE(false, true);
		productEye = GENERATE(false, true);
		expectedRed = 0;
		expectedBlue = .5;
	}
	SECTION("copied parent waits for a cross-world child image") {
		nestedWorld = complete = true;
		expectedRed = 0;
		expectedBlue = .5;
	}
	SECTION("incoming body is visible in the child after clearing the parent mouth") {
		forwardBody = nestedWorld = complete = true;
		resident = GENERATE(false, true);
		expectedRed = .5;
		expectedBlue = 0;
	}
	SECTION("resident parent waits for a cross-world child image") {
		nestedWorld = resident = complete = true;
		expectedRed = 0;
		expectedBlue = .5;
	}
	SECTION("nested image rejects a budget that only fits its parent") {
		nestedWorld = complete = shortSurfaceBudget = true;
	}
	SECTION("clearing a waiting parent retires its child reply endpoint") {
		nestedWorld = complete = cancelNested = true;
	}
	SECTION("a waiting parent recovers after child endpoint replacement") {
		nestedWorld = complete = replaceNested = true;
		resident = GENERATE(false, true);
	}
	SECTION("a waiting parent releases timed out child work and retries") {
		nestedWorld = complete = timeoutNested = true;
		resident = GENERATE(false, true);
	}
	SECTION("a waiting parent refuses a moved child seam and retries") {
		nestedWorld = complete = moveNested = true;
		seamChange = GENERATE(0, 1, 2);
		resident = GENERATE(false, true);
	}
	SECTION("a waiting parent refuses a newly appearing child portal and retries") {
		nestedWorld = complete = appearNested = true;
		resident = GENERATE(false, true);
	}
	SECTION("endpoint invalidation") {}
	SECTION("a full eye view arrives through copied pixels without a seam plane") {
		eyeView = complete = true;
	}
	SECTION("a full eye view arrives through a resident image without a seam plane") {
		eyeView = resident = complete = true;
	}
	SECTION("a moving eye accepts its outstanding image before requesting a newer camera") {
		cameraMotion = eyeView = complete = true;
		resident = GENERATE(false, true);
	}
	SECTION("a moving portal accepts its image with the matching sampling matrix") {
		cameraMotion = complete = true;
		resident = GENERATE(false, true);
	}
	SECTION("unchanged copied image renews without capture") {
		renewal = complete = true;
	}
	SECTION("unchanged resident image renews without capture") {
		renewal = resident = complete = true;
	}

	SECTION("completed host presentation keeps its frame timing and draw rows") {
		alreadyPresented = publishedReplica = complete = true;
	}
	SECTION("replica capture retains the rows published by destination presentation") {
		publishedReplica = complete = true;
	}
	SECTION("resident replica capture retains destination presentation rows") {
		publishedReplica = resident = complete = true;
	}
	SECTION("shared renderer delivers an owned resident receipt without readback") {
		resident = complete = true;
	}
	SECTION("copied capture includes owned source body geometry") {
		carriedBody = complete = true;
	}
	SECTION("resident capture includes owned source body geometry") {
		carriedBody = resident = complete = true;
	}
	SECTION("authored seam collects a crossing body into copied destination pixels") {
		authoredBody = carriedBody = complete = true;
	}
	SECTION("authored seam collects a crossing body into resident destination pixels") {
		authoredBody = carriedBody = resident = complete = true;
	}
	SECTION("reciprocal destination mouth preserves the copied crossing image") {
		const auto sample = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
		authoredEye = authoredEyes[sample];
		CAPTURE(sample);
		reciprocalMouth = authoredBody = carriedBody = complete = true;
	}
	SECTION("reciprocal destination mouth preserves the resident crossing image") {
		const auto sample = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
		authoredEye = authoredEyes[sample];
		CAPTURE(sample);
		reciprocalMouth = authoredBody = carriedBody = resident = complete = true;
	}
	SECTION("retired producer cannot retain drawable readiness") {
		retireProducer = complete = true;
		resident = GENERATE(false, true);
		replaceProducer = GENERATE(false, true);
		retirePending = GENERATE(false, true);
	}
	SECTION("pending work does not extend image expiry") {
		expiry = true;
	}
	SECTION("completed pixels survive bus backpressure") {
		pressure = true;
	}
	SECTION("complete world keeps opaque radiance") {
		complete = true;
	}
	SECTION("complete world blends transparency before tone mapping") {
		complete = glass = true;
		expectedRed = .5 * (128.0 / 255.0);
	}
	SECTION("spatial interface is visible without drawable geometry") {
		complete = spatialOnly = true;
	}
	SECTION("request mirror works without the destination active camera") {
		complete = mirrorOnly = true;
	}
	SECTION("mirror and portal child captures share the requested camera chain") {
		complete = mirrorOnly = mixedSurface = true;
	}
	SECTION("overlapping glass is sorted for the portal child eye") {
		complete = mirrorOnly = mixedSurface = childGlass = true;
		const double alpha = 128.0 / 255.0;
		expectedRed = .5 * alpha;
		expectedBlue = .5 * alpha * (1 - alpha);
	}
	SECTION("a mixed capture refuses a budget missing its seam light fields") {
		complete = mirrorOnly = mixedSurface = shortSurfaceBudget = true;
	}
	SECTION("a beam inside a portal inside a mirror faces the child camera") {
		complete = mirrorOnly = mixedSurface = beamOnly = true;
		expectedRed = 128.0 / 255.0;
	}
	SECTION("billboard layout uses the requested frustum focal scale") {
		complete = spatialOnly = billboardOnly = true;
	}
	SECTION("particle-only destination retains linear radiance") {
		complete = particleOnly = true;
		expectedRed = 128.0 / 255.0;
	}
	SECTION("camera-facing beam uses the requested eye instead of retained world geometry") {
		complete = beamOnly = true;
		expectedRed = 128.0 / 255.0;
	}
	SECTION("unchanged camera receives continuously moving destination") {
		complete = cadence = true;
	}
	const uint32_t captureExtent = productEye ? 128 : 32;
	const uint32_t displayExtent = productEye ? 128 : 65;
	render::test::FixtureDevice sourceDevice, destinationDevice;
	sourceDevice.Initialise();
	if (!resident) {
		destinationDevice.Initialise();
	}
	auto &sourceRenderer = sourceDevice.Render;
	auto &destinationRenderer = resident ? sourceRenderer : destinationDevice.Render;
	PortalResidentImages residentImages(sourceRenderer);
	assets::MeshData mesh;
	mesh.Vertices = {
		{{-.5f, -.5f, 0}, {0, 0, 1}, {0, 1}},
		{{.5f, -.5f, 0}, {0, 0, 1}, {1, 1}},
		{{.5f, .5f, 0}, {0, 0, 1}, {1, 0}},
		{{-.5f, .5f, 0}, {0, 0, 1}, {0, 0}}
	};
	mesh.Indices = {0, 1, 2, 0, 2, 3};
	mesh.ComputeBounds();
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	for (auto *renderer : {&sourceRenderer, &destinationRenderer}) {
		REQUIRE(renderer->AddMesh(core::Name("runtime-plane"), mesh));

		// Portal parts expose both faces. A single front-facing triangle sheet would
		// disappear when the reflected camera sees its back.
		auto apertureMesh = mesh;
		apertureMesh.Indices.insert(apertureMesh.Indices.end(), {2, 1, 0, 3, 2, 0});
		REQUIRE(renderer->AddMesh(core::Name("runtime-aperture"), apertureMesh));
		if (authoredBody) {
			auto bodyMesh = apertureMesh;
			for (auto &vertex : bodyMesh.Vertices) {
				vertex.Position[2] = -.5f;
			}
			const auto back = bodyMesh.Vertices;
			for (auto vertex : back) {
				vertex.Position[2] = .5f;
				bodyMesh.Vertices.push_back(vertex);
			}
			for (const auto index : apertureMesh.Indices) {
				bodyMesh.Indices.push_back(index + 4);
			}
			bodyMesh.ComputeBounds();
			REQUIRE(renderer->AddMesh(core::Name("runtime-body"), bodyMesh));
		}
		REQUIRE(renderer->AddTexture(core::Name("runtime-white"), white));
	}
	scene::RegisterSceneClasses();
	RegisterPresentationComponents();
	effects::RegisterEffectClasses();
	ecs::Entity activeCamera, wallEntity, billboardEntity;
	gui::RegisterGuiClasses();
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc part;
		part.Frame.Position = {0, 0, -4};
		part.Size = {16, 16, .01f};
		part.Mesh = core::Name("runtime-plane");
		const auto wall = scene::MakePart(store, part);
		wallEntity = wall;
		REQUIRE(store.SetParent(wall, workspace));
		auto visual = *store.Get<scene::Visual>(wall);
		visual.Tint = {1, 0, 0};
		store.Set(wall, visual);
		auto appearance = *store.Get<scene::SurfaceAppearance>(wall);
		appearance.ColourMap = core::Name("runtime-white");
		store.Set(wall, appearance);
		if (publishedReplica) {
			scene::DrawInstance interpolated;
			interpolated.Source = wall.Id;
			interpolated.Frame = part.Frame;
			interpolated.HalfExtent = {8, 8, .005f};
			interpolated.Mesh = core::Name("runtime-plane");
			interpolated.Texture = core::Name("runtime-white");
			interpolated.Tint = {1, 0, 0};
			auto latest = *store.Get<scene::Transform>(wall);
			latest.Frame.Position.X = 100;
			store.Set(wall, latest);
			scheduler.Add(
				"publish-interpolated-rows", ecs::Phase::PreRender, [interpolated](ecs::Store &target) {
					target.SetResource(DrawList{.Instances = {interpolated}});
				}
			);
		}
		if (carriedBody) {
			visual.Transparency = 1;
			store.Set(wall, visual);
		}
		if (glass) {
			visual.Tint = {};
			store.Set(wall, visual);
			part.Frame.Position.Z = -3;
			const auto pane = scene::MakePart(store, part);
			REQUIRE(store.SetParent(pane, workspace));
			auto paneVisual = *store.Get<scene::Visual>(pane);
			paneVisual.Tint = {1, 0, 0};
			paneVisual.Transparency = .5f;
			store.Set(pane, paneVisual);
			store.Set(pane, appearance);
		}
		if (cadence) {
			part.Frame.Position.Z = -5;
			const auto rear = scene::MakePart(store, part);
			REQUIRE(store.SetParent(rear, workspace));
			auto rearVisual = *store.Get<scene::Visual>(rear);
			rearVisual.Tint = {0, 1, 0};
			store.Set(rear, rearVisual);
			store.Set(rear, appearance);
		}
		if (spatialOnly) {
			visual.Transparency = 1;
			store.Set(wall, visual);
			const auto collector = store.CreateInstance(
				gui::GuiClass(billboardOnly ? "BillboardGui" : "SurfaceGui"), "OnlyWorldInterface"
			);
			REQUIRE(store.SetParent(collector, wall));
			gui::Surface surface;
			surface.On = gui::Face::Back;
			surface.CanvasSize = {100, 100};
			surface.Brightness = .5f;
			if (billboardOnly) {
				gui::Billboard billboard;
				billboard.Size = {2, 0, 2, 0};
				billboard.Brightness = .5f;
				store.Set(collector, billboard);
				billboardEntity = collector;
			} else {
				store.Set(collector, surface);
			}
			const auto rectangle = store.CreateInstance(gui::GuiClass("Frame"), "RedPanel");
			REQUIRE(store.SetParent(rectangle, collector));
			gui::Element element;
			element.Size = {1, 0, 1, 0};
			store.Set(rectangle, element);
			gui::Background background;
			background.Color = {1, 0, 0};
			background.BorderSizePixel = 0;
			store.Set(rectangle, background);
		}

		if (particleOnly) {
			visual.Transparency = 1;
			store.Set(wall, visual);
			const auto origin =
				store.CreateInstance(ecs::Classes::Find(core::Name("Attachment")), "ParticleOrigin");
			REQUIRE(store.SetParent(origin, wall));
			scene::Attachment attachment;
			attachment.Frame.Position = {0, 0, 1};
			store.Set(origin, attachment);
			const auto emitter =
				store.CreateInstance(ecs::Classes::Find(core::Name("ParticleEmitter")), "RedParticle");
			REQUIRE(store.SetParent(emitter, origin));
			auto settings = *store.Get<effects::ParticleEmitter>(emitter);
			settings.Rate = 0;
			settings.Speed = core::NumberRange(0, 0);
			settings.Lifetime = core::NumberRange(10, 10);
			settings.Size = core::NumberSequence(3);
			settings.Colour = core::ColorSequence(core::Color3{.5f, 0, 0});
			settings.Texture = core::Name("runtime-white");
			store.Set(emitter, settings);
			effects::InstallParticles(store, 128);
			store.ResourceMutable<effects::ParticleSystem>()->DeviceStepped = true;
			scene::ResolveAttachments(store);
			REQUIRE(effects::RefreshEmitters(store) == 1);
			REQUIRE(effects::EmitParticles(store, emitter, 1));
		}

		if (beamOnly) {
			visual.Transparency = 1;
			store.Set(wall, visual);
			std::array<ecs::Entity, 2> ends;
			for (size_t index = 0; index < ends.size(); ++index) {
				ends[index] = store.CreateInstance(ecs::Classes::Find(core::Name("Attachment")), "BeamEnd");
				REQUIRE(store.SetParent(ends[index], wall));
				scene::Attachment attachment;
				attachment.Frame.Position = mixedSurface ? core::Vector3{0, index == 0 ? -2.0f : 2.0f, 1}
														 : core::Vector3{index == 0 ? -2.0f : 2.0f, 0, 1};
				store.Set(ends[index], attachment);
			}
			const auto beamEntity = store.CreateInstance(ecs::Classes::Find(core::Name("Beam")), "RedBeam");
			REQUIRE(store.SetParent(beamEntity, workspace));
			effects::Beam beam;
			beam.Attachment0 = ends[0];
			beam.Attachment1 = ends[1];
			beam.FaceCamera = true;
			beam.Width0 = beam.Width1 = mixedSurface ? 4 : 2;
			beam.Colour = core::ColorSequence(core::Color3{.5f, 0, 0});
			store.Set(beamEntity, beam);
			store.SetResource(effects::RibbonBuffer{});
			REQUIRE(effects::BuildRibbons(store, core::Vector3(0, 100, -3), 0) == 1);
		}

		if (mirrorOnly) {
			auto wallPlacement = *store.Get<scene::Transform>(wall);
			wallPlacement.Frame = mixedSurface ? core::CFrame(core::Vector3(20, 0, -6))
											   : core::CFrame(core::Vector3(0, 0, 2)) *
													 core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			store.Set(wall, wallPlacement);
			const auto makeSurface = [&](core::Vector3 position) {
				scene::PartDesc pane;
				pane.Frame.Position = position;
				pane.Size = {8, 8, .01f};
				pane.Mesh = core::Name("runtime-aperture");
				const auto partEntity = scene::MakePart(store, pane);
				REQUIRE(store.SetParent(partEntity, workspace));
				const auto camera =
					store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "Surface");
				REQUIRE(store.SetParent(camera, partEntity));
				auto surface = *store.Get<scene::SurfaceCamera>(camera);
				surface.Width = surface.Height = 32;
				surface.Surface = -1;
				surface.FPS = 0;
				store.Set(camera, surface);
				return std::pair{partEntity, camera};
			};
			(void)makeSurface({0, 0, -3});
			if (mixedSurface) {
				const auto entrance = makeSurface({0, 0, 1});
				const auto destination = makeSurface({20, 0, -3});
				scene::Portal portal;
				portal.Destination = destination.first;
				store.Set(entrance.second, portal);
				portal.Destination = entrance.first;
				store.Set(destination.second, portal);
			}
		}

		if (childGlass) {
			visual.Transparency = 1;
			store.Set(wall, visual);
			for (size_t index = 0; index < 2; ++index) {
				part.Frame.Position = {20, 0, index == 0 ? -4.0f : -6.0f};
				const auto sheet = scene::MakePart(store, part);
				REQUIRE(store.SetParent(sheet, workspace));
				auto tint = *store.Get<scene::Visual>(sheet);
				tint.Transparency = .5f;
				tint.Tint = index == 0 ? core::Color3{1, 0, 0} : core::Color3{0, 0, 1};
				store.Set(sheet, tint);
				store.Set(sheet, appearance);
			}
		}

		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
		activeCamera = store.CreateInstance(scene::CameraClass(), "UnrelatedCamera");
		scene::Transform transform;
		transform.Frame.Position = beamOnly ? core::Vector3(0, 100, -3) : core::Vector3(100, 100, 100);
		store.Set(activeCamera, transform);
		if (!mirrorOnly) {
			store.SetResource(scene::ActiveCamera{activeCamera});
		}
		if (publishedReplica && !alreadyPresented) {
			store.SetAdoptOnly(true);
		}
	});
	auto document = graph::DefaultPbrDocument();
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = core::Name("runtime-check"),
		 .NodeKind = core::Name("capture"),
		 .Scope = graph::NodeScope::Frame}
	);
	document.Record(
		{.Kind = graph::EditKind::Reads, .Target = core::Name("portaled"), .Key = core::Name("source")}
	);
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(sourceRenderer.SetPipeline(core::Name("runtime-source"), pipeline));
	PortalInboxLimits inboxLimits;
	if (timeoutNested) inboxLimits.Timeout = std::chrono::seconds(3);
	PortalImageSource source(
		worlds.Universe,
		sourceRenderer,
		worlds.Source,
		worlds.Replies,
		inboxLimits,
		resident ? &residentImages : nullptr
	);
	PortalImageProducer producer(
		worlds.Universe,
		destinationRenderer,
		worlds.Destination,
		worlds.Requests,
		resident ? &residentImages : nullptr
	);
	auto request = Request();
	request.Scope = complete ? PortalImageScope::CompleteWorld : PortalImageScope::OpaqueLighting;
	if (eyeView) {
		request.Projection = PortalImageProjection::Eye;
		request.ClipPlane = {};
	}
	request.Width = request.Height = captureExtent;
	// Two viewer-independent 128-square light fields are part of mixed capture work.
	request.PixelBudget = captureExtent * captureExtent *
							  (mixedSurface ? 3
							   : mirrorOnly ? 2
											: 1) +
						  (mixedSurface ? 2 * 128 * 128 : 0);
	request.RecursionDepth = mixedSurface ? 2 : mirrorOnly ? 1 : 0;
	if (carriedBody && !authoredBody) {
		PortalGeometry geometry;
		PortalGeometryRow row;
		row.Name = "Source.Character.Body";
		row.Assets[0] = "runtime-plane";
		row.Assets[1] = "runtime-white";
		row.Pose[2] = -4;
		row.HalfExtent = {8, 8, .005f};
		row.Tint = {1, 0, 0};
		row.SeamPlane = {0, 0, -1, 3.9f};
		geometry.Rows.push_back(row);
		std::string error;
		REQUIRE(EncodePortalGeometry(geometry, request.Geometry, error));
	}
	if (shortSurfaceBudget) {
		request.PixelBudget = 3 * captureExtent * captureExtent;
	}
	auto binding = Binding();
	scene::SurfaceLens lens;
	lens.Left = -.1f;
	lens.Right = .1f;
	lens.Bottom = -.1f;
	lens.Top = .1f;
	lens.NearPlane = .1f;
	lens.FarPlane = 100;
	lens.ClipNormal = {0, 0, -1};
	lens.ClipDistance = 1;
	core::CFrame through;
	through.Position = {0, 0, -4};
	binding.Sampling = scene::SurfaceProjection(lens, core::CFrame{}) * through.ToMatrix();
	if (authoredBody) {
		worlds.Universe.Enter(worlds.Source, [&](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc aperture;
			aperture.Size = {8, 8, .01f};
			const auto entrance = scene::MakePart(store, aperture);
			REQUIRE(store.SetParent(entrance, workspace));
			aperture.Frame =
				core::CFrame(core::Vector3{0, 0, -4}) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			const auto exit = scene::MakePart(store, aperture);
			const auto camera = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Door");
			REQUIRE(store.SetParent(camera, entrance));
			auto link = *store.Get<scene::Portal>(camera);
			link.Destination = exit;
			link.DestinationWorld = core::Name("portal-destination");
			store.Set(camera, link);
			scene::PartDesc body;
			body.Size = {2, 2, .2f};
			const auto entity = scene::MakePart(store, body);
			scene::DrawInstance row;
			row.Source = entity.Id;
			row.Mesh = core::Name("runtime-body");
			row.Texture = core::Name("runtime-white");
			row.HalfExtent = {1, 1, .1f};
			row.Tint = {1, 0, 0};
			if (reciprocalMouth) {
				row.Frame.Position.Z = authoredEye.Z > -.005f ? .02f : -.02f;
			}
			View viewer;
			viewer.World = worlds.Source.Index;
			viewer.WorldName = core::Name("portal-source");
			viewer.CameraFrame = core::CFrame::LookAt(authoredEye, {0, 0, -.005f});
			viewer.Instances = std::span(&row, 1);
			std::vector<scene::SurfaceSlot> slots;
			scene::GatherSurfaceSlots(store, slots);
			std::vector<PortalImageDemand> demands;
			std::vector<PortalView> portals;
			PortalImageDemandSettings settings{
				.Width = 32, .Height = 32, .RecursionDepth = 0, .PixelBudget = 1024
			};
			REQUIRE(CollectPortalImageDemands(store, viewer, settings, demands, portals, slots).Ready == 1);
			REQUIRE(demands.size() == 1);
			request = demands[0].Request;
			binding = demands[0].Binding;
			request.Key.PortalKey = "Door";
			binding.Portal = core::Name("Door");
			REQUIRE_FALSE(request.Geometry.empty());
			CHECK(row.SeamNormal == core::Vector3{});
			CHECK(store.Resource<scene::ActiveCamera>() == nullptr);
		});
	}
	if (reciprocalMouth) {
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			scene::PartDesc aperture;
			aperture.Size = {8, 8, .01f};
			aperture.Mesh = core::Name("runtime-aperture");
			aperture.Frame =
				core::CFrame(core::Vector3{0, 0, -4}) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			const auto entrance = scene::MakePart(store, aperture);
			aperture.Frame = {};
			const auto exit = scene::MakePart(store, aperture);
			auto hidden = *store.Get<scene::Visual>(exit);
			hidden.Transparency = 1;
			store.Set(exit, hidden);
			const auto camera = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Return");
			REQUIRE(store.SetParent(camera, entrance));
			auto link = *store.Get<scene::Portal>(camera);
			link.Destination = exit;
			link.DestinationWorld = core::Name("portal-source");
			store.Set(camera, link);
		});
	}
	std::unique_ptr<PortalImageProducer> childProducer;
	ecs::Entity childColourEntity, nestedEntrance, nestedExit;
	if (nestedWorld) {
		request.RecursionDepth = 1;
		request.PixelBudget = (shortSurfaceBudget ? 1 : 2) * captureExtent * captureExtent;
		worlds.Universe.Enter(worlds.Source, [&](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc part;
			part.Frame.Position = productEye ? core::Vector3{0, 3, -8} : core::Vector3{0, 0, -4};
			part.Size = {16, 16, .01f};
			part.Mesh = core::Name("runtime-plane");
			const auto wall = scene::MakePart(store, part);
			childColourEntity = wall;
			REQUIRE(store.SetParent(wall, workspace));
			auto visual = *store.Get<scene::Visual>(wall);
			visual.Tint = {0, 0, 1};
			if (productEye) visual.Transparency = 1;
			store.Set(wall, visual);
			auto appearance = *store.Get<scene::SurfaceAppearance>(wall);
			appearance.ColourMap = core::Name("runtime-white");
			store.Set(wall, appearance);
			if (productEye) {
				part.Frame = core::CFrame(core::Vector3{0, -1, 0});
				part.Size = {100, 2, 100};
				part.Mesh = {};
				const auto floor = scene::MakePart(store, part);
				childColourEntity = floor;
				REQUIRE(store.SetParent(floor, workspace));
				auto floorVisual = *store.Get<scene::Visual>(floor);
				floorVisual.Tint = {0, 0, 1};
				store.Set(floor, floorVisual);
				if (!floorDefaultMaterial) store.Set(floor, appearance);
				part.Frame = core::CFrame(core::Vector3{0, 3, -3});
				part.Size = {10, 10, .4f};
				const auto entrance = scene::MakePart(store, part);
				REQUIRE(store.SetParent(entrance, workspace));
				part.Frame = core::CFrame(core::Vector3{0, 3, -3.4f}) *
							 core::CFrame::Angles(0, 3.14159265358979323846f, 0);
				const auto exit = scene::MakePart(store, part);
				REQUIRE(store.SetParent(exit, workspace));
				auto hidden = *store.Get<scene::Visual>(exit);
				hidden.Transparency = 1;
				store.Set(exit, hidden);
				const auto camera = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Return");
				REQUIRE(store.SetParent(camera, entrance));
				auto portal = *store.Get<scene::Portal>(camera);
				portal.Destination = exit;
				portal.DestinationWorld = core::Name("portal-destination");
				store.Set(camera, portal);
			}
			store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
		});
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			if (productEye) {
				auto hidden = *store.Get<scene::Visual>(wallEntity);
				hidden.Transparency = 1;
				store.Set(wallEntity, hidden);
			}
			scene::PartDesc part;
			part.Frame.Position = productEye ? core::Vector3{0, 3, -3} : core::Vector3{0, 0, -2};
			part.Size = productEye ? core::Vector3{10, 10, .4f} : core::Vector3{8, 8, .01f};
			part.Mesh = productEye ? core::Name{} : core::Name("runtime-aperture");
			const auto entrance = scene::MakePart(store, part);
			nestedEntrance = entrance;
			REQUIRE(store.SetParent(entrance, workspace));
			part.Frame = core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			if (productEye) part.Frame.Position = {0, 3, -3.4f};
			const auto exit = scene::MakePart(store, part);
			nestedExit = exit;
			auto hidden = *store.Get<scene::Visual>(exit);
			hidden.Transparency = 1;
			store.Set(exit, hidden);
			const auto camera = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Nested");
			REQUIRE(store.SetParent(camera, entrance));
			auto portal = *store.Get<scene::Portal>(camera);
			portal.Destination = exit;
			portal.DestinationWorld = core::Name("portal-source");
			store.Set(camera, portal);
			if (foregroundFloor) {
				part.Frame = core::CFrame(core::Vector3{0, productEye ? 0.0f : -1.0f, 0}) *
							 core::CFrame::Angles(-1.57079632679f, 0, 0);
				part.Size = {100, 100, .01f};
				part.Mesh = core::Name("runtime-plane");
				if (productEye) {
					part.Frame = core::CFrame(core::Vector3{0, -1, 0});
					part.Size = {100, 2, 100};
					part.Mesh = {};
				}
				const auto floor = scene::MakePart(store, part);
				REQUIRE(store.SetParent(floor, workspace));
				auto floorVisual = *store.Get<scene::Visual>(floor);
				floorVisual.Tint = {1, 0, 0};
				store.Set(floor, floorVisual);
				auto appearance = *store.Get<scene::SurfaceAppearance>(floor);
				if (!floorDefaultMaterial) appearance.ColourMap = core::Name("runtime-white");
				store.Set(floor, appearance);
				if (floorDefaultLighting) store.RemoveResource<scene::Sun>();
			}
		});
		const auto requests =
			worlds.Universe.OpenPresentation(worlds.Source, core::Name(PORTAL_REQUEST_CHANNEL));
		REQUIRE(requests.Status == world::PresentationStatus::Ok);
		childProducer = std::make_unique<PortalImageProducer>(
			worlds.Universe,
			destinationRenderer,
			worlds.Source,
			requests.Address,
			resident ? &residentImages : nullptr
		);
	}
	if (productEye) {
		request.Position = {-1.22769022f, 4.00007629f, 2.79496264f};
		request.Orientation = {0, -.10828726f, 0, .99411976f};
		const float extent = .1f * std::tan(1.22f / 2);
		request.Frustum = {-extent, extent, -extent, extent, .1f, 500};
	}

	if (forwardBody) {
		request.EyePlayer = "91";
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			std::vector<scene::PortalSeam> seams;
			scene::GatherPortalSeams(store, seams);
			REQUIRE(seams.size() == 1);
			const auto &seam = seams.front();
			PortalGeometry geometry;
			PortalGeometryRow body;
			body.Player = "91";
			body.Assets[0] = "runtime-plane";
			body.Assets[1] = "runtime-white";
			const auto entryNormal =
				seam.Normal * ((authoredEye - seam.Centre).Dot(seam.Normal) < 0 ? -1.f : 1.f);
			const auto position = seam.Centre - entryNormal;
			body.Pose[0] = position.X;
			body.Pose[1] = position.Y;
			body.Pose[2] = position.Z;
			body.HalfExtent = {1, 1, .005f};
			body.Tint = {1, 0, 0};
			body.SeamPlane = {entryNormal.X, entryNormal.Y, entryNormal.Z, seam.Centre.Dot(entryNormal)};
			geometry.Rows.push_back(body);
			std::string error;
			REQUIRE(EncodePortalGeometry(geometry, request.Geometry, error));
		});
	}
	const auto started = std::chrono::steady_clock::now();
	const auto issue = source.Issue(worlds.Requests, request, binding, started);
	REQUIRE(issue.Status == PortalInboxStatus::Issued);
	CHECK_FALSE(source.Capture("Door"));
	const auto requestBytes = worlds.Universe.PresentationTrafficCounts().EnqueuedBytes;
	if (cameraMotion) {
		auto moving = request;
		for (uint64_t step = 1; step <= 100; ++step) {
			moving.Key.CameraRevision = request.Key.CameraRevision + step;
			moving.Position[0] = request.Position[0] + static_cast<float>(step) * .001f;
			auto movingBinding = binding;
			movingBinding.Sampling[3][0] += static_cast<float>(step) * .001f;
			REQUIRE(
				source.Issue(worlds.Requests, moving, movingBinding, started).Status ==
				PortalInboxStatus::Busy
			);
		}
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
	}
	if (alreadyPresented) {
		worlds.Universe.Present(worlds.Destination, .125f, .375f);
	}
	const auto preparedViews = [] {
		const auto counter = core::Metrics::Get("render.portal_snapshot.prepared_views");
		return counter ? counter->Value : 0;
	};
	const double preparedBefore = preparedViews();
	const auto first = producer.Pump(0, 0, started, alreadyPresented);
	if (nestedWorld) CHECK(preparedViews() == preparedBefore);

	if (shortSurfaceBudget) {
		CHECK(first.Rendered == 0);
		CHECK(first.Sent == 1);
		const auto replies = source.Poll(started);
		REQUIRE(replies.size() == 1);
		CHECK(replies.front().Status == PortalImageStatus::BudgetExceeded);
		CHECK(source.Image("Door") == 0);
		CHECK(sourceRenderer.PortalImageUsage().PendingCpuBytes == 0);
		CHECK(destinationRenderer.ResourceTexture(core::Name("mirror-views"), 0) == nullptr);
		CHECK(destinationRenderer.ResourceTexture(core::Name("portal-image"), 0) == nullptr);
		return;
	}

	REQUIRE(first.Rendered == (nestedWorld ? 0 : 1));
	CHECK(first.Sent == (resident && !nestedWorld ? 1 : 0));
	if (nestedWorld) CHECK(source.Poll(started).empty());
	if (cancelNested) {
		const auto reply =
			worlds.Universe.LookupPresentation(worlds.Destination, "portal-image-replies/nested/0");
		REQUIRE(reply.Generation != 0);
		producer.Clear();
		CHECK(worlds.Universe.LookupPresentation(worlds.Destination, reply.Channel).Generation == 0);
		CHECK(destinationRenderer.PortalImageUsage().Images == 0);
		CHECK(producer.Pump(0, 0, started).Rendered == 0);
		return;
	}
	if (replaceNested || timeoutNested || moveNested || appearNested) {
		if (appearNested) {
			worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
				scene::PartDesc part;
				part.Frame.Position = {-2, 0, -2};
				part.Size = {2, 2, .01f};
				part.Mesh = core::Name("runtime-aperture");
				const auto pane = scene::MakePart(store, part);
				REQUIRE(store.SetParent(pane, scene::InstallServices(store)));
				const auto camera =
					store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Appeared");
				REQUIRE(store.SetParent(camera, pane));
				auto portal = *store.Get<scene::Portal>(camera);
				portal.Destination = nestedExit;
				portal.DestinationWorld = core::Name("portal-source");
				store.Set(camera, portal);
			});
		}
		CAPTURE(resident, seamChange);
		if (moveNested) {
			worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
				const auto moved = seamChange == 2 ? nestedExit : nestedEntrance;
				auto placement = *store.Get<scene::Transform>(moved);
				if (seamChange == 1)
					placement.Frame = placement.Frame * core::CFrame::Angles(0, .1f, 0);
				else
					placement.Frame.Position.X += .5f;
				store.Set(moved, placement);
			});
		}
		auto interruptedAt = started + std::chrono::milliseconds(timeoutNested ? 1100 : 1);
		if (replaceNested) {
			const auto previous = worlds.Universe.LookupPresentation(worlds.Source, PORTAL_REQUEST_CHANNEL);
			REQUIRE(worlds.Universe.ClosePresentation(previous) == world::PresentationStatus::Ok);
			const auto replacement =
				worlds.Universe.OpenPresentation(worlds.Source, core::Name(PORTAL_REQUEST_CHANNEL));
			REQUIRE(replacement.Status == world::PresentationStatus::Ok);
			REQUIRE(replacement.Address.Generation != previous.Generation);
			childProducer = std::make_unique<PortalImageProducer>(
				worlds.Universe,
				destinationRenderer,
				worlds.Source,
				replacement.Address,
				resident ? &residentImages : nullptr
			);
		}
		const auto interrupted = producer.Pump(0, 0, interruptedAt);
		CHECK(interrupted.Rendered == 0);
		CHECK(interrupted.Sent == 1);
		const auto failed = source.Poll(interruptedAt);
		REQUIRE(failed.size() == 1);
		CHECK(failed.front().Status == PortalImageStatus::Unavailable);
		CHECK(source.Image("Door") == 0);
		CHECK(destinationRenderer.PortalImageUsage().Images == 0);
		CHECK(sourceRenderer.PortalImageUsage().PendingCpuBytes == 0);
		const auto retryAt = interruptedAt + std::chrono::milliseconds(1);
		if (appearNested) request.PixelBudget = 3 * captureExtent * captureExtent;
		REQUIRE(source.Issue(worlds.Requests, request, binding, retryAt).Status == PortalInboxStatus::Issued);
		const auto wallStart = std::chrono::steady_clock::now();
		std::vector<PortalRuntimeCompletion> recovered;
		while (recovered.empty() && std::chrono::steady_clock::now() < wallStart + std::chrono::seconds(1)) {
			const auto now = retryAt + (std::chrono::steady_clock::now() - wallStart);
			childProducer->Pump(0, 0, now);
			producer.Pump(0, 0, now);
			recovered = source.Poll(now);
			if (recovered.empty()) SDL_Delay(1);
		}
		REQUIRE(recovered.size() == 1);
		INFO(recovered.front().Diagnostic);
		CHECK(recovered.front().Status == PortalImageStatus::Ok);
		CHECK(source.Image("Door") != 0);
		return;
	}

	if (pressure) {
		const std::array junk{std::byte{0}};
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Destination, worlds.Requests, worlds.Replies, 999, junk
			) == world::PresentationStatus::Ok
		);
		size_t refused = 0;
		while (refused == 0 && std::chrono::steady_clock::now() < started + std::chrono::milliseconds(500)) {
			const auto progress = producer.Pump(0, 0, std::chrono::steady_clock::now());
			CHECK(progress.Rendered == 0);
			CHECK(progress.Sent == 0);
			refused += progress.Refused;
			if (refused == 0) {
				SDL_Delay(1);
			}
		}
		REQUIRE(refused > 0);
		CHECK(source.Poll(std::chrono::steady_clock::now()).empty());
	}
	std::vector<PortalRuntimeCompletion> replies;
	const auto deadline = started + std::chrono::seconds(10);
	while (replies.empty() && std::chrono::steady_clock::now() < deadline) {
		const auto now = std::chrono::steady_clock::now();
		if (childProducer) childProducer->Pump(0, 0, now);
		producer.Pump(0, 0, now);
		replies = source.Poll(now);
		if (replies.empty()) {
			SDL_Delay(1);
		}
	}
	REQUIRE(replies.size() == 1);
	INFO(replies.front().Diagnostic);
	REQUIRE(replies.front().Status == PortalImageStatus::Ok);
	REQUIRE(source.Image("Door") != 0);
	const auto captured = source.Capture("Door");
	REQUIRE(captured.has_value());
	CHECK(captured->Image == source.Image("Door"));
	CHECK(captured->Producer == worlds.Requests);
	CHECK(captured->EyePlayer == request.EyePlayer);
	CHECK(captured->Camera.Position == request.Position);
	CHECK(captured->Camera.Orientation == request.Orientation);
	CHECK(captured->Camera.Frustum == request.Frustum);
	CHECK(captured->Camera.ClipPlane == request.ClipPlane);
	CHECK(captured->Binding.Sampling == binding.Sampling);
	CHECK(captured->Width == request.Width);
	CHECK(captured->Height == request.Height);
	REQUIRE(captured->CaptureLighting);
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		const auto lighting = scene::LightingOf(store);
		CHECK(
			captured->CaptureLighting->Direction ==
			std::array{lighting.Direction.X, lighting.Direction.Y, lighting.Direction.Z}
		);
		CHECK(
			captured->CaptureLighting->Ambient ==
			std::array{lighting.Ambient.R, lighting.Ambient.G, lighting.Ambient.B}
		);
	});
	CHECK(
		sourceRenderer.PortalImageUsage().PendingCpuBytes ==
		(resident ? 0 : captureExtent * captureExtent * 12)
	);
	if (resident) {
		// The nested case carries bounded lighting for both parent and child, still no pixel payload.
		CHECK(
			worlds.Universe.PresentationTrafficCounts().EnqueuedBytes <
			1024 + 2 * (69 + 44 * MAX_PORTAL_CAPTURE_LIGHTS)
		);
	} else {
		// Copied images may compress below their expanded CPU allocation.
		CHECK(worlds.Universe.PresentationTrafficCounts().EnqueuedBytes > requestBytes);
	}
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		if (alreadyPresented) {
			CHECK(store.Time().FrameDelta == .125f);
		}
		if (publishedReplica) {
			CHECK(store.Get<scene::Transform>(wallEntity)->Frame.Position.X == 100);
			const auto *published = store.Resource<DrawList>();
			REQUIRE(published != nullptr);
			REQUIRE(published->Instances.size() == 1);
			CHECK(published->Instances[0].Frame.Position.X == 0);
		}
		if (mirrorOnly) {
			CHECK(store.Resource<scene::ActiveCamera>() == nullptr);
		} else {
			CHECK(store.Resource<scene::ActiveCamera>()->Entity == activeCamera);
		}
		if (billboardOnly) {
			const auto *canvas = store.Get<gui::SpatialCanvas>(billboardEntity);
			REQUIRE(canvas != nullptr);
			// At z=-4, two studs cover 2 * 32 / (2 * 4) pixels with a 90-degree lens.
			CHECK(std::abs(canvas->Size.X - 8.0f) < .001f);
			CHECK(std::abs(canvas->Size.Y - 8.0f) < .001f);
		}
		CHECK(
			store.Get<scene::Transform>(activeCamera)->Frame.Position ==
			(beamOnly ? core::Vector3(0, 100, -3) : core::Vector3(100, 100, 100))
		);
	});
	scene::DrawInstance pane;
	pane.Source = 1;
	pane.Mesh = core::Name("runtime-aperture");
	pane.Texture = core::Name("runtime-white");
	pane.HalfExtent = {1.5f, 1.2f, .01f};
	pane.Tint = {0, 0, 1};
	pane.Surface = 0;
	pane.CastShadow = false;
	PortalView portal;
	portal.ExternalImage = true;
	portal.ImportedImage = source.Image("Door");
	portal.ImagePortal = core::Name("Door");
	portal.Normal = {0, 0, 1};
	portal.First = {1.5f, 0, 0};
	portal.Second = {0, 1.2f, 0};
	SceneTarget target{displayExtent, displayExtent};
	View view;
	view.World = worlds.Source.Index;
	view.WorldName = core::Name("portal-source");
	view.Pipeline = core::Name("runtime-source");
	view.Target = &target;
	view.Instances = std::span(&pane, 1);
	view.Portals = std::span(&portal, 1);
	view.CameraFrame.Position = {0, 0, 4};
	view.Camera.FieldOfViewRadians = 1.0471975512f;
	if (authoredBody) {
		pane.Frame.Position.Z = -.005f;
		portal.Centre = pane.Frame.Position;
		view.CameraFrame = core::CFrame::LookAt(authoredEye, portal.Centre);
	}
	CAPTURE(authoredEye.X, authoredEye.Y, authoredEye.Z, resident);

	view.OverrideLighting = true;
	view.Lighting.Ambient = {0, 0, 2};
	view.Lighting.Direct = {};
	const auto refresh = [&] {
		const auto now = std::chrono::steady_clock::now();
		REQUIRE(source.Issue(worlds.Requests, request, binding, now).Status == PortalInboxStatus::Issued);
		std::array<size_t, 4> counts{};
		std::vector<PortalRuntimeCompletion> completed;
		while (completed.empty() && std::chrono::steady_clock::now() < now + std::chrono::seconds(1)) {
			const auto time = std::chrono::steady_clock::now();
			const auto child = childProducer->Pump(0, 0, time);
			const auto parent = producer.Pump(0, 0, time);
			counts[0] += child.Rendered;
			counts[1] += child.Reused;
			counts[2] += parent.Rendered;
			counts[3] += parent.Reused;
			completed = source.Poll(time);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		INFO(completed.front().Diagnostic);
		REQUIRE(completed.front().Status == PortalImageStatus::Ok);
		return counts;
	};
	OverlayImage overlay;
	if (foregroundFloor) {
		graph::RenderGraph eyePipeline;
		REQUIRE(
			graph::Build(graph::DefaultEyeDocument(), eyePipeline, offender) ==
			graph::PipelineDocumentStatus::Ok
		);
		view.Pipeline = core::Name("runtime-eye");
		REQUIRE(sourceRenderer.SetPipeline(view.Pipeline, eyePipeline));
		for (int cycle = 0; cycle < 3; ++cycle) {
			CAPTURE(cycle, floorDefaultMaterial, floorDefaultLighting, productEye);
			if (cycle != 0) {
				worlds.Universe.Enter(worlds.Source, [&](ecs::Store &store) {
					auto visual = *store.Get<scene::Visual>(childColourEntity);
					visual.Tint = cycle == 1 ? core::Color3{0, 1, 0} : core::Color3{0, 0, 1};
					store.Set(childColourEntity, visual);
				});
				CHECK(refresh() == std::array<size_t, 4>{1, 0, 1, 0});
			}
			view.EyeImage = source.Image("Door");
			view.EyeImageKey = core::Name("Door");
			(void)sourceRenderer.Render(std::span(&view, 1), overlay, nullptr, false);
			const auto eye = render::test::CaptureResource(
				sourceRenderer,
				core::Name("composed-image"),
				0,
				displayExtent,
				displayExtent,
				render::test::ImageFormat::Rgba8Unorm
			);
			const auto format = sourceRenderer.Backend().ColourFormat;
			const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
							  format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
			const size_t floor = size_t(displayExtent - 1) * eye.RowStrideBytes + (displayExtent / 2) * 4;
			const auto red = std::to_integer<uint8_t>(eye.Bytes[floor + (bgra ? 2 : 0)]);
			const auto blue = std::to_integer<uint8_t>(eye.Bytes[floor + (bgra ? 0 : 2)]);
			const auto floorGreen = std::to_integer<uint8_t>(eye.Bytes[floor + 1]);
			const bool floorVisible =
				productEye ? std::max({red, floorGreen, blue}) > 32 : red > 32 && red > blue;
			if (!floorVisible)
				render::test::WriteImagePreview(
					core::Paths::Base() / "portal-eye-foreground.ppm", eye.View()
				);
			// A thick mouth may cover this ray, exposing the other room's blue floor.
			CHECK(floorVisible);
			const size_t centre =
				size_t(productEye ? displayExtent * 3 / 4 : displayExtent / 2) * eye.RowStrideBytes +
				(displayExtent / 2) * 4;
			const auto green = std::to_integer<uint8_t>(eye.Bytes[centre + 1]);
			const auto childBlue = std::to_integer<uint8_t>(eye.Bytes[centre + (bgra ? 0 : 2)]);
			CHECK((cycle == 1 ? green : childBlue) > 32);
			CHECK((cycle == 1 ? green > childBlue : childBlue > green));
		}
		return;
	}
	REQUIRE(sourceRenderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfaceInstances > 0);
	const auto image = render::test::CaptureResource(
		sourceRenderer, core::Name("portaled"), 0, 65, 65, render::test::ImageFormat::Rgba8Unorm
	);
	const auto at = size_t(32) * image.RowStrideBytes + 32 * 4;
	const auto display = [](double radiance) {
		const double aces = (radiance * (2.51 * radiance + .03)) / (radiance * (2.43 * radiance + .59) + .14);
		const double encoded = std::pow(aces, 1 / 2.2);
		return int(std::lround(
			(encoded <= .0031308 ? 12.92 * encoded : 1.055 * std::pow(encoded, 1 / 2.4) - .055) * 255
		));
	};
	if (nestedWorld)
		render::test::WriteImagePreview(
			core::Paths::Base() / (forwardBody ? "portal-child-forwarded.ppm" : "portal-child-native.ppm"),
			image.View()
		);
	CHECK(std::abs(int(std::to_integer<uint8_t>(image.Bytes[at])) - display(expectedRed)) <= 2);
	CHECK(std::to_integer<uint8_t>(image.Bytes[at + 1]) <= 2);
	CHECK(std::abs(int(std::to_integer<uint8_t>(image.Bytes[at + 2])) - display(expectedBlue)) <= 2);

	if (nestedWorld) {

		const auto unchanged = refresh();
		CHECK(unchanged == std::array<size_t, 4>{0, 1, 0, 1});
		worlds.Universe.Enter(worlds.Source, [&](ecs::Store &store) {
			auto visual = *store.Get<scene::Visual>(childColourEntity);
			visual.Tint = {0, 1, 0};
			store.Set(childColourEntity, visual);
		});
		const auto changed = refresh();
		CHECK(changed == std::array<size_t, 4>{1, 0, 1, 0});
		portal.ImportedImage = source.Image("Door");
		REQUIRE(sourceRenderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfaceInstances > 0);
		const auto updated = render::test::CaptureResource(
			sourceRenderer, core::Name("portaled"), 0, 65, 65, render::test::ImageFormat::Rgba8Unorm
		);
		CHECK(
			std::abs(int(std::to_integer<uint8_t>(updated.Bytes[at])) - display(forwardBody ? .5 : 0)) <= 2
		);
		CHECK(
			std::abs(int(std::to_integer<uint8_t>(updated.Bytes[at + 1])) - display(forwardBody ? 0 : .5)) <=
			2
		);
		CHECK(std::to_integer<uint8_t>(updated.Bytes[at + 2]) <= 2);
		producer.Clear();
		source.Clear();
		CHECK(sourceRenderer.PortalImageUsage().Images == 0);
		CHECK(destinationRenderer.PortalImageUsage().Images == 0);
		return;
	}
	CHECK(sourceRenderer.PortalImageUsage().Uploads == (resident ? 0 : 2));
	if (resident) {
		CHECK(sourceRenderer.PortalImageUsage().TextureBytes == captureExtent * captureExtent * 12);
		CHECK(sourceRenderer.PortalImageUsage().StagingBytes == 0);
		source.Clear();
		CHECK(sourceRenderer.PortalImageUsage().Images == 0);
		return;
	}
	if (mixedSurface) {
		for (const char *resource : {"portal-image", "mirror-views"}) {
			auto *device = static_cast<SDL_GPUDevice *>(destinationRenderer.Backend().Device);
			auto *texture =
				static_cast<SDL_GPUTexture *>(destinationRenderer.ResourceTexture(core::Name(resource), 0));
			REQUIRE(texture != nullptr);
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = 256;
			auto *transfer = gpu::CreateTransferBuffer(device, &info);
			REQUIRE(transfer != nullptr);
			auto *command = SDL_AcquireGPUCommandBuffer(device);
			REQUIRE(command != nullptr);
			auto *copy = SDL_BeginGPUCopyPass(command);
			REQUIRE(copy != nullptr);
			SDL_GPUTextureRegion region{};
			region.texture = texture;
			region.x = 16;
			region.y = 16;
			region.w = region.h = region.d = 1;
			SDL_GPUTextureTransferInfo output{};
			output.transfer_buffer = transfer;
			output.pixels_per_row = 32;
			output.rows_per_layer = 1;
			SDL_DownloadFromGPUTexture(copy, &region, &output);
			SDL_EndGPUCopyPass(copy);
			auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
			REQUIRE(fence != nullptr);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline) {
				SDL_Delay(1);
			}
			REQUIRE(SDL_QueryGPUFence(device, fence));
			const auto *mapped =
				static_cast<const uint16_t *>(SDL_MapGPUTransferBuffer(device, transfer, false));
			REQUIRE(mapped != nullptr);
			const std::array<uint16_t, 4> pixel{mapped[0], mapped[1], mapped[2], mapped[3]};
			SDL_UnmapGPUTransferBuffer(device, transfer);
			SDL_ReleaseGPUFence(device, fence);
			gpu::ReleaseTransferBuffer(device, transfer);
			INFO(
				"HDR resource=" << resource << " channels=" << pixel[0] << "," << pixel[1] << "," << pixel[2]
								<< "," << pixel[3]
			);
			CHECK(std::abs(int(pixel[0]) - (childGlass ? 0x3404 : beamOnly ? 0x3804 : 0x3800)) <= 1);
			CHECK(pixel[1] == 0);

			if (childGlass) {
				// First blend rounding (2^-12) is attenuated by less than 1/2,
				// then the second blend rounds by at most 2^-14. The copied
				// parent image adds no rounding allowance.
				const unsigned exponent = (pixel[2] >> 10) & 31;
				REQUIRE((pixel[2] & 0x8000) == 0);
				REQUIRE(exponent > 0);
				REQUIRE(exponent < 31);
				const double blue = std::ldexp(double(1024 + (pixel[2] & 1023)), int(exponent) - 25);
				CHECK(std::abs(blue - expectedBlue) <= 1.0 / 4096.0);
			} else {
				CHECK(pixel[2] == 0);
			}
		}
	}
	if (complete) {
		const auto direct = render::test::CaptureResource(
			destinationRenderer, core::Name("tonemapped"), 0, 32, 32, render::test::ImageFormat::Rgba8Unorm
		);
		const auto centre = 16 * direct.RowStrideBytes + 16 * 4;
		if (mixedSurface && beamOnly) {
			// Width4 at distance9 through the mirror's 8-wide aperture at distance3
			// covers this off-centre pixel. A strip facing the primary eye is too thin.
			const auto edge = 16 * direct.RowStrideBytes + 17 * 4;
			CHECK(std::abs(int(std::to_integer<uint8_t>(direct.Bytes[edge])) - display(expectedRed)) <= 2);
			CHECK(std::to_integer<uint8_t>(direct.Bytes[edge + 1]) <= 2);
			CHECK(std::to_integer<uint8_t>(direct.Bytes[edge + 2]) <= 2);
		}

		CHECK(std::abs(int(std::to_integer<uint8_t>(direct.Bytes[centre])) - display(expectedRed)) <= 2);
		for (size_t channel = 0; channel < 3; ++channel) {
			CHECK(
				std::abs(
					int(std::to_integer<uint8_t>(direct.Bytes[centre + channel])) -
					int(std::to_integer<uint8_t>(image.Bytes[at + channel]))
				) <= 2
			);
		}
	}
	if (renewal) {
		const auto originalHandle = source.Image("Door");
		const auto originalUploads = sourceRenderer.PortalImageUsage().Uploads;
		const auto renewedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		REQUIRE(
			source.Issue(worlds.Requests, request, binding, renewedAt).Status == PortalInboxStatus::Issued
		);
		const auto queued = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(queued.size() == 1);
		PortalImageRequest offered;
		std::string error;
		REQUIRE(DecodePortalImageRequest(queued[0].Payload, offered, error));
		REQUIRE(offered.KnownImage.has_value());
		PortalResidentReceipt forged{
			offered.Key,
			offered.Scope,
			0,
			offered.KnownImage->ContentRevision + 1,
			offered.KnownImage->LightingRevision,
			offered.Width,
			offered.Height,
			captured->CaptureLighting
		};
		std::vector<std::byte> forgedWire;
		REQUIRE(EncodePortalImageRenewal(forged, forgedWire, error));
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Destination, worlds.Requests, worlds.Replies, offered.Key.RequestId, forgedWire
			) == world::PresentationStatus::Ok
		);
		CHECK(source.Poll(renewedAt).empty());
		CHECK(source.Image("Door") == originalHandle);
		forged.ContentRevision = offered.KnownImage->ContentRevision;
		REQUIRE(forged.CaptureLighting);
		forged.CaptureLighting->Direct[0] += 1;
		REQUIRE(EncodePortalImageRenewal(forged, forgedWire, error));
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Destination, worlds.Requests, worlds.Replies, offered.Key.RequestId, forgedWire
			) == world::PresentationStatus::Ok
		);
		CHECK(source.Poll(renewedAt).empty());
		REQUIRE(source.Capture("Door"));
		CHECK(source.Capture("Door")->CaptureLighting == captured->CaptureLighting);
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Source, worlds.Replies, worlds.Requests, offered.Key.RequestId, queued[0].Payload
			) == world::PresentationStatus::Ok
		);
		const std::array queueBlocker{std::byte{0}};
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Destination, worlds.Requests, worlds.Replies, 999, queueBlocker
			) == world::PresentationStatus::Ok
		);
		const auto pressured = producer.Pump(0, 0, renewedAt);
		CHECK(pressured.Rendered == 0);
		CHECK(pressured.Reused == 1);
		CHECK(pressured.Sent == 0);
		CHECK(pressured.Refused == 1);
		CHECK(source.Poll(renewedAt).empty());
		CHECK(source.Image("Door") == originalHandle);
		const auto progress = producer.Pump(0, 0, renewedAt);
		CHECK(progress.Rendered == 0);
		CHECK(progress.Reused == 0);
		CHECK(progress.Sent == 1);
		const auto renewed = source.Poll(renewedAt);
		REQUIRE(renewed.size() == 1);
		CHECK(renewed[0].Status == PortalImageStatus::Ok);
		const auto renewedCapture = source.Capture("Door");
		REQUIRE(renewedCapture.has_value());
		CHECK(renewedCapture->Image == originalHandle);
		CHECK(renewedCapture->CaptureLighting == captured->CaptureLighting);
		CHECK(renewedCapture->Binding.Expected == offered.Key);
		CHECK(renewedCapture->Camera.Position == request.Position);
		CHECK(source.Image("Door") == originalHandle);
		CHECK(sourceRenderer.PortalImageUsage().Uploads == originalUploads);
		source.Poll(renewedAt + std::chrono::milliseconds(600));
		CHECK(source.Image("Door") == originalHandle);
		const auto delayedAt = renewedAt + std::chrono::milliseconds(600);
		REQUIRE(
			source.Issue(worlds.Requests, request, binding, delayedAt).Status == PortalInboxStatus::Issued
		);
		const auto delayed = producer.Pump(0, 0, delayedAt);
		CHECK(delayed.Rendered == 0);
		CHECK(delayed.Reused == 1);
		CHECK(delayed.Sent == 1);
		CHECK(source.Poll(renewedAt + std::chrono::milliseconds(1000)).empty());
		CHECK(source.Image("Door") == 0);
		const auto recoveredAt = renewedAt + std::chrono::milliseconds(1100);
		REQUIRE(
			source.Issue(worlds.Requests, request, binding, recoveredAt).Status == PortalInboxStatus::Issued
		);
		const auto recovery = producer.Pump(0, 0, recoveredAt);
		CHECK(recovery.Rendered == 1);
		CHECK(recovery.Reused == 0);
		const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		replies = source.Poll(recoveredAt);
		while (replies.empty() && std::chrono::steady_clock::now() < recoveryDeadline) {
			producer.Pump(0, 0, recoveredAt);
			replies = source.Poll(recoveredAt);
			if (replies.empty()) {
				SDL_Delay(1);
			}
		}
		REQUIRE(replies.size() == 1);
		REQUIRE(replies.front().Status == PortalImageStatus::Ok);
		REQUIRE(source.Image("Door") != 0);
		portal.ImportedImage = source.Image("Door");
		REQUIRE(sourceRenderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfaceInstances > 0);
		CHECK(sourceRenderer.PortalImageUsage().Uploads == originalUploads + (resident ? 0 : 2));

		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			auto moved = *store.Get<scene::Transform>(wallEntity);
			moved.Frame.Position.X = 100;
			store.Set(wallEntity, moved);
		});
		const auto changedAt = recoveredAt + std::chrono::milliseconds(100);
		REQUIRE(
			source.Issue(worlds.Requests, request, binding, changedAt).Status == PortalInboxStatus::Issued
		);
		const auto changed = producer.Pump(0, 1, changedAt);
		CHECK(changed.Rendered == 1);
		CHECK(changed.Reused == 0);
		const auto changedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		replies = source.Poll(changedAt);
		while (replies.empty() && std::chrono::steady_clock::now() < changedDeadline) {
			producer.Pump(0, 1, changedAt);
			replies = source.Poll(changedAt);
			if (replies.empty()) {
				SDL_Delay(1);
			}
		}
		REQUIRE(replies.size() == 1);
		REQUIRE(replies.front().Status == PortalImageStatus::Ok);
		portal.ImportedImage = source.Image("Door");
		REQUIRE(sourceRenderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfaceInstances > 0);
		const auto changedImage = render::test::CaptureResource(
			sourceRenderer, core::Name("portaled"), 0, 65, 65, render::test::ImageFormat::Rgba8Unorm
		);
		const auto changedPixel = size_t(32) * changedImage.RowStrideBytes + 32 * 4;
		CHECK(std::to_integer<uint8_t>(changedImage.Bytes[changedPixel]) <= 2);
		CHECK(sourceRenderer.PortalImageUsage().Uploads == originalUploads + (resident ? 0 : 4));
	}

	if (cadence) {
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			auto moved = *store.Get<scene::Transform>(wallEntity);
			moved.Frame.Position.X = 100;
			store.Set(wallEntity, moved);
		});
		const auto next = source.Issue(worlds.Requests, request, binding, std::chrono::steady_clock::now());
		REQUIRE(next.Status == PortalInboxStatus::Issued);
		CHECK(next.RequestId > issue.RequestId);
		REQUIRE(producer.Pump(0, 1, std::chrono::steady_clock::now()).Rendered == 1);
		replies.clear();
		while (replies.empty() && std::chrono::steady_clock::now() < deadline) {
			const auto now = std::chrono::steady_clock::now();
			producer.Pump(0, 1, now);
			replies = source.Poll(now);
			if (replies.empty()) {
				SDL_Delay(1);
			}
		}
		REQUIRE(replies.size() == 1);
		REQUIRE(replies.front().Status == PortalImageStatus::Ok);
		portal.ImportedImage = source.Image("Door");
		REQUIRE(sourceRenderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfaceInstances > 0);
		const auto moving = render::test::CaptureResource(
			sourceRenderer, core::Name("portaled"), 0, 65, 65, render::test::ImageFormat::Rgba8Unorm
		);
		const auto pixel = size_t(32) * moving.RowStrideBytes + 32 * 4;
		CHECK(std::to_integer<uint8_t>(moving.Bytes[pixel]) <= 2);
		CHECK(std::abs(int(std::to_integer<uint8_t>(moving.Bytes[pixel + 1])) - display(.5)) <= 2);
		CHECK(std::to_integer<uint8_t>(moving.Bytes[pixel + 2]) <= 2);
		CHECK(sourceRenderer.PortalImageUsage().Uploads == 4);
	}

	if (cameraMotion) {
		const auto previous = source.Capture("Door");
		REQUIRE(previous.has_value());
		worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) {
			store.SetResource(scene::Sun{{0, 1, 0}, {.2f, .3f, .4f}});
			const auto holder = store.CreateInstance(scene::PartClass(), "capture-lamp-holder");
			store.Set(holder, scene::Transform{core::CFrame(core::Vector3{1, 2, -3})});
			store.Remove<scene::Visual>(holder);
			for (const auto kind : {scene::LightKind::Point, scene::LightKind::Spot}) {
				const auto bulb =
					store.CreateInstance(ecs::Classes::Find(core::Name("PointLight")), "capture-lamp");
				store.SetParent(bulb, holder);
				scene::Light light;
				light.Colour = {.3f, .4f, .5f};
				light.Brightness = 2;
				light.Range = 12;
				light.Kind = kind;
				store.Set(bulb, light);
			}
		});
		auto moving = request;
		moving.Key.CameraRevision++;
		moving.Position[0] += .25f;
		moving.EyePlayer = "99";
		auto movingBinding = binding;
		movingBinding.Sampling[3][0] += .25f;
		const auto next =
			source.Issue(worlds.Requests, moving, movingBinding, std::chrono::steady_clock::now());
		REQUIRE(next.Status == PortalInboxStatus::Issued);
		const auto pending = source.Capture("Door");
		REQUIRE(pending.has_value());
		CHECK(pending->Image == previous->Image);
		CHECK(pending->Binding.Expected == previous->Binding.Expected);
		CHECK(pending->Binding.Sampling == previous->Binding.Sampling);
		CHECK(pending->Camera.Position == previous->Camera.Position);
		CHECK(pending->EyePlayer == previous->EyePlayer);
		CHECK(pending->CaptureLighting == previous->CaptureLighting);
		std::vector<PortalRuntimeCompletion> completed;
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (completed.empty() && std::chrono::steady_clock::now() < until) {
			const auto now = std::chrono::steady_clock::now();
			producer.Pump(0, 0, now);
			completed = source.Poll(now);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		REQUIRE(completed.front().Status == PortalImageStatus::Ok);
		const auto replaced = source.Capture("Door");
		REQUIRE(replaced.has_value());
		CHECK(replaced->Image == source.CurrentImage("Door"));
		CHECK(replaced->Binding.Expected.RequestId == next.RequestId);
		CHECK(replaced->Binding.Sampling == movingBinding.Sampling);
		CHECK(replaced->Camera.Position == moving.Position);
		CHECK(replaced->EyePlayer == moving.EyePlayer);
		REQUIRE(replaced->CaptureLighting);
		CHECK(replaced->CaptureLighting->Direction == std::array{0.f, 1.f, 0.f});
		CHECK(replaced->CaptureLighting->Ambient == std::array{.2f, .3f, .4f});
		CHECK(replaced->CaptureLighting->LightCount == 2);
		for (size_t lightIndex = 0; lightIndex < replaced->CaptureLighting->LightCount; ++lightIndex) {
			const auto &light = replaced->CaptureLighting->Lights[lightIndex];
			CHECK(light.Position == std::array{1.f, 2.f, -3.f});
			CHECK(light.Colour == std::array{.6f, .8f, 1.f});
			CHECK(light.Range == 12);
		}
		CHECK(previous->CaptureLighting == captured->CaptureLighting);
		CHECK(previous->Camera.Position == request.Position);
		worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) {
			store.Each<scene::Light>([](ecs::Entity, scene::Light &light) {
				light.Range = std::numeric_limits<float>::quiet_NaN();
			});
		});
		moving.Key.CameraRevision++;
		const auto invalidAt = std::chrono::steady_clock::now();
		REQUIRE(
			source.Issue(worlds.Requests, moving, movingBinding, invalidAt).Status ==
			PortalInboxStatus::Issued
		);
		const auto invalidLighting = producer.Pump(0, 0, invalidAt);
		CHECK(invalidLighting.Rendered == 0);
		CHECK(invalidLighting.Sent == 1);
		const auto refused = source.Poll(invalidAt);
		REQUIRE(refused.size() == 1);
		CHECK(refused[0].Status == PortalImageStatus::Failed);
		CHECK(refused[0].Diagnostic == "invalid destination capture lighting");
		const auto retained = source.Capture("Door");
		REQUIRE(retained);
		CHECK(retained->Image == replaced->Image);
		CHECK(retained->CaptureLighting == replaced->CaptureLighting);
	}
	if (retireProducer) {
		CAPTURE(resident, replaceProducer, retirePending);
		const auto oldImage = source.CurrentImage("Door");
		REQUIRE(oldImage != 0);
		if (retirePending) {
			request.Key.CameraRevision++;
			REQUIRE(
				source.Issue(worlds.Requests, request, binding, std::chrono::steady_clock::now()).Status ==
				PortalInboxStatus::Issued
			);
			REQUIRE(source.Image("Door") == oldImage);
		}
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
		world::PresentationAddress replacement;
		if (replaceProducer) {
			const auto opened =
				worlds.Universe.OpenPresentation(worlds.Destination, core::Name(PORTAL_REQUEST_CHANNEL));
			REQUIRE(opened.Status == world::PresentationStatus::Ok);
			replacement = opened.Address;
			REQUIRE(replacement != worlds.Requests);
		}
		source.Poll(std::chrono::steady_clock::now());
		CHECK(source.CurrentImage("Door") == 0);
		CHECK(source.Image("Door") == 0);
		CHECK(sourceRenderer.PortalImageUsage().Images == 0);
		if (replaceProducer) {
			PortalImageProducer restarted(
				worlds.Universe,
				destinationRenderer,
				worlds.Destination,
				replacement,
				resident ? &residentImages : nullptr
			);
			REQUIRE(
				source.Issue(replacement, request, binding, std::chrono::steady_clock::now()).Status ==
				PortalInboxStatus::Issued
			);
			std::vector<PortalRuntimeCompletion> restored;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (restored.empty() && std::chrono::steady_clock::now() < deadline) {
				const auto now = std::chrono::steady_clock::now();
				restarted.Pump(0, 0, now);
				restored = source.Poll(now);
				if (restored.empty()) SDL_Delay(1);
			}
			REQUIRE(restored.size() == 1);
			REQUIRE(restored.front().Status == PortalImageStatus::Ok);
			CHECK(source.CurrentImage("Door") != 0);
			CHECK(source.CurrentImage("Door") != oldImage);
			source.InvalidateEndpoint(replacement);
		}
	} else if (expiry) {
		const auto acceptedAt = std::chrono::steady_clock::now();
		request.Key.CameraRevision++;
		REQUIRE(
			source.Issue(worlds.Requests, request, binding, acceptedAt + std::chrono::milliseconds(900))
				.Status == PortalInboxStatus::Issued
		);
		source.Poll(acceptedAt + std::chrono::milliseconds(1100));
	} else {
		source.InvalidateEndpoint(worlds.Requests);
	}

	CHECK(source.Image("Door") == 0);
	CHECK(sourceRenderer.PortalImageUsage().Images == 0);
	CHECK_FALSE(source.Capture("Door"));
}

TEST_CASE("portal source refuses an image binding owned by another world index", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	auto binding = Binding();
	binding.World = worlds.Destination.Index;
	CHECK(source.Issue(worlds.Requests, Request(), binding, START).Status == PortalInboxStatus::Invalid);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
}

TEST_CASE("body motion follows the completed portal request", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	auto request = Request();
	PortalGeometry geometry;
	PortalGeometryRow body;
	body.Name = "Body";
	geometry.Rows.push_back(body);
	std::string error;
	REQUIRE(EncodePortalGeometry(geometry, request.Geometry, error));
	const auto first = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Busy);
	geometry.Rows[0].Pose[0] = 1;
	REQUIRE(EncodePortalGeometry(geometry, request.Geometry, error));
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Busy);
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	CHECK(producer.Pump(0, 0, START).Requests == 1);
	const auto replies = worlds.Universe.TakePresentation(worlds.Replies);
	REQUIRE(replies.size() == 1);
	PortalImageReply previous;
	REQUIRE(DecodePortalImageReply(replies[0].Payload, previous, error));
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination, worlds.Requests, worlds.Replies, first.RequestId, replies[0].Payload
		) == world::PresentationStatus::Ok
	);
	REQUIRE(source.Poll(START).size() == 1);
	const auto moved = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(moved.Status == PortalInboxStatus::Issued);
	CHECK(moved.RequestId != first.RequestId);
	const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(messages.size() == 1);
	PortalImageRequest current;
	REQUIRE(DecodePortalImageRequest(messages[0].Payload, current, error));
	CHECK(previous.Key.CameraRevision != current.Key.CameraRevision);
	CHECK(current.Geometry == request.Geometry);
}

TEST_CASE("portal content revision follows resident flipbook cells", "[render][gpu][portal-runtime][.]") {
	RuntimeWorlds worlds;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	assets::TextureData texture;
	texture.Width = texture.Height = 2;
	texture.Format = assets::TextureFormat::RGBA8;
	texture.FlipbookSide = 2;
	texture.FlipbookFrames = 2;
	texture.FlipbookFrameRate = 2;
	texture.Pixels = {
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255}
	};
	REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture));
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc part;
		part.Frame.Position = {0, 0, -4};
		part.Size = {8, 8, .1f};
		part.Simulated = false;
		const auto wall = scene::MakePart(store, part);
		REQUIRE(store.SetParent(wall, workspace));
		auto appearance = *store.Get<scene::SurfaceAppearance>(wall);
		appearance.ColourMap = core::Name("portal-flipbook");
		store.Set(wall, appearance);
	});
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	uint64_t sequence = 0;
	const auto capture = [&] {
		auto request = Request();
		request.Key.RequestId = ++sequence;
		std::vector<std::byte> wire;
		std::string error;
		REQUIRE(EncodePortalImageRequest(request, wire, error));
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Source, worlds.Replies, worlds.Requests, sequence, wire
			) == world::PresentationStatus::Ok
		);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			producer.Pump(0, 0, START);
			auto replies = worlds.Universe.TakePresentation(worlds.Replies);
			if (replies.empty()) {
				SDL_Delay(1);
				continue;
			}
			REQUIRE(replies.size() == 1);
			PortalImageReply reply;
			REQUIRE(DecodePortalImageReply(replies[0].Payload, reply, error));
			REQUIRE(reply.Status == PortalImageStatus::Ok);
			return reply;
		}
		FAIL("portal capture timed out");
		return PortalImageReply{};
	};
	worlds.Universe.Tick(.01f);
	(void)capture();
	const auto first = capture();
	worlds.Universe.Tick(.1f);
	const auto sameCell = capture();
	CHECK(sameCell.PixelHash == first.PixelHash);
	CHECK(sameCell.ContentRevision == first.ContentRevision);
	SECTION("same-name texture replacement") {
		for (size_t at = 0; at < texture.Pixels.size(); at += 4) {
			texture.Pixels[at] = std::byte{0};
			texture.Pixels[at + 1] = std::byte{0};
			texture.Pixels[at + 2] = std::byte{255};
		}
		const auto beforeReplacement = fixture.Render.ResourceRevision();
		REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture));
		CHECK(fixture.Render.ResourceRevision() > beforeReplacement);
		const auto replaced = capture();
		CHECK(replaced.PixelHash != sameCell.PixelHash);
		CHECK(replaced.ContentRevision != sameCell.ContentRevision);
		const auto beforeRefusal = fixture.Render.ResourceRevision();
		CHECK_FALSE(fixture.Render.DropTexture(core::Name("not-registered")));
		CHECK(fixture.Render.ResourceRevision() == beforeRefusal);
		REQUIRE(fixture.Render.DropTexture(core::Name("portal-flipbook")));
		CHECK(fixture.Render.ResourceRevision() > beforeRefusal);
		const auto missing = capture();
		CHECK(missing.ContentRevision != replaced.ContentRevision);
		REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture));
		const auto restored = capture();
		CHECK(restored.PixelHash == replaced.PixelHash);
		CHECK(restored.ContentRevision != missing.ContentRevision);
	}
	SECTION("flipbook advancement") {
		for (int frame = 0; frame < 36; ++frame)
			worlds.Universe.Tick(1.0f / 60.0f);
		const auto nextCell = capture();
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			CHECK(store.Time().Elapsed >= .5);
		});
		CHECK(nextCell.PixelHash != first.PixelHash);
		CHECK(nextCell.ContentRevision != first.ContentRevision);
	}
}

TEST_CASE(
	"copied whole-eye images preserve player body selection", "[render][gpu][portal-runtime][eye-body][.]"
) {
	const bool foreignBody = GENERATE(false, true);
	RuntimeWorlds worlds;
	std::vector<std::byte> geometry;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -16};
		wall.Size = {40, 40, .1f};
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		const auto missing = scene::MakePart(store, wall);
		store.GetMutable<scene::Visual>(missing)->Mesh = core::Name("unloaded-before-body");
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
	});
	worlds.Universe.Enter(foreignBody ? worlds.Source : worlds.Destination, [&](ecs::Store &store) {
		if (foreignBody) scene::InstallServices(store);
		const auto player = scene::AddPlayer(store, "viewer", false, 91);
		scene::CharacterDesc character;
		character.Frame.Position = {0, -2.5f, -10};
		character.TorsoColour = character.LegColour = character.SkinColour = {0, 0, 1};
		const auto model = scene::MakeCharacter(store, character);
		REQUIRE(scene::SetPlayerCharacter(store, player, model));
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		if (foreignBody) {
			RegisterPresentationComponents();
			store.SetResource(DrawList{});
			scene::SyncRendered(store);
			CollectInstances(store);
			const auto *draws = store.Resource<DrawList>();
			REQUIRE(draws != nullptr);
			std::string error;
			REQUIRE(EncodePortalDraws(store, draws->Instances, draws->JointFrames, geometry, error));
			store.DestroyInstance(model);
		}
	});
	render::test::FixtureDevice sourceDevice, destinationDevice;
	sourceDevice.Initialise();
	destinationDevice.Initialise();
	REQUIRE(sourceDevice.Render.Backend().Device != destinationDevice.Render.Backend().Device);
	PortalImageSource source(worlds.Universe, sourceDevice.Render, worlds.Source, worlds.Replies);
	PortalImageProducer producer(
		worlds.Universe, destinationDevice.Render, worlds.Destination, worlds.Requests
	);
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultEyeDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("copied.body-eye");
	REQUIRE(sourceDevice.Render.SetPipeline(pipelineName, pipeline));
	SceneTarget target{65, 65};
	View eye;
	eye.Target = &target;
	eye.World = worlds.Source.Index;
	eye.WorldName = core::Name("portal-source");
	eye.Pipeline = pipelineName;
	eye.EyeImageKey = core::Name("Door");
	const auto started = std::chrono::steady_clock::now();
	const auto now = [&] { return START + (std::chrono::steady_clock::now() - started); };
	for (const bool hidden : {false, true, false}) {
		CAPTURE(hidden);
		eye.EyePlayer = hidden ? std::optional<int64_t>(91) : std::nullopt;
		PortalImageDemand demand;
		auto &request = demand.Request;
		REQUIRE(
			BuildPortalEyeDemand(
				eye.EyeImageKey,
				eye,
				{.Width = 65, .Height = 65, .RecursionDepth = 0, .PixelBudget = 65 * 65},
				demand
			) == PortalDemandStatus::Ready
		);
		request.Geometry = geometry;
		REQUIRE(
			source.Issue(worlds.Requests, request, demand.Binding, now()).Status == PortalInboxStatus::Issued
		);
		CHECK(source.CurrentImage("Door") == 0);
		std::vector<PortalRuntimeCompletion> completed;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
			const auto observed = now();
			producer.Pump(0, 1, observed);
			completed = source.Poll(observed);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		INFO(completed[0].Diagnostic);
		REQUIRE(completed[0].Status == PortalImageStatus::Ok);
		eye.EyeImage = source.CurrentImage("Door");
		REQUIRE(eye.EyeImage != 0);
		const auto uploadedBefore = sourceDevice.Render.PortalImageUsage().UploadedBytes;
		OverlayImage overlay;
		sourceDevice.Render.Render(std::span(&eye, 1), overlay, nullptr, false);
		CHECK(sourceDevice.Render.PortalImageUsage().UploadedBytes > uploadedBefore);
		CHECK(sourceDevice.Render.PortalImageUsage().PendingCpuBytes == 0);
		const auto image = render::test::CaptureResource(
			sourceDevice.Render,
			core::Name("composed-image"),
			0,
			65,
			65,
			render::test::ImageFormat::Bgra8Unorm
		);
		render::test::WriteImagePreview(
			core::Paths::Base() / (std::string("eye-import-") + (foreignBody ? "foreign-" : "native-") +
								   (hidden ? "hidden" : "visible") + ".ppm"),
			image.View()
		);
		const auto *centre = image.Bytes.data() + 32 * image.RowStrideBytes + 32 * 4;
		const auto blue = std::to_integer<int>(centre[0]), red = std::to_integer<int>(centre[2]);
		CAPTURE(red, blue);
		CHECK((hidden ? red > blue : blue > red));
	}
}

TEST_CASE(
	"portal producer pairs camera depth with a zero background",
	"[render][gpu][portal-runtime][producer-depth][.]"
) {
	RuntimeWorlds worlds;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -4};
		wall.Size = {2, 2, .1f};
		wall.Simulated = false;
		REQUIRE(store.SetParent(scene::MakePart(store, wall), workspace));
	});
	worlds.Universe.Tick(.01f);
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	auto request = Request();
	request.Projection = GENERATE(PortalImageProjection::Seam, PortalImageProjection::Eye);
	if (request.Projection == PortalImageProjection::Eye) request.ClipPlane = {};
	request.Key.RequestId = 1;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 1, wire) ==
		world::PresentationStatus::Ok
	);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::vector<world::PresentationMessage> replies;
	while (replies.empty() && std::chrono::steady_clock::now() < deadline) {
		producer.Pump(0, 0, START);
		replies = worlds.Universe.TakePresentation(worlds.Replies);
		if (replies.empty()) SDL_Delay(1);
	}
	REQUIRE(replies.size() == 1);
	PortalImageReply reply;
	REQUIRE(DecodePortalImageReply(replies.front().Payload, reply, error));
	INFO(reply.Diagnostic);
	REQUIRE(reply.Status == PortalImageStatus::Ok);
	CHECK(reply.Key == request.Key);
	REQUIRE(reply.Depth.size() == size_t(request.Width) * request.Height * 4);
	core::ByteReader samples(reply.Depth);
	size_t background = 0, surfaces = 0;
	while (!samples.AtEnd()) {
		const float distance = samples.ReadFloat();
		if (distance == 0)
			++background;
		else {
			++surfaces;
			CHECK(std::abs(distance - 3.95f) < .02f);
		}
	}
	CHECK(background > 0);
	CHECK(surfaces > 0);
}

TEST_CASE(
	"seam captures exclude only the selected primary body",
	"[render][gpu][portal-runtime][portal-primary-selection][.]"
) {
	const bool copied = GENERATE(false, true);
	RuntimeWorlds worlds;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -16};
		wall.Size = {40, 40, .1f};
		wall.Simulated = false;
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
	});
	std::array<ecs::Entity, 2> models;
	std::vector<std::byte> geometry;
	const auto bodyWorld = copied ? worlds.Source : worlds.Destination;
	worlds.Universe.Enter(bodyWorld, [&](ecs::Store &store) {
		if (copied) scene::InstallServices(store);
		for (size_t index = 0; index < models.size(); ++index) {
			const auto player = scene::AddPlayer(store, "viewer", false, 91 + index);
			scene::CharacterDesc character;
			character.Frame.Position = {index == 0 ? 0.f : 5.f, -2.5f, -10};
			character.TorsoColour = character.LegColour = character.SkinColour =
				index == 0 ? core::Color3{0, 0, 1} : core::Color3{0, 1, 0};
			models[index] = scene::MakeCharacter(store, character);
			REQUIRE(scene::SetPlayerCharacter(store, player, models[index]));
		}
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		if (copied) {
			RegisterPresentationComponents();
			store.SetResource(DrawList{});
			scene::SyncRendered(store);
			CollectInstances(store);
			const auto *draws = store.Resource<DrawList>();
			REQUIRE(draws != nullptr);
			std::string error;
			REQUIRE(EncodePortalDraws(store, draws->Instances, draws->JointFrames, geometry, error));
		}
	});
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	uint64_t sequence = 0;
	for (const std::string selected : {"", "91", "92", ""}) {
		CAPTURE(copied, selected);
		auto request = Request();
		request.Key.RequestId = ++sequence;
		request.Key.SeamRevision = sequence;
		request.Width = request.Height = 65;
		request.PixelBudget = 65 * 65;
		request.EyePlayer = selected;
		request.Geometry = geometry;
		std::vector<std::byte> bytes;
		std::string error;
		REQUIRE(EncodePortalImageRequest(request, bytes, error));
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Source, worlds.Replies, worlds.Requests, sequence, bytes
			) == world::PresentationStatus::Ok
		);
		std::vector<world::PresentationMessage> replies;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (replies.empty() && std::chrono::steady_clock::now() < deadline) {
			producer.Pump(0, 0, START);
			replies = worlds.Universe.TakePresentation(worlds.Replies);
			if (replies.empty()) SDL_Delay(1);
		}
		REQUIRE(replies.size() == 1);
		PortalImageReply reply;
		REQUIRE(DecodePortalImageReply(replies[0].Payload, reply, error));
		INFO(reply.Diagnostic);
		REQUIRE(reply.Status == PortalImageStatus::Ok);
		size_t blue = 0, green = 0;
		core::ByteReader pixels(reply.Pixels);
		while (!pixels.AtEnd()) {
			const auto rg = glm::unpackHalf2x16(pixels.ReadUInt32());
			const auto ba = glm::unpackHalf2x16(pixels.ReadUInt32());
			blue += ba.x > rg.x + .02f && ba.x > rg.y + .02f;
			green += rg.y > rg.x + .02f && rg.y > ba.x + .02f;
		}
		CHECK((selected == "91" ? blue == 0 : blue > 0));
		CHECK((selected == "92" ? green == 0 : green > 0));
		worlds.Universe.Enter(bodyWorld, [&](ecs::Store &store) {
			for (const auto model : models)
				CHECK(store.Alive(model));
		});
	}
}

TEST_CASE(
	"opaque portal lighting is independent of later spatial lenses",
	"[render][gpu][portal-runtime][opaque-capture-profile][.]"
) {
	RuntimeWorlds worlds;
	scene::RegisterSceneClasses();
	ecs::Entity lens;
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -4};
		wall.Size = {8, 8, .1f};
		wall.Simulated = false;
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
		lens = store.CreateInstance(ecs::Classes::Find(core::Name("ShaderLens")), "Warp");
		REQUIRE(lens != ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(lens, workspace));
		store.GetMutable<scene::Transform>(lens)->Frame.Position = {0, 0, -2};
		auto *effect = store.GetMutable<scene::ShaderLens>(lens);
		effect->Shader = core::Name("gravitational-lens");
		effect->Radius = 2;
		effect->InnerRadius = .5f;
		effect->Strength = 2;
	});
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ShaderLibrary library;
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		REQUIRE(library.RefreshLenses(store) == 1);
		const auto *module = library.FindLens(core::Name("gravitational-lens"));
		REQUIRE(module != nullptr);
		REQUIRE(fixture.Render.AddLensShader(core::Name("gravitational-lens"), module->SpirV));
	});
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	uint64_t sequence = 0;
	for (const auto scope : {PortalImageScope::OpaqueLighting, PortalImageScope::CompleteWorld}) {
		CAPTURE(scope);
		std::optional<PortalImageReply> original;
		for (const bool enabled : {false, true, false}) {
			worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
				store.GetMutable<scene::ShaderLens>(lens)->Enabled = enabled;
			});
			worlds.Universe.Tick(.01f);
			auto request = Request();
			request.Key.RequestId = ++sequence;
			request.Scope = scope;
			request.Width = request.Height = 65;
			request.PixelBudget = 65 * 65;
			std::vector<std::byte> bytes;
			std::string error;
			REQUIRE(EncodePortalImageRequest(request, bytes, error));
			REQUIRE(
				worlds.Universe.SendPresentation(
					worlds.Source, worlds.Replies, worlds.Requests, sequence, bytes
				) == world::PresentationStatus::Ok
			);
			std::vector<world::PresentationMessage> replies;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (replies.empty() && std::chrono::steady_clock::now() < deadline) {
				producer.Pump(0, 0, START);
				replies = worlds.Universe.TakePresentation(worlds.Replies);
				if (replies.empty()) SDL_Delay(1);
			}
			REQUIRE(replies.size() == 1);
			PortalImageReply reply;
			REQUIRE(DecodePortalImageReply(replies[0].Payload, reply, error));
			INFO(reply.Diagnostic);
			REQUIRE(reply.Status == PortalImageStatus::Ok);
			if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
				std::vector<float> colour;
				colour.reserve(reply.Pixels.size() / 2);
				core::ByteReader pixels(reply.Pixels);
				while (!pixels.AtEnd()) {
					const auto pair = glm::unpackHalf2x16(pixels.ReadUInt32());
					colour.push_back(pair.x);
					colour.push_back(pair.y);
				}
				const auto directory = std::filesystem::path(output);
				std::filesystem::create_directories(directory);
				const std::string stem =
					std::string(scope == PortalImageScope::OpaqueLighting ? "opaque-" : "complete-") +
					(enabled ? "lens-on" : "lens-off");
				render::test::WriteImagePreview(
					directory / (stem + ".ppm"),
					{request.Width,
					 request.Height,
					 render::test::ImageFormat::Rgba32Float,
					 std::as_bytes(std::span(colour))},
					4
				);
			}
			if (!original)
				original = reply;
			else {
				CHECK(
					(reply.PixelHash == original->PixelHash) ==
					(!enabled || scope == PortalImageScope::OpaqueLighting)
				);
				CHECK(reply.Depth == original->Depth);
			}
		}
	}
}

TEST_CASE(
	"portal producer captures ordered layers and refuses visible overflow",
	"[render][gpu][portal-runtime][producer-layers][.]"
) {
	RuntimeWorlds worlds(1);
	scene::RegisterSceneClasses();
	RegisterPresentationComponents();
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		scene::InstallServices(store);
		store.SetResource(DrawList{});
	});
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const core::Name mesh("producer-layer-plane"), emission("producer-layer-emission");
	REQUIRE(fixture.Render.AddMesh(mesh, plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(fixture.Render.AddTexture(emission, white));
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	PortalImageInbox inbox;
	const auto endpoint = [](const world::PresentationAddress &address) {
		return PortalEndpointView{address.World, address.Channel, address.Session, address.Generation};
	};
	uint64_t sequence = 0;
	// Recovery follows overflow on the same producer, exercising capture retirement.
	for (const int panes : {0, 1, 2, 3, -3, 2, 0}) {
		CAPTURE(panes, sequence);
		worlds.Universe.Tick(.01f);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			auto &rows = store.ResourceMutable<DrawList>()->Instances;
			rows.clear();
			for (int index = 0; index <= std::abs(panes); ++index) {
				scene::DrawInstance row;
				row.Source = index + 1;
				row.Mesh = mesh;
				row.HalfExtent = {8, 8, 1};
				row.Frame.Position = {0, 0, index == 0 ? (panes < 0 ? -3.5f : -6.f) : -1.f - index};
				row.Transparency = index == 0 ? 0 : .5f;
				row.CastShadow = false;
				row.Tint = {};
				row.EmissiveMap = emission;
				row.EmissiveStrength = 16;
				row.EmissiveTint = index == 0	? core::Color3{0, 0, 1}
								   : index == 1 ? core::Color3{1, 0, 0}
												: core::Color3{0, 1, 0};
				rows.push_back(row);
			}
		});
		auto request = Request();
		request.Key.RequestId = ++sequence;
		request.Scope = PortalImageScope::OpaqueLighting;
		request.OrderedLayers = true;
		request.Projection = PortalImageProjection::Eye;
		request.ClipPlane = {};
		request.Width = request.Height = 17;
		request.PixelBudget = 4 * 17 * 17;
		std::vector<std::byte> wire;
		std::string error;
		const auto issued = inbox.Issue(endpoint(worlds.Replies), endpoint(worlds.Requests), request, START);
		REQUIRE(issued.Status == PortalInboxStatus::Issued);
		request = issued.Request;
		wire = issued.Wire;
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Source, worlds.Replies, worlds.Requests, sequence, wire
			) == world::PresentationStatus::Ok
		);
		if (sequence == 3) {
			REQUIRE(producer.Pump(0, 0, START, true).Rendered == 1);
			const std::array filler{std::byte{0}};
			REQUIRE(
				worlds.Universe.SendPresentation(
					worlds.Destination, worlds.Requests, worlds.Replies, 999, filler
				) == world::PresentationStatus::Ok
			);
			bool full = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!full && std::chrono::steady_clock::now() < deadline) {
				const auto progress = producer.Pump(0, 0, START, true);
				CHECK(progress.Rendered == 0);
				full = progress.Refused != 0;
				if (!full) SDL_Delay(1);
			}
			REQUIRE(full);
			for (int retry = 0; retry < 3; ++retry) {
				const auto progress = producer.Pump(0, 0, START, true);
				CHECK(progress.Rendered == 0);
				CHECK(progress.Sent == 0);
				CHECK(progress.Refused == 1);
			}
			const auto discarded = worlds.Universe.TakePresentation(worlds.Replies);
			REQUIRE(discarded.size() == 1);
			CHECK(discarded[0].Correlation == 999);
		}
		std::vector<world::PresentationMessage> replies;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (replies.empty() && std::chrono::steady_clock::now() < deadline) {
			producer.Pump(0, 0, START, true);
			replies = worlds.Universe.TakePresentation(worlds.Replies);
			if (replies.empty()) SDL_Delay(1);
		}
		REQUIRE(replies.size() == 1);
		CHECK(replies[0].From == worlds.Requests);
		CHECK(replies[0].To == worlds.Replies);
		CHECK(replies[0].Correlation == sequence);
		const auto accepted = inbox.AcceptAuthenticated(
			endpoint(replies[0].From),
			endpoint(replies[0].To),
			replies[0].Correlation,
			replies[0].Payload,
			START
		);
		if (panes == 3) {
			CHECK(accepted.Status == PortalInboxStatus::CompletedFailure);
			CHECK(inbox.Usage().HeldCount == 0);
			PortalImageReply failed;
			REQUIRE(DecodePortalImageReply(replies[0].Payload, failed, error));
			CHECK(failed.Key == request.Key);
			CHECK(failed.Status == PortalImageStatus::BudgetExceeded);
			CHECK(failed.Diagnostic == "destination transparency exceeds two ordered layers");
			CHECK(failed.Pixels.empty());
			CHECK(failed.Depth.empty());
			continue;
		}
		INFO(accepted.Error);
		REQUIRE(accepted.Status == PortalInboxStatus::Accepted);
		auto owned = inbox.TakeLayers(
			endpoint(worlds.Replies), endpoint(worlds.Requests), request.Key.PortalKey, START
		);
		REQUIRE(owned);
		CHECK(inbox.Usage().HeldBytes == 0);
		const auto &layers = *owned;
		REQUIRE(ValidPortalImageLayerSet(layers));
		CHECK(layers.Opaque.Key == request.Key);
		REQUIRE(layers.Transparent.size() == 2);
		for (size_t layer = 0; layer < 3; ++layer) {
			const auto &image = layer == 0 ? layers.Opaque : layers.Transparent[layer - 1];
			core::ByteReader pixels(image.Pixels), depths(image.Depth);
			const bool occupied = layer == 0 || int(layer) <= std::abs(panes);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto rg = glm::unpackHalf2x16(pixels.ReadUInt32());
				const auto ba = glm::unpackHalf2x16(pixels.ReadUInt32());
				const auto depth = depths.ReadFloat();
				CHECK(
					std::abs(
						depth - (occupied ? (layer == 0 ? (panes < 0 ? 3.5f : 6.f) : 1.f + layer) : 0.f)
					) < .003f
				);
				if (!occupied) {
					CHECK(rg == glm::vec2{});
					CHECK(ba == glm::vec2{});
				} else {
					CHECK(std::abs(ba.y - (layer == 0 ? 1.f : 128.f / 255)) < .001f);
					CHECK((layer == 0 ? ba.x : layer == 1 ? rg.x : rg.y) > 7.f);
				}
			}
		}
	}
}

TEST_CASE(
	"portal source publishes layer groups only after upload", "[render][gpu][portal-runtime][layer-source][.]"
) {
	RuntimeWorlds worlds;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalResidentImages resident(fixture.Render);
	PortalImageSource source(worlds.Universe, fixture.Render, worlds.Source, worlds.Replies, {}, &resident);
	const auto issue = [&](float position, bool ordered = true) {
		auto request = Request();
		request.Scope = PortalImageScope::OpaqueLighting;
		request.OrderedLayers = ordered;
		request.Projection = PortalImageProjection::Eye;
		request.ClipPlane = {};
		request.Position[0] = position;
		request.Key.CameraRevision = static_cast<uint64_t>(position) + 1;
		request.PixelBudget *= 4;
		return source.Issue(worlds.Requests, request, Binding(), START);
	};
	const auto deliver = [&](uint64_t id, uint64_t revision, bool ordered = true) {
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 1);
		PortalImageRequest request;
		std::string error;
		REQUIRE(DecodePortalImageRequest(messages[0].Payload, request, error));
		CHECK(request.Key.RequestId == id);
		CHECK(request.OrderedLayers == ordered);
		CHECK_FALSE(request.KnownImage);
		PortalImageLayerSet layers;
		auto &opaque = layers.Opaque;
		opaque.Key = request.Key;
		opaque.Scope = request.Scope;
		opaque.Status = PortalImageStatus::Ok;
		opaque.CaptureTick = revision;
		opaque.ContentRevision = revision;
		opaque.Width = opaque.Height = 8;
		opaque.RowStride = 64;
		opaque.Pixels.assign(8 * 8 * 8, std::byte{});
		opaque.Depth.assign(8 * 8 * 4, std::byte{});
		opaque.PixelHash = assets::Hasher::Of(opaque.Pixels);
		opaque.DepthHash = assets::Hasher::Of(opaque.Depth);
		layers.Transparent.assign(2, opaque);
		std::vector<std::byte> wire;
		const bool encoded = ordered ? EncodePortalImageLayerSet(layers, wire, error)
									 : EncodePortalImageReply(opaque, wire, error);
		REQUIRE(encoded);
		REQUIRE(
			worlds.Universe.SendPresentation(worlds.Destination, worlds.Requests, worlds.Replies, id, wire) ==
			world::PresentationStatus::Ok
		);
	};
	SceneTarget target{8, 8};
	View view;
	view.World = worlds.Source.Index;
	view.WorldName = core::Name(worlds.Replies.World);
	view.Target = &target;
	OverlayImage overlay;
	const auto upload = [&] {
		REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false)
					.Ran(core::Name("portal-capture")));
		CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	};
	const auto first = issue(0);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	deliver(first.RequestId, 1);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK(source.Image("Door") == 0);
	CHECK(source.CurrentImage("Door") == 0);
	CHECK_FALSE(source.Capture("Door"));
	CHECK(issue(1).Status == PortalInboxStatus::Busy);
	view.World = worlds.Destination.Index;
	view.WorldName = core::Name(worlds.Requests.World);
	view.Slot = 7;
	upload();
	view.World = worlds.Source.Index;
	view.WorldName = core::Name(worlds.Replies.World);
	view.Slot = 0;
	CHECK(source.Image("Door") == 0);
	const auto completed = source.Poll(START);
	REQUIRE(completed.size() == 1);
	CHECK(completed[0].RequestId == first.RequestId);
	CHECK(completed[0].Status == PortalImageStatus::Ok);
	CHECK_FALSE(source.HasPendingUploads());
	const auto original = source.Capture("Door");
	REQUIRE(original);
	CHECK(original->Camera.Position[0] == 0);
	CHECK(original->TransparentImages[0] != 0);
	CHECK(original->TransparentImages[1] != 0);
	CHECK(source.CurrentImage("Door") == original->Image);
	CHECK(fixture.Render.PortalImageUsage().Images == 3);
	const auto second = issue(3);
	REQUIRE(second.Status == PortalInboxStatus::Issued);
	deliver(second.RequestId, 2);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK(source.Image("Door") == original->Image);
	CHECK(source.CurrentImage("Door") == 0);
	CHECK(source.Capture("Door")->Camera.Position == original->Camera.Position);
	CHECK(fixture.Render.PortalImageUsage().Images == 6);
	upload();
	CHECK(source.Image("Door") == original->Image);
	const auto replaced = source.Poll(START);
	REQUIRE(replaced.size() == 1);
	CHECK(replaced[0].RequestId == second.RequestId);
	const auto current = source.Capture("Door");
	REQUIRE(current);
	CHECK(current->Image != original->Image);
	CHECK(current->Camera.Position[0] == 3);
	CHECK(fixture.Render.PortalImageUsage().Images == 3);
	CHECK_FALSE(fixture.Render.DropPortalImage(original->Image));
	for (const bool submit : {false, true}) {
		const auto cancelled = issue(5);
		REQUIRE(cancelled.Status == PortalInboxStatus::Issued);
		deliver(cancelled.RequestId, 3);
		CHECK(source.Poll(START).empty());
		CHECK(source.HasPendingUploads());
		if (submit) upload();
		source.RestartRequests();
		CHECK_FALSE(source.HasPendingUploads());
		CHECK(source.Poll(START).empty());
		CHECK(source.Capture("Door")->Image == current->Image);
		CHECK(source.Capture("Door")->Camera.Position == current->Camera.Position);
		CHECK(fixture.Render.PortalImageUsage().Images == 3);
		CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	}
	const auto flattened = issue(6, false);
	REQUIRE(flattened.Status == PortalInboxStatus::Issued);
	deliver(flattened.RequestId, 4, false);
	const auto flatCompletion = source.Poll(START);
	REQUIRE(flatCompletion.size() == 1);
	CHECK(flatCompletion[0].Status == PortalImageStatus::Ok);
	CHECK(source.Capture("Door")->TransparentImages == std::array<uint64_t, 2>{});
	CHECK(fixture.Render.PortalImageUsage().Images == 1);
	CHECK_FALSE(fixture.Render.DropPortalImage(current->Image));
	upload();
	const auto withdrawn = issue(7);
	REQUIRE(withdrawn.Status == PortalInboxStatus::Issued);
	deliver(withdrawn.RequestId, 4);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
	CHECK(source.Poll(START).empty());
	CHECK_FALSE(source.HasPendingUploads());
	CHECK_FALSE(source.Capture("Door"));
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	CHECK(fixture.Render.PortalImageUsage().CachedTextureBytes == 0);
}
