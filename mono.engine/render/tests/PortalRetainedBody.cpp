#include "RenderFixture.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalShadowImage.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <glm/packing.hpp>

TEST_SUITE_ID("engine.render.portalretainedbody")
TEST_DEPENDS("engine.render.portalimageruntime")

TEST_CASE(
	"source shadow retention bounds complete and pending ownership",
	"[render][gpu][portal-runtime][retained-body][.]"
) {
	using namespace engine;
	using namespace engine::render;
	scene::RegisterSceneClasses();
	world::Universe worlds;
	const auto viewer = worlds.Create({.Name = core::Name("shadow-budget-viewer")});
	const auto room = worlds.Create({.Name = core::Name("shadow-budget-room")});
	REQUIRE(worlds.ConfigurePresentation(792));
	const auto replies = worlds.OpenPresentation(viewer, core::Name(PORTAL_REPLY_CHANNEL)).Address;
	const auto requests = worlds.OpenPresentation(room, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	worlds.Enter(room, [](ecs::Store &store) { scene::InstallServices(store); });
	test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageProducer producer(worlds, fixture.Render, room, requests);
	producer.SetRetainedBodyAuthorization([&](const world::PresentationAddress &requester,
											  std::string_view player) {
		return requester == replies && player == "91";
	});
	PortalImageInbox::Time now{};
	PortalImageRequest request;
	request.Key.PortalKey = "Eye";
	request.Key.CameraRevision = 1;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.RetainedBodyPlayer = "91";
	request.Projection = PortalImageProjection::Eye;
	request.ClipPlane = {};
	request.Frustum = {-.1f, .1f, -.1f, .1f, .1f, 100};
	request.Width = request.Height = 9;
	request.PixelBudget = 4 * 9 * 9;
	std::array<PortalExchangeKey, 2> accepted;
	std::array<assets::ContentHash, 2> pixels;
	const auto submit = [&] {
		std::vector<std::byte> wire;
		std::string error;
		REQUIRE(EncodePortalImageRequest(request, wire, error));
		REQUIRE(
			worlds.SendPresentation(viewer, replies, requests, request.Key.RequestId, wire) ==
			world::PresentationStatus::Ok
		);
		return producer.Pump(0, 1, now);
	};
	for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
		request.Key.RequestId = sequence;
		const auto submitted = submit();
		const PortalCaptureTreeEndpoint endpoint{
			requests.World, requests.Channel, requests.Session, requests.Generation
		};
		CHECK_FALSE(producer.ResolveShadowRoute(replies, request.Key, endpoint, request.Key, now));
		CHECK(submitted.Rendered == (sequence <= 2 ? 1 : 0));
		std::vector<world::PresentationMessage> completed;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
			completed = worlds.TakePresentation(replies);
			if (!completed.empty()) break;
			CHECK(producer.Pump(0, 1, now).Rendered == 0);
			SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		std::string error;
		if (sequence <= 2) {
			PortalImageLayerSet layers;
			REQUIRE(DecodePortalImageLayerSet(completed[0].Payload, layers, error));
			REQUIRE(layers.Opaque.Status == PortalImageStatus::Ok);
			accepted[sequence - 1] = layers.Opaque.Key;
			pixels[sequence - 1] = layers.Opaque.PixelHash;
		} else {
			PortalImageReply refused;
			REQUIRE(DecodePortalImageReply(completed[0].Payload, refused, error));
			CHECK(refused.Status == PortalImageStatus::BudgetExceeded);
			CHECK(refused.Diagnostic.find("shadow retention") != std::string::npos);
		}
		const size_t held = std::min<size_t>(sequence, 2);
		CHECK(producer.ShadowUsage().Ready == held);
		CHECK(producer.ShadowUsage().ReservedBytes == held * PORTAL_SHADOW_BYTES);
		CHECK(producer.ShadowUsage().CpuBytes == held * PORTAL_SHADOW_BYTES);
	}
	CHECK(producer.ShadowUsage().ReservedBytes == MAX_PORTAL_PRODUCER_SHADOW_BYTES);
	worlds.Enter(room, [](ecs::Store &store) { store.SetResource(scene::Sun{{1, 0, 0}, {.1f, .1f, .1f}}); });
	const auto first = producer.TakeShadow(replies, accepted[0], now);
	REQUIRE(first);
	CHECK(first->Snapshot.SourceEmpty);
	CHECK(first->Snapshot.CaptureTick == 0);
	CHECK(first->Snapshot.EyePixelHash == pixels[0]);
	const auto route =
		producer.ResolveShadowRoute(replies, accepted[0], first->Snapshot.Producer, accepted[0], now);
	REQUIRE(route);
	CHECK(route->Requester == replies);
	CHECK(route->Producer == requests);
	CHECK(route->ParentEye == accepted[0]);
	CHECK(ValidPortalShadowImage(*first));
	CHECK(producer.ShadowUsage().ReservedBytes == PORTAL_SHADOW_BYTES);
	now += std::chrono::seconds(1);
	CHECK_FALSE(
		producer.ResolveShadowRoute(replies, accepted[0], first->Snapshot.Producer, accepted[0], now)
	);
	CHECK_FALSE(producer.TakeShadow(replies, accepted[1], now));
	CHECK(producer.ShadowUsage().CpuBytes == 0);
	CHECK(producer.ShadowUsage().ReservedBytes == 0);
	request.Key.RequestId = 4;
	REQUIRE(submit().Rendered == 1);
	CHECK(producer.ShadowUsage().ReservedBytes == PORTAL_SHADOW_BYTES);
	CHECK_FALSE(producer.TakeShadow(replies, request.Key, now));
	producer.Clear();
	CHECK(producer.ShadowUsage().Images == 0);
	CHECK(producer.ShadowUsage().CpuBytes == 0);
	CHECK(producer.ShadowUsage().ReservedBytes == 0);
	CHECK(producer.ShadowUsage().Routes == 0);
	CHECK_FALSE(producer.TakeShadow(replies, request.Key, now));
	CHECK(worlds.TakePresentation(replies).empty());
	for (uint64_t sequence = 5; sequence <= 9; ++sequence) {
		request.Key.RequestId = sequence;
		CHECK(submit().Rendered == (sequence < 9 ? 1 : 0));
		std::vector<world::PresentationMessage> completed;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
			completed = worlds.TakePresentation(replies);
			if (!completed.empty()) break;
			CHECK(producer.Pump(0, 1, now).Rendered == 0);
			SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		std::string error;
		if (sequence < 9) {
			PortalImageLayerSet layers;
			REQUIRE(DecodePortalImageLayerSet(completed[0].Payload, layers, error));
			REQUIRE(layers.Opaque.Status == PortalImageStatus::Ok);
			REQUIRE(producer.TakeShadow(replies, layers.Opaque.Key, now));
		} else {
			PortalImageReply refused;
			REQUIRE(DecodePortalImageReply(completed[0].Payload, refused, error));
			CHECK(refused.Status == PortalImageStatus::BudgetExceeded);
			CHECK(refused.Diagnostic.find("shadow route") != std::string::npos);
		}
		CHECK(producer.ShadowUsage().Images == 0);
		CHECK(producer.ShadowUsage().ReadyRoutes == std::min<size_t>(sequence - 4, 4));
		CHECK(producer.ShadowUsage().Routes <= MAX_PORTAL_PRODUCER_SHADOW_ROUTES);
	}
	producer.Clear();
	CHECK(producer.ShadowUsage().Routes == 0);
	CHECK(producer.ShadowUsage().RouteMetadataBytes == 0);
}

TEST_CASE(
	"authorized retained body capture removes colour and casters in the full native domain",
	"[render][gpu][portal-runtime][retained-body][.]"
) {
	using namespace engine;
	using namespace engine::render;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	world::Universe worlds;
	const auto viewer = worlds.Create({.Name = core::Name("retained-viewer")});
	const auto room = worlds.Create({.Name = core::Name("retained-room")});
	REQUIRE(worlds.ConfigurePresentation(791));
	const auto replies = worlds.OpenPresentation(viewer, core::Name(PORTAL_REPLY_CHANNEL)).Address;
	const auto requests = worlds.OpenPresentation(room, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	ecs::Entity retainedModel;
	worlds.Enter(room, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -16};
		wall.Size = {40, 40, .1f};
		wall.Simulated = false;
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		const auto canvas = store.CreateInstance(gui::GuiClass("SurfaceGui"), "Overlay");
		REQUIRE(store.SetParent(canvas, backdrop));
		gui::Surface surface;
		surface.On = gui::Face::Back;
		surface.AlwaysOnTop = true;
		surface.CanvasSize = {100, 100};
		store.Set(canvas, surface);
		const auto panel = store.CreateInstance(gui::GuiClass("Frame"), "Colour");
		REQUIRE(store.SetParent(panel, canvas));
		gui::Element element;
		element.Size = {1, 0, 1, 0};
		store.Set(panel, element);
		gui::Background background;
		background.Color = {.2f, .1f, .1f};
		background.Transparency = 0;
		background.BorderSizePixel = 0;
		store.Set(panel, background);
		for (const bool retained : {true, false}) {
			const auto player = scene::AddPlayer(store, retained ? "body" : "eye", false, retained ? 91 : 17);
			scene::CharacterDesc character;
			character.Frame.Position = retained ? core::Vector3{0, -2.5f, -6} : core::Vector3{5, -2.5f, -10};
			character.TorsoColour = character.LegColour = character.SkinColour =
				retained ? core::Color3{0, 0, 1} : core::Color3{0, 1, 0};
			const auto model = scene::MakeCharacter(store, character);
			REQUIRE(scene::SetPlayerCharacter(store, player, model));
			if (retained) retainedModel = model;
		}
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		// Light travels toward the backdrop, so the nearer body writes its own depth.
		store.SetResource(scene::Sun{{.4f, -1, -1}, {.5f, .5f, .5f}});
	});
	test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageProducer producer(worlds, fixture.Render, room, requests);
	const auto takeShadow = [&](uint64_t token) -> std::optional<ResourceImage> {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto image = fixture.Render.TakeResourceImage(token)) return image;
			SDL_Delay(1);
		}
		return {};
	};
	bool allowed = true;
	size_t policyChecks = 0;
	producer.SetRetainedBodyAuthorization([&](const world::PresentationAddress &requester,
											  std::string_view player) {
		++policyChecks;
		CHECK(requester == replies);
		CHECK(player == "91");
		return allowed && requester == replies && player == "91";
	});
	const PortalImageInbox::Time now{};
	std::vector<std::byte> eyeShadow, excludedShadow;
	std::vector<scene::DrawInstance> fullRows, roomRows;
	std::vector<core::CFrame> joints;
	std::optional<PortalCaptureLighting> capturedLighting;
	std::optional<PortalShadowImage> pairedShadow;
	PortalCaptureCamera capturedCamera;
	uint64_t sequence = 0;
	for (const int mode : {0, 1, 2, 3}) {
		CAPTURE(mode);
		PortalImageRequest request;
		request.Key = {.RequestId = ++sequence, .PortalKey = "Eye", .CameraRevision = sequence};
		request.Scope = PortalImageScope::OpaqueLighting;
		request.OrderedLayers = true;
		request.RecursionDepth = 1;
		request.Projection = PortalImageProjection::Eye;
		request.ClipPlane = {};
		request.Width = request.Height = 65;
		request.PixelBudget = 5 * 65 * 65;
		request.Frustum = {-.1f, .1f, -.1f, .1f, .1f, 100};
		request.EyePlayer = mode == 1 ? "91" : "17";
		if (mode >= 2) request.RetainedBodyPlayer = "91";
		std::vector<std::byte> wire;
		std::string error;
		const bool encoded = EncodePortalImageRequest(request, wire, error);
		INFO(error);
		REQUIRE(encoded);
		REQUIRE(
			worlds.SendPresentation(viewer, replies, requests, sequence, wire) ==
			world::PresentationStatus::Ok
		);
		// Graph resources may alias after their final read. Capture the shadow at
		// its node rather than reading the transient resource after submission.
		const uint64_t eyeShadowToken =
			mode == 1 ? fixture.Render.QueueResourceImage(
							core::Name("portal-image-layers"), core::Name("portal-shadow-export")
						)
					  : 0;
		if (mode == 1) REQUIRE(eyeShadowToken != 0);
		const auto submitted = producer.Pump(0, 1, now);
		REQUIRE(submitted.Rendered == 1);
		CHECK(submitted.Sent == 0);
		REQUIRE(worlds.TakePresentation(replies).empty());
		CHECK_FALSE(producer.TakeShadow(replies, request.Key, now));
		CHECK(producer.ShadowUsage().ReservedBytes == (mode >= 2 ? PORTAL_SHADOW_BYTES : 0));
		if (mode == 3) allowed = false;
		std::vector<world::PresentationMessage> completed;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
			CHECK(producer.Pump(0, 1, now).Rendered == 0);
			completed = worlds.TakePresentation(replies);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		CHECK(completed[0].From == requests);
		if (mode == 3) {
			PortalImageReply refused;
			REQUIRE(DecodePortalImageReply(completed[0].Payload, refused, error));
			CHECK(refused.Key == request.Key);
			CHECK(refused.Status == PortalImageStatus::Unavailable);
			CHECK(refused.Diagnostic.find("authorized") != std::string::npos);
			CHECK(refused.Pixels.empty());
			CHECK(producer.Pump(0, 1, now).Sent == 0);
			CHECK(worlds.TakePresentation(replies).empty());
			CHECK_FALSE(producer.TakeShadow(replies, request.Key, now));
			CHECK(producer.ShadowUsage().ReservedBytes == 0);
			continue;
		}
		PortalCaptureTree tree;
		REQUIRE(DecodePortalCaptureTree(completed[0].Payload, tree, error));
		REQUIRE(tree.Nodes.size() == 1);
		const auto &node = tree.Nodes.front();
		REQUIRE(node.Layers.SpatialOverlay);
		size_t overlayPixels = 0;
		core::ByteReader overlaySamples(node.Layers.SpatialOverlay->Pixels);
		while (!overlaySamples.AtEnd()) {
			overlaySamples.ReadUInt32();
			overlayPixels += glm::unpackHalf2x16(overlaySamples.ReadUInt32()).y > 0;
		}
		CHECK(overlayPixels > 0);
		CHECK(node.RetainedBodyPlayer == request.RetainedBodyPlayer);
		CHECK(node.Producer.World == requests.World);
		CHECK(node.Producer.Generation == requests.Generation);
		const auto &opaque = node.Layers.Opaque;
		REQUIRE(opaque.Status == PortalImageStatus::Ok);
		CHECK(opaque.Key == request.Key);
		size_t blue = 0, green = 0;
		core::ByteReader pixels(opaque.Pixels);
		while (!pixels.AtEnd()) {
			const auto rg = glm::unpackHalf2x16(pixels.ReadUInt32());
			const auto ba = glm::unpackHalf2x16(pixels.ReadUInt32());
			blue += ba.x > rg.x + .02f && ba.x > rg.y + .02f;
			green += rg.y > rg.x + .02f && rg.y > ba.x + .02f;
		}
		CHECK((mode == 0 ? blue > 0 : blue == 0));
		CHECK((mode == 1 ? green > 0 : green == 0));
		if (mode == 1) {
			auto shadow = takeShadow(eyeShadowToken);
			REQUIRE(shadow);
			REQUIRE(shadow->Status == ResourceImageStatus::Ok);
			REQUIRE(shadow->Kind == ResourceImageKind::DirectionalShadow);
			eyeShadow = std::move(shadow->Depth);
		}
		if (mode == 2) {
			CHECK(producer.ShadowUsage().Ready == 1);
			CHECK(producer.ShadowUsage().CpuBytes > PORTAL_SHADOW_BYTES);
			CHECK(producer.ShadowUsage().CpuBytes <= PORTAL_SHADOW_BYTES + MAX_PORTAL_EXCHANGE_BYTES);
			auto wrongEye = request.Key;
			++wrongEye.CameraRevision;
			CHECK_FALSE(producer.TakeShadow(replies, wrongEye, now));
			auto wrongRequester = replies;
			++wrongRequester.Generation;
			CHECK_FALSE(producer.TakeShadow(wrongRequester, request.Key, now));
			pairedShadow = producer.TakeShadow(replies, request.Key, now);
			REQUIRE(pairedShadow);
			CHECK_FALSE(producer.TakeShadow(replies, request.Key, now));
			CHECK(producer.ShadowUsage().ReservedBytes == 0);
			CHECK(producer.ShadowUsage().CpuBytes == 0);
			const auto &snapshot = pairedShadow->Snapshot;
			CHECK(ValidPortalShadowImage(*pairedShadow));
			CHECK(snapshot.Producer == node.Producer);
			CHECK(snapshot.Eye == opaque.Key);
			CHECK(snapshot.CaptureTick == opaque.CaptureTick);
			CHECK(snapshot.ContentRevision == opaque.ContentRevision);
			CHECK(snapshot.LightingRevision == opaque.LightingRevision);
			CHECK(snapshot.EyePixelHash == opaque.PixelHash);
			CHECK(snapshot.ExcludedPlayer == "91");
			CHECK_FALSE(snapshot.SourceEmpty);
			excludedShadow = pairedShadow->Depth;
			capturedLighting = opaque.CaptureLighting;
			capturedCamera = {request.Position, request.Orientation, request.Frustum, request.ClipPlane};
			worlds.Enter(room, [&](ecs::Store &store) {
				REQUIRE(store.Alive(retainedModel));
				const auto *draws = store.Resource<DrawList>();
				REQUIRE(draws != nullptr);
				fullRows = draws->Instances;
				joints = draws->JointFrames;
				for (const auto &row : fullRows)
					if (row.Rig == 0 || store.ParentOf(ecs::Entity{row.Rig}) != retainedModel)
						roomRows.push_back(row);
			});
		}
	}
	CHECK(policyChecks > 0);
	REQUIRE_FALSE(roomRows.empty());
	REQUIRE(roomRows.size() < fullRows.size());
	const auto fullBounds = graph::BoundsOfAll(fullRows);
	const auto roomBounds = graph::BoundsOfAll(roomRows);
	REQUIRE(fullBounds.Maximum != roomBounds.Maximum);
	const auto bounds = [](const core::AABB &box) {
		return std::array{
			box.Minimum.X, box.Minimum.Y, box.Minimum.Z, box.Maximum.X, box.Maximum.Y, box.Maximum.Z
		};
	};
	CHECK(pairedShadow->Snapshot.DomainBounds == bounds(fullBounds));
	CHECK(pairedShadow->Snapshot.SourceBounds == bounds(roomBounds));
	const auto baseDocument = graph::DefaultPbrDocument();
	graph::PipelineDocument referenceDocument;
	const core::Name shadowExport("retained-shadow-export");
	for (const auto &edit : baseDocument.Edits()) {
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("present")) {
			referenceDocument.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = shadowExport,
				 .NodeKind = core::Name("shadow-capture"),
				 .Scope = graph::NodeScope::View}
			);
			referenceDocument.Record(
				{.Kind = graph::EditKind::Reads, .Target = core::Name("shadow"), .Key = core::Name("shadow")}
			);
		}
		referenceDocument.Record(edit);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(referenceDocument, graph, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name referencePipeline("retained-body-native-reference");
	REQUIRE(fixture.Render.SetPipeline(referencePipeline, graph));
	SceneTarget target{65, 65};
	View view;
	view.Target = &target;
	view.World = room.Index;
	view.WorldName = worlds.NameOf(room);
	view.Pipeline = referencePipeline;
	view.JointFrames = joints;
	REQUIRE(ResolvePortalCaptureCamera(capturedCamera, PortalImageProjection::Eye, view));
	REQUIRE(capturedLighting.has_value());
	std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> lights;
	REQUIRE(ResolvePortalCaptureLighting(*capturedLighting, view, lights));
	OverlayImage overlay;
	view.Instances = fullRows;
	view.DirectionalShadowBounds = fullBounds;
	const uint64_t fullToken = fixture.Render.QueueResourceImage(referencePipeline, shadowExport);
	REQUIRE(fullToken != 0);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	auto fullReference = takeShadow(fullToken);
	REQUIRE(fullReference);
	REQUIRE(fullReference->Status == ResourceImageStatus::Ok);
	REQUIRE(fullReference->Kind == ResourceImageKind::DirectionalShadow);
	CHECK(std::ranges::equal(eyeShadow, fullReference->Depth));

	view.Instances = roomRows;
	for (const bool sharedDomain : {true, false}) {
		view.DirectionalShadowBounds = sharedDomain ? std::optional(fullBounds) : std::nullopt;
		const uint64_t token = fixture.Render.QueueResourceImage(referencePipeline, shadowExport);
		REQUIRE(token != 0);
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		auto reference = takeShadow(token);
		REQUIRE(reference);
		REQUIRE(reference->Status == ResourceImageStatus::Ok);
		REQUIRE(reference->Kind == ResourceImageKind::DirectionalShadow);
		CHECK(std::ranges::equal(reference->Depth, excludedShadow) == sharedDomain);
	}
}
