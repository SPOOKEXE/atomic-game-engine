#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/render/PortalShadowTransport.hpp>
#include <engine/scene/Accessories.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.render.portalshadowtransportintegration")
TEST_DEPENDS("engine.render.portalimagehost")

TEST_CASE(
	"host composes retained shadows through owned multi-hop presentation traffic",
	"[render][gpu][portal-shadow-transport][.]"
) {
	using namespace engine;
	using namespace engine::render;
	const int mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
	const bool nested = mode == 1 || mode == 4;
	const bool prepared = mode >= 9;
	const bool animatedAccessory = mode == 12;
	CAPTURE(mode);
	scene::RegisterSceneClasses();
	world::Universe worlds;
	const auto viewer = worlds.Create({.Name = core::Name("shadow-transport-viewer")});
	std::array<world::WorldId, 3> rooms;
	for (size_t index = 0; index < rooms.size(); ++index)
		rooms[index] = worlds.Create({.Name = core::Name("shadow-transport-room-" + std::to_string(index))});
	world::PresentationLimits limits;
	limits.MessagesPerEndpoint = 1;
	REQUIRE(worlds.ConfigurePresentation(913, limits));
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	assets::MeshData panel;
	panel.Vertices = {
		{{-.5f, -.5f, 0}, {0, 0, 1}, {0, 1}},
		{{.5f, -.5f, 0}, {0, 0, 1}, {1, 1}},
		{{.5f, .5f, 0}, {0, 0, 1}, {1, 0}},
		{{-.5f, .5f, 0}, {0, 0, 1}, {0, 0}}
	};
	panel.Indices = {0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0};
	panel.ComputeBounds();
	const core::Name mesh("shadow-transport-panel");
	REQUIRE(renderer.AddMesh(mesh, panel));
	for (size_t index = 0; index < rooms.size(); ++index) {
		REQUIRE(renderer.AddMesh(mesh, panel, worlds.NameOf(rooms[index])));
		worlds.Enter(rooms[index], [&](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc part;
			part.Mesh = mesh;
			part.Frame.Position = {0, 0, -6};
			part.Size = {16, 16, .01f};
			const auto wall = scene::MakePart(store, part);
			REQUIRE(store.SetParent(wall, workspace));
			auto visual = *store.Get<scene::Visual>(wall);
			visual.Tint = index == 0 ? core::Color3{1, .1f, .05f} : core::Color3{.05f, .1f, 1};
			store.Set(wall, visual);
			store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
			if (animatedAccessory && index == 0) {
				const auto player = scene::AddPlayer(store, "accessory-viewer");
				const auto character = scene::LoadCharacter(store, player);
				REQUIRE(character != ecs::NULL_ENTITY);
				const auto root = store.Get<scene::Character>(character)->Root;
				const auto head = store.FindFirstChild(character, "Head");
				REQUIRE(head != ecs::NULL_ENTITY);
				const auto hat = store.CreateInstance(scene::AccessoryClass(), "animated-hat");
				scene::PartDesc handlePart;
				handlePart.Mesh = mesh;
				handlePart.Size = {12, 12, .01f};
				const auto handle = scene::MakePart(store, handlePart);
				store.SetInstanceName(handle, "Handle");
				REQUIRE(store.SetParent(handle, hat));
				store.GetMutable<scene::Visual>(handle)->Tint = {0, 1, 0};
				const auto handlePoint = store.CreateInstance(scene::AttachmentClass(), "HatAttachment");
				const auto headPoint = store.CreateInstance(scene::AttachmentClass(), "HatAttachment");
				REQUIRE(store.SetParent(handlePoint, handle));
				REQUIRE(store.SetParent(headPoint, head));
				store.Set(handlePoint, scene::Attachment{});
				store.Set(headPoint, scene::Attachment{});
				REQUIRE(scene::EquipAccessory(store, character, hat));
				REQUIRE(scene::PoseCharacters(store) > 0);
				const auto beforePose = store.Get<scene::Transform>(handle)->Frame.Position;
				store.Set(root, scene::Transform{core::CFrame({.45f, -5, -4})});
				REQUIRE(scene::PoseCharacters(store) > 0);
				CHECK((store.Get<scene::Transform>(handle)->Frame.Position - beforePose).Magnitude() > 4);
				const auto accessory = store.Get<scene::Accessory>(hat);
				REQUIRE(accessory != nullptr);
				const auto handleFrame = scene::ResolveAttachment(store, accessory->HandleAttachment);
				const auto characterFrame = scene::ResolveAttachment(store, accessory->CharacterAttachment);
				CHECK((handleFrame.Position - characterFrame.Position).Magnitude() < .0001f);
			}
			if (!nested || index + 1 == rooms.size()) return;
			part.Frame.Position = {0, 0, -2};
			part.Size = {4, 4, .01f};
			const auto entrance = scene::MakePart(store, part);
			REQUIRE(store.SetParent(entrance, workspace));
			part.Frame = core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			const auto exit = scene::MakePart(store, part);
			auto hidden = *store.Get<scene::Visual>(exit);
			hidden.Transparency = 1;
			store.Set(exit, hidden);
			const auto portal = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Deeper");
			REQUIRE(store.SetParent(portal, entrance));
			auto destination = *store.Get<scene::Portal>(portal);
			destination.Destination = exit;
			const size_t child = mode == 4 && index == 1 ? 0 : index + 1;
			destination.DestinationWorld = worlds.NameOf(rooms[child]);
			store.Set(portal, destination);
		});
	}
	graph::PipelineDocument eyeDocument;
	const auto eyeBase = graph::DefaultEyeDocument();
	for (const auto &edit : eyeBase.Edits()) {
		eyeDocument.Record(edit);
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("eye-image"))
			eyeDocument.Record(
				{.Kind = graph::EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"}
			);
	}
	graph::RenderGraph eyePipeline;
	core::Name offender;
	REQUIRE(graph::Build(eyeDocument, eyePipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name eyePipelineName("shadow-composed-eye");
	REQUIRE(renderer.SetPipeline(eyePipelineName, eyePipeline));
	PortalImageHost host(worlds, renderer);
	for (const auto room : rooms) {
		REQUIRE(host.SetRetainedBodyAuthorization(room, [&](const auto &requester, std::string_view player) {
			if (player != "91") return false;
			if (requester.World == worlds.NameOf(viewer).Text()) return true;
			for (const auto parent : rooms)
				if (requester.World == worlds.NameOf(parent).Text()) return true;
			return false;
		}));
		REQUIRE(host.Serve(room).Generation != 0);
	}
	const core::Name portal("Door");
	PortalImageDemand demand;
	demand.DestinationWorld = worlds.NameOf(rooms[0]);
	demand.Request.Key = {0, "Door", 1, 0};
	demand.Request.Scope = PortalImageScope::OpaqueLighting;
	demand.Request.OrderedLayers = true;
	demand.Request.RetainedBodyPlayer = "91";
	demand.Request.Projection = PortalImageProjection::Eye;
	demand.Request.ClipPlane = {};
	demand.Request.Frustum = {-.1f, .1f, -.1f, .1f, .1f, 100};
	demand.Request.Width = demand.Request.Height = 33;
	demand.Request.RecursionDepth = nested ? 2 : 0;
	demand.Request.PixelBudget = (nested ? 3 : 1) * 4 * 33 * 33;
	demand.Binding.World = viewer.Index;
	demand.Binding.WorldName = worlds.NameOf(viewer);
	demand.Binding.Portal = portal;
	const PortalImageDestination destination{worlds.NameOf(rooms[0]), rooms[0]};
	PortalImageHost::Time now{};
	REQUIRE(host.Submit(viewer, 0, std::span(&demand, 1), std::span(&destination, 1), now) == 1);
	const SceneTarget target{33, 33};
	View upload;
	upload.World = viewer.Index;
	upload.WorldName = worlds.NameOf(viewer);
	upload.Target = &target;
	OverlayImage overlay;
	const auto captureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!host.Capture(0, portal) && std::chrono::steady_clock::now() < captureDeadline) {
		host.Pump(0, 1, now);
		if (host.HasPendingUploads()) renderer.Render(std::span(&upload, 1), overlay, nullptr, false);
		SDL_Delay(1);
	}
	const auto capture = host.Capture(0, portal);
	REQUIRE(capture);
	REQUIRE(capture->Tree != 0);
	const auto *tree = renderer.FindPortalCaptureTree(capture->Tree);
	REQUIRE(tree);
	const size_t nodes = nested ? 3 : 1;
	REQUIRE(tree->Nodes.size() == nodes);
	const auto before = renderer.PortalImageUsage();
	const auto sentBefore = worlds.PresentationTrafficCounts().EnqueuedBytes;
	const auto refresh = [&] {
		demand.Request.Key.CameraRevision = 2;
		REQUIRE(host.Submit(viewer, 0, std::span(&demand, 1), std::span(&destination, 1), now) == 1);
		const auto messages = worlds.TakePresentation(host.Serve(rooms[0]));
		REQUIRE(messages.size() == 1);
		PortalImageRequest request;
		std::string error;
		REQUIRE(DecodePortalImageRequest(messages.front().Payload, request, error));
		CHECK(request.Key.CameraRevision != capture->Binding.Expected.CameraRevision);
	};
	if (mode == 7) refresh();
	uint64_t preparation = 0;
	std::array<scene::DrawInstance, 1> bodyRows;
	if (prepared) {
		bodyRows[0].Source = 1000;
		bodyRows[0].Mesh = mesh;
		bodyRows[0].Frame.Position = {0, 0, -4};
		bodyRows[0].HalfExtent = {.4f, .4f, .02f};
		bodyRows[0].CastShadow = true;
		auto reference = upload;
		reference.Instances = bodyRows;
		uint64_t ticket = 0, cancelledTicket = 0;
		REQUIRE(host.QueueBodyPreparation(portal, 0, now, ticket) == PortalTreeCompositionStatus::Pending);
		REQUIRE(
			host.QueueBodyPreparation(portal, 0, now, cancelledTicket) == PortalTreeCompositionStatus::Pending
		);
		CHECK(
			host.PollBodyPreparationTicket(cancelledTicket, now).Status ==
			PortalTreeCompositionStatus::Pending
		);
		host.CancelBodyPreparationTicket(cancelledTicket);
		CHECK(
			host.PollBodyPreparationTicket(cancelledTicket, now).Status ==
			PortalTreeCompositionStatus::Invalid
		);
		REQUIRE(host.PollBodyPreparationTicket(ticket, now).Status == PortalTreeCompositionStatus::Complete);
		REQUIRE(
			host.BeginQueuedBodyPreparation(ticket, reference, now, preparation) ==
			PortalTreeCompositionStatus::Pending
		);
		REQUIRE(preparation != 0);
		if (mode == 10) {
			now += std::chrono::seconds(11);
			host.Pump(0, 1, now);
			CHECK(host.PollBodyPreparation(preparation, now).Status == PortalTreeCompositionStatus::Invalid);
			return;
		}
		if (mode == 11) {
			REQUIRE(worlds.ClosePresentation(capture->Producer) == world::PresentationStatus::Ok);
			host.Pump(0, 1, now);
			CHECK(host.PollBodyPreparation(preparation, now).Status == PortalTreeCompositionStatus::Invalid);
			return;
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline) {
			host.Pump(0, 1, now);
			if (host.HasPendingUploads()) renderer.Render(std::span(&upload, 1), overlay, nullptr, false);
			const auto progress = host.PollBodyPreparation(preparation, now);
			if (progress.Status == PortalTreeCompositionStatus::Complete) break;
			REQUIRE(progress.Status == PortalTreeCompositionStatus::Pending);
			now += std::chrono::milliseconds(1);
			SDL_Delay(1);
		}
		REQUIRE(host.PollBodyPreparation(preparation, now).Status == PortalTreeCompositionStatus::Complete);
		if (mode == 12) refresh();
	}
	uint64_t job = 0;
	auto currentBody = upload;
	if (prepared) {
		bodyRows[0].Frame.Position.X = .2f;
		currentBody.Instances = bodyRows;
		REQUIRE(
			host.BeginPreparedBodyComposition(preparation, currentBody, now, job) ==
			PortalTreeCompositionStatus::Pending
		);
	} else
		REQUIRE(host.BeginBodyComposition(portal, upload, now, job) == PortalTreeCompositionStatus::Pending);
	REQUIRE(job != 0);
	if (!nested && !prepared) {
		const auto lost = worlds.TakePresentation(host.Serve(rooms[0]));
		REQUIRE(lost.size() == 1);
		PortalShadowPull pull;
		std::string error;
		REQUIRE(DecodePortalShadowPull(lost.front().Payload, pull, error));
		CHECK(pull.Part == PORTAL_SHADOW_MANIFEST_PART);
		if (mode == 6) {
			const auto &message = lost.front();
			const std::array junk{std::byte{0}};
			REQUIRE(
				worlds.SendPresentation(rooms[0], message.To, message.From, 999, junk) ==
				world::PresentationStatus::Ok
			);
			REQUIRE(
				worlds.SendPresentation(rooms[0], message.To, message.From, 1000, junk) ==
				world::PresentationStatus::Full
			);
			REQUIRE(
				worlds.SendPresentation(
					viewer, message.From, message.To, message.Correlation, message.Payload
				) == world::PresentationStatus::Ok
			);
		}
		now += std::chrono::milliseconds(101);
	}
	if (mode == 8) refresh();
	if (mode == 5) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (renderer.PortalImageUsage().PendingCpuBytes < PORTAL_SHADOW_BYTES &&
			   std::chrono::steady_clock::now() < deadline) {
			host.Pump(0, 1, now);
			REQUIRE(host.PollBodyComposition(job, now).Status == PortalTreeCompositionStatus::Pending);
			now += std::chrono::milliseconds(1);
			SDL_Delay(1);
		}
		REQUIRE(renderer.PortalImageUsage().PendingCpuBytes == PORTAL_SHADOW_BYTES);
		host.CancelBodyComposition(job);
		CHECK(host.PollBodyComposition(job, now).Status == PortalTreeCompositionStatus::Invalid);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		host.RemoveViewport(0);
		now += std::chrono::seconds(11);
		host.Pump(0, 1, now);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		return;
	}
	const auto finish = [&](uint64_t active) {
		PortalTreeCompositionProgress result;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline) {
			host.Pump(0, 1, now);
			if (host.HasPendingUploads()) renderer.Render(std::span(&upload, 1), overlay, nullptr, false);
			result = host.PollBodyComposition(active, now);
			if (result.Status == PortalTreeCompositionStatus::Complete ||
				result.Status == PortalTreeCompositionStatus::Invalid)
				break;
			CHECK(renderer.PortalImageUsage().PendingCpuBytes <= MAX_IMPORTED_PORTAL_CPU_BYTES);
			CHECK(renderer.PortalImageUsage().TextureBytes <= MAX_IMPORTED_PORTAL_TEXTURE_BYTES);
			now += std::chrono::milliseconds(mode == 2 ? 100 : 1);
			SDL_Delay(1);
		}
		return result;
	};
	auto result = finish(job);
	REQUIRE(result.Status == PortalTreeCompositionStatus::Complete);
	REQUIRE(result.Image != 0);
	size_t releasedDisplayImages = 0;
	if (prepared) {
		const auto preparedUsage = renderer.PortalImageUsage();
		const auto preparedTraffic = worlds.PresentationTrafficCounts().EnqueuedBytes;
		bodyRows[0].Frame.Position.X = -.2f;
		currentBody.Instances = bodyRows;
		REQUIRE(
			host.BeginPreparedBodyComposition(preparation, currentBody, now, job) ==
			PortalTreeCompositionStatus::Pending
		);
		result = finish(job);
		REQUIRE(result.Status == PortalTreeCompositionStatus::Complete);
		REQUIRE(result.Image != 0);
		CHECK(renderer.PortalImageUsage().Uploads == preparedUsage.Uploads);
		CHECK(renderer.PortalImageUsage().UploadedBytes == preparedUsage.UploadedBytes);
		CHECK(worlds.PresentationTrafficCounts().EnqueuedBytes == preparedTraffic);
		releasedDisplayImages = renderer.PortalImageUsage().Images;
		host.ReleaseBodyPreparation(preparation);
		uint64_t firstTicket = 0, secondTicket = 0, thirdTicket = 0;
		REQUIRE(
			host.QueueBodyPreparation(portal, 0, now, firstTicket) == PortalTreeCompositionStatus::Pending
		);
		REQUIRE(
			host.QueueBodyPreparation(portal, 0, now, secondTicket) == PortalTreeCompositionStatus::Pending
		);
		REQUIRE(
			host.QueueBodyPreparation(portal, 0, now, thirdTicket) == PortalTreeCompositionStatus::Pending
		);
		CHECK(
			host.PollBodyPreparationTicket(firstTicket, now).Status == PortalTreeCompositionStatus::Complete
		);
		CHECK(
			host.PollBodyPreparationTicket(secondTicket, now).Status == PortalTreeCompositionStatus::Pending
		);
		CHECK(
			host.PollBodyPreparationTicket(thirdTicket, now).Status == PortalTreeCompositionStatus::Pending
		);
		host.CancelBodyPreparationTicket(firstTicket);
		CHECK(
			host.PollBodyPreparationTicket(secondTicket, now).Status == PortalTreeCompositionStatus::Complete
		);
		host.CancelBodyPreparationTicket(secondTicket);
		CHECK(
			host.PollBodyPreparationTicket(thirdTicket, now).Status == PortalTreeCompositionStatus::Complete
		);
		host.CancelBodyPreparationTicket(thirdTicket);
	}
	upload.Pipeline = eyePipelineName;
	upload.EyeImageKey = portal;
	upload.EyeImage = result.Image;
	renderer.Render(std::span(&upload, 1), overlay, nullptr, false);
	const auto colour =
		test::CaptureResource(renderer, core::Name("tonemapped"), 0, 33, 33, test::ImageFormat::Rgba8Unorm);
	size_t litPixels = 0;
	for (size_t pixel = 0; pixel < 33 * 33; ++pixel)
		litPixels += std::to_integer<unsigned>(colour.Bytes[pixel * 4]) +
						 std::to_integer<unsigned>(colour.Bytes[pixel * 4 + 1]) +
						 std::to_integer<unsigned>(colour.Bytes[pixel * 4 + 2]) >
					 5;
	CHECK(litPixels > 0);
	if (animatedAccessory) {
		size_t greenPixels = 0;
		for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
			const auto offset = pixel * 4;
			const auto red = std::to_integer<uint8_t>(colour.Bytes[offset]);
			const auto green = std::to_integer<uint8_t>(colour.Bytes[offset + 1]);
			const auto blue = std::to_integer<uint8_t>(colour.Bytes[offset + 2]);
			greenPixels += green > red + 16 && green > blue + 16;
		}
		CHECK(greenPixels >= 16);
		CHECK(greenPixels <= 1000);
	}
	if (prepared) {
		if (mode == 12) {
			REQUIRE(worlds.ClosePresentation(capture->Producer) == world::PresentationStatus::Ok);
			host.Pump(0, 1, now);
		} else {
			now += std::chrono::seconds(11);
			host.Pump(0, 1, now);
		}
		CHECK(renderer.PortalImageUsage().Images < releasedDisplayImages);
	}
	if (prepared) {
		CHECK(renderer.PortalImageUsage().UploadedBytes > before.UploadedBytes);
		CHECK(worlds.PresentationTrafficCounts().EnqueuedBytes > sentBefore);
	} else {
		CHECK(
			renderer.PortalImageUsage().UploadedBytes == before.UploadedBytes + nodes * PORTAL_SHADOW_BYTES
		);
		CHECK(worlds.PresentationTrafficCounts().EnqueuedBytes >= sentBefore + nodes * PORTAL_SHADOW_BYTES);
	}
	CHECK(host.PollBodyComposition(job, now).Status != PortalTreeCompositionStatus::Complete);
	if (mode == 3) {
		const auto firstImage = result.Image;
		upload.EyeImage = 0;
		upload.EyeImageKey = {};
		now += std::chrono::milliseconds(1500);
		host.Pump(0, 1, now);
		REQUIRE(host.Capture(0, portal));
		REQUIRE(host.BeginBodyComposition(portal, upload, now, job) == PortalTreeCompositionStatus::Pending);
		result = finish(job);
		REQUIRE(result.Status == PortalTreeCompositionStatus::Complete);
		REQUIRE(result.Image != 0);
		if (result.Image != firstImage) CHECK_FALSE(renderer.DropPortalImage(firstImage));
		now = PortalImageHost::Time{} + std::chrono::seconds(10);
		host.Pump(0, 1, now);
		CHECK_FALSE(host.Capture(0, portal));
		CHECK_FALSE(renderer.DropPortalImage(result.Image));
	}
	host.RemoveViewport(0);
	CHECK_FALSE(renderer.DropPortalImage(result.Image));
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
}
