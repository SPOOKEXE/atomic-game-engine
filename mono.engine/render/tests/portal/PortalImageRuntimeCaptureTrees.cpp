// Payload capture trees, recursive tree publication, endpoint bindings and retained body authorization.
#include "PortalImageRuntimeFixtures.hpp"

TEST_SUITE_ID("engine.render.portalimageruntimetrees")
TEST_DEPENDS("engine.render.portalimageruntime")

namespace {
	PortalCaptureTreeNode
	PayloadNode(const PortalImageRequest &request, const world::PresentationAddress &endpoint, bool child) {
		PortalCaptureTreeNode node;
		node.Producer = {endpoint.World, endpoint.Channel, endpoint.Session, endpoint.Generation};
		node.Camera = {
			request.Position, request.Orientation, request.Frustum, request.ClipPlane, request.Projection
		};
		if (child) node.Camera.Position = {5, 6, 7};
		auto &opaque = node.Layers.Opaque;
		opaque.Key = request.Key;
		if (child) opaque.Key.PortalKey = "ChildDoor";
		opaque.Scope = request.Scope;
		opaque.Status = PortalImageStatus::Ok;
		opaque.CaptureTick = 21;
		opaque.ContentRevision = 22;
		opaque.LightingRevision = 23;
		opaque.Width = opaque.Height = child ? 2 : 8;
		opaque.RowStride = opaque.Width * 8;
		opaque.Pixels.resize(opaque.RowStride * opaque.Height);
		opaque.Depth.resize(opaque.Width * opaque.Height * 4);
		opaque.Pixels[0] = std::byte{0x00};
		opaque.Pixels[1] = std::byte{0x3c};
		opaque.CaptureLighting.emplace();
		opaque.CaptureLighting->Ambient = {.1f, .2f, .3f};
		opaque.PixelHash = assets::Hasher::Of(opaque.Pixels);
		opaque.DepthHash = assets::Hasher::Of(opaque.Depth);
		node.Layers.Transparent.assign(2, opaque);
		for (auto &layer : node.Layers.Transparent) {
			layer.Pixels.assign(layer.Pixels.size(), std::byte{});
			layer.PixelHash = assets::Hasher::Of(layer.Pixels);
		}
		return node;
	}

}

TEST_CASE(
	"payload portal sources retain authenticated capture trees without GPU imports",
	"[render][portal-runtime][portal-payload]"
) {
	const bool nested = GENERATE(false, true);
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(
		worlds.Universe,
		renderer,
		worlds.Source,
		worlds.Replies,
		{},
		nullptr,
		PortalImageSourceDelivery::CapturePayloads
	);
	CHECK(source.Issue(worlds.Requests, Request(), Binding(), START).Status == PortalInboxStatus::Invalid);
	auto request = Request();
	request.OrderedLayers = true;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.PixelBudget = 1024;
	request.RecursionDepth = nested ? 2 : 0;
	request.Position = {2, 3, 4};
	const auto issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	{
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 1);
		std::string decodeError;
		REQUIRE(DecodePortalImageRequest(messages.front().Payload, request, decodeError));
		CHECK(request.Key.RequestId == issued.RequestId);
	}
	const auto childWorld = worlds.Universe.Create({.Name = core::Name("payload-child")});
	const auto childEndpoint =
		worlds.Universe.OpenPresentation(childWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	PortalCaptureTree expected;
	expected.Nodes.push_back(PayloadNode(request, worlds.Requests, false));
	if (nested) {
		expected.Nodes.push_back(PayloadNode(request, childEndpoint, true));
		PortalCaptureTreeEdge edge;
		edge.PortalKey = "ChildDoor";
		edge.Position = {1, 2, 3};
		PortalGeometry geometry;
		geometry.Rows.emplace_back();
		std::string error;
		REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
		expected.Edges.push_back(std::move(edge));
	}
	std::vector<std::byte> wire;
	std::string error;
	if (nested)
		REQUIRE(EncodePortalCaptureTree(expected, wire, error));
	else
		REQUIRE(EncodePortalImageLayerSet(expected.Nodes.front().Layers, wire, error));
	const auto before = renderer.PortalImageUsage();
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination, worlds.Requests, worlds.Replies, issued.RequestId, wire
		) == world::PresentationStatus::Ok
	);
	const auto completions = source.Poll(START);
	REQUIRE(completions.size() == 1);
	CHECK(completions.front().RequestId == issued.RequestId);
	CHECK(completions.front().Status == PortalImageStatus::Ok);
	CHECK(completions.front().Image == 0);
	CHECK_FALSE(source.HasPendingUploads());
	CHECK(source.Image("Door") == 0);
	CHECK(renderer.PortalImageUsage().Images == before.Images);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == before.PendingCpuBytes);
	CHECK_FALSE(source.RenameIdleForUnrelatedRoute(worlds.Replies.World));
	CHECK_FALSE(source.RenameIdleForUnrelatedRoute(worlds.Requests.World));
	if (nested) CHECK_FALSE(source.RenameIdleForUnrelatedRoute("payload-child"));
	const auto unrelated = worlds.Universe.Create({.Name = core::Name("payload-unrelated")});
	REQUIRE(source.RenameIdleForUnrelatedRoute("payload-unrelated"));
	REQUIRE(worlds.Universe.Rename(unrelated, core::Name("payload-renamed")) == world::WorldStatus::Ok);
	SECTION("take exactly once") {
		auto captured = source.TakeTree("Door", START);
		REQUIRE(captured);
		CHECK(*captured == expected);
		CHECK_FALSE(source.TakeTree("Door", START));
	}
	SECTION("expiry") {
		CHECK_FALSE(source.TakeTree("Door", START + std::chrono::seconds(2)));
	}
	SECTION("root endpoint withdrawal") {
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
		CHECK_FALSE(source.TakeTree("Door", START));
	}
	SECTION("child endpoint withdrawal") {
		if (nested) {
			REQUIRE(worlds.Universe.ClosePresentation(childEndpoint) == world::PresentationStatus::Ok);
			CHECK_FALSE(source.TakeTree("Door", START));
		} else {
			CHECK(source.TakeTree("Door", START).has_value());
		}
	}
	SECTION("consumer endpoint withdrawal") {
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Replies) == world::PresentationStatus::Ok);
		CHECK_FALSE(source.TakeTree("Door", START));
	}
	CHECK(renderer.PortalImageUsage().Images == before.Images);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == before.PendingCpuBytes);
}

TEST_CASE(
	"payload portal sources consume completed captures before issuing deferred demand",
	"[render][portal-runtime][portal-payload]"
) {
	RuntimeWorlds worlds(1);
	Renderer renderer;
	PortalImageSource source(
		worlds.Universe,
		renderer,
		worlds.Source,
		worlds.Replies,
		{},
		nullptr,
		PortalImageSourceDelivery::CapturePayloads
	);
	auto first = Request();
	first.OrderedLayers = true;
	first.Scope = PortalImageScope::OpaqueLighting;
	first.PixelBudget = 1024;
	const auto issued = source.Issue(worlds.Requests, first, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	auto latest = first;
	latest.Key.CameraRevision = 7;
	PortalGeometry geometry;
	geometry.Rows.emplace_back();
	geometry.Rows.front().Name = "latest";
	std::string error;
	REQUIRE(EncodePortalGeometry(geometry, latest.Geometry, error));
	CHECK(source.Issue(worlds.Requests, latest, Binding(), START).Status == PortalInboxStatus::Busy);

	const auto requests = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(requests.size() == 1);
	PortalImageRequest accepted;
	REQUIRE(DecodePortalImageRequest(requests.front().Payload, accepted, error));
	PortalImageLayerSet layers = PayloadNode(accepted, worlds.Requests, false).Layers;
	std::vector<std::byte> wire;
	REQUIRE(EncodePortalImageLayerSet(layers, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination, worlds.Requests, worlds.Replies, issued.RequestId, wire
		) == world::PresentationStatus::Ok
	);
	REQUIRE(source.Poll(START).size() == 1);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	const std::array occupied{std::byte{0}};
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 999, occupied) ==
		world::PresentationStatus::Ok
	);

	const auto captured = source.TakeTree("Door", START);
	REQUIRE(captured);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
	CHECK(source.Poll(START).empty());
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
	REQUIRE(worlds.Universe.TakePresentation(worlds.Requests).size() == 1);
	CHECK(source.Poll(START + std::chrono::milliseconds(1)).empty());
	const auto deferred = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(deferred.size() == 1);
	PortalImageRequest decoded;
	REQUIRE(DecodePortalImageRequest(deferred.front().Payload, decoded, error));
	uint64_t expectedRevision = latest.Key.CameraRevision;
	for (const auto byte : assets::Hasher::Of(latest.Geometry).Digest)
		expectedRevision = scene::MixSignature(expectedRevision, byte);
	CHECK(decoded.Key.CameraRevision == expectedRevision);
	CHECK(source.Poll(START + std::chrono::milliseconds(2)).empty());
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
}

TEST_CASE(
	"retained body leaf layers publish one owned capture tree",
	"[render][gpu][portal-runtime][tree-source][.]"
) {
	const bool retained = GENERATE(false, true);
	RuntimeWorlds worlds;
	const auto unrelated = worlds.Universe.Create({.Name = core::Name("import-unrelated")});
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageSource source(worlds.Universe, fixture.Render, worlds.Source, worlds.Replies);
	const world::PresentationAddress publicRoot{
		"public-import-root", std::string(PORTAL_REQUEST_CHANNEL), 700, 71
	};
	world::PresentationBindings bindings{700, 1, {{worlds.Requests, publicRoot}}, {}};
	if (retained) source.SetEndpointBindings(&bindings);
	auto request = Request();
	request.OrderedLayers = true;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.RecursionDepth = 0;
	request.PixelBudget = 1024;
	request.Position = {2, 3, 4};
	request.Entrance = PortalImageEntrance{"import-entrance", {}, {1, 0, 0}, {0, 1, 0}};
	request.RetainedBodyPlayer = retained ? "91" : "";
	const auto issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(messages.size() == 1);
	std::string error;
	REQUIRE(DecodePortalImageRequest(messages.front().Payload, request, error));
	const auto node = PayloadNode(request, retained ? publicRoot : worlds.Requests, false);
	std::vector<std::byte> wire;
	REQUIRE(EncodePortalImageLayerSet(node.Layers, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination, worlds.Requests, worlds.Replies, issued.RequestId, wire
		) == world::PresentationStatus::Ok
	);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK_FALSE(source.Capture("Door"));
	CHECK(source.ReferencesWorld("import-entrance"));
	REQUIRE(source.RenameIdleForUnrelatedRoute("import-unrelated"));
	REQUIRE(worlds.Universe.Rename(unrelated, core::Name("import-renamed")) == world::WorldStatus::Ok);
	SceneTarget target{8, 8};
	View view;
	view.World = worlds.Source.Index;
	view.WorldName = core::Name(worlds.Replies.World);
	view.Target = &target;
	OverlayImage overlay;
	REQUIRE(
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("surface-capture"))
	);
	const auto completed = source.Poll(START);
	REQUIRE(completed.size() == 1);
	REQUIRE(completed.front().Status == PortalImageStatus::Ok);
	const auto captured = source.Capture("Door");
	REQUIRE(captured);
	CHECK(captured->TransparentImages[0] != 0);
	CHECK(captured->TransparentImages[1] != 0);
	CHECK(captured->RetainedBodyPlayer == request.RetainedBodyPlayer);
	CHECK(source.ReferencesWorld("import-entrance"));
	CHECK(source.RenameIdleForUnrelatedRoute("import-renamed"));
	if (retained) {
		REQUIRE(captured->Tree != 0);
		const auto *tree = fixture.Render.FindPortalCaptureTree(captured->Tree);
		REQUIRE(tree);
		REQUIRE(tree->Nodes.size() == 1);
		CHECK(tree->Edges.empty());
		CHECK(tree->Nodes.front().Producer == node.Producer);
		CHECK(tree->Nodes.front().Camera == node.Camera);
		CHECK(tree->Nodes.front().RetainedBodyPlayer == "91");
		CHECK(tree->Nodes.front().Binding.Expected == request.Key);
		CHECK(tree->Nodes.front().Images[0] == captured->Image);
		CHECK(source.PinCapture("Door", captured->Tree, request.Key, START, START + std::chrono::seconds(2)));
		CHECK(source.RenameIdleForUnrelatedRoute("import-renamed"));
	} else {
		CHECK(captured->Tree == 0);
	}
	const auto replacementWorld = worlds.Universe.Create({.Name = core::Name("replacement-producer")});
	REQUIRE(replacementWorld.IsValid());
	const auto replacement =
		worlds.Universe.OpenPresentation(replacementWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	auto replacementRequest = request;
	replacementRequest.Entrance->SourceWorld = "replacement-entrance";
	++replacementRequest.Key.CameraRevision;
	replacementRequest.Key.RequestId = 0;
	const auto replacementIssue = source.Issue(replacement, replacementRequest, Binding(), START);
	REQUIRE(replacementIssue.Status == PortalInboxStatus::Issued);
	CHECK(source.Image("Door") == captured->Image);
	CHECK(source.ReferencesWorld("import-entrance"));
	CHECK(source.ReferencesWorld("replacement-entrance"));
	CHECK(source.RenameIdleForUnrelatedRoute("import-renamed"));
	const auto retainedCapture = source.Capture("Door");
	REQUIRE(retainedCapture);
	CHECK(retainedCapture->Producer == worlds.Requests);
	SECTION("completed replacement preserves and retires the old pin") {
		if (retained) {
			const auto replacementMessages = worlds.Universe.TakePresentation(replacement);
			REQUIRE(replacementMessages.size() == 1);
			PortalImageRequest received;
			REQUIRE(DecodePortalImageRequest(replacementMessages.front().Payload, received, error));
			const auto replacementNode = PayloadNode(received, replacement, false);
			std::vector<std::byte> replacementWire;
			REQUIRE(EncodePortalImageLayerSet(replacementNode.Layers, replacementWire, error));
			REQUIRE(
				worlds.Universe.SendPresentation(
					replacementWorld, replacement, worlds.Replies, replacementIssue.RequestId, replacementWire
				) == world::PresentationStatus::Ok
			);
			CHECK(source.Poll(START).empty());
			REQUIRE(source.HasPendingUploads());
			REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false)
						.Ran(core::Name("surface-capture")));
			const auto replacementCompletion = source.Poll(START);
			REQUIRE(replacementCompletion.size() == 1);
			REQUIRE(replacementCompletion.front().Status == PortalImageStatus::Ok);
			REQUIRE(fixture.Render.FindPortalCaptureTree(captured->Tree) != nullptr);
			CHECK(source.ReferencesWorld("import-entrance"));
			CHECK(source.ReferencesWorld(worlds.Requests.World));
			CHECK(source.RenameIdleForUnrelatedRoute("import-renamed"));
			bindings.Exports.clear();
			CHECK(source.ReferencesWorld(worlds.Requests.World));
			source.InvalidateEndpoint(worlds.Requests);
			CHECK(fixture.Render.FindPortalCaptureTree(captured->Tree) == nullptr);
			CHECK_FALSE(source.ReferencesWorld("import-entrance"));
			const auto current = source.Capture("Door");
			REQUIRE(current);
			CHECK(current->Producer == replacement);
		}
	}
	SECTION("old producer invalidation retires the retained image") {
		source.InvalidateEndpoint(worlds.Requests);
		CHECK(source.Image("Door") == 0);
		source.Clear();
		CHECK_FALSE(source.Capture("Door"));
		CHECK(fixture.Render.PortalImageUsage().Images == 0);
		if (retained) CHECK(fixture.Render.FindPortalCaptureTree(captured->Tree) == nullptr);
	}
}

TEST_CASE(
	"portal sources publish recursive captures atomically after upload",
	"[render][gpu][portal-runtime][tree-source][.]"
) {
	const bool explicitInvalidation = GENERATE(false, true);
	RuntimeWorlds worlds;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageSource source(worlds.Universe, fixture.Render, worlds.Source, worlds.Replies);
	auto request = Request();
	request.OrderedLayers = true;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.RecursionDepth = 2;
	request.PixelBudget = 1024;
	const auto childWorld = worlds.Universe.Create({.Name = core::Name("import-child")});
	const auto child =
		worlds.Universe.OpenPresentation(childWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	auto issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	{
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 1);
		std::string decodeError;
		REQUIRE(DecodePortalImageRequest(messages.front().Payload, request, decodeError));
		CHECK(request.Key.RequestId == issued.RequestId);
	}
	PortalCaptureTree tree;
	tree.Nodes.push_back(PayloadNode(request, worlds.Requests, false));
	tree.Nodes.push_back(PayloadNode(request, child, true));
	auto &rootLayers = tree.Nodes[0].Layers;
	rootLayers.SpatialOverlay = rootLayers.Transparent[0];
	rootLayers.SpatialOverlay->Depth.clear();
	rootLayers.SpatialOverlay->DepthHash = {};
	PortalCaptureTreeEdge edge;
	edge.PortalKey = "ChildDoor";
	PortalGeometry geometry;
	geometry.Rows.emplace_back();
	std::string error;
	REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
	tree.Edges.push_back(std::move(edge));
	const auto deliver = [&](const std::vector<std::byte> &wire) {
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Destination, worlds.Requests, worlds.Replies, issued.RequestId, wire
			) == world::PresentationStatus::Ok
		);
	};
	std::vector<std::byte> wire;
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	deliver(wire);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK(source.Image("Door") == 0);
	CHECK_FALSE(source.Capture("Door"));
	SceneTarget target{8, 8};
	View view;
	view.World = worlds.Source.Index;
	view.WorldName = core::Name(worlds.Replies.World);
	view.Target = &target;
	OverlayImage overlay;
	REQUIRE(
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("surface-capture"))
	);
	CHECK(source.Image("Door") == 0);
	const auto completed = source.Poll(START);
	REQUIRE(completed.size() == 1);
	CHECK(completed[0].Status == PortalImageStatus::Ok);
	CHECK_FALSE(source.HasPendingUploads());
	auto captured = source.Capture("Door");
	REQUIRE(captured);
	REQUIRE(captured->Tree != 0);
	REQUIRE(source.Image("Door") != 0);
	CHECK(captured->TransparentImages[0] != 0);
	CHECK(captured->TransparentImages[1] != 0);
	CHECK(captured->SpatialOverlayImage != 0);
	const auto *imported = fixture.Render.FindPortalCaptureTree(captured->Tree);
	REQUIRE(imported);
	REQUIRE(imported->Nodes.size() == 2);
	CHECK(imported->Nodes[1].Camera == tree.Nodes[1].Camera);
	CHECK(imported->Nodes[1].Producer == tree.Nodes[1].Producer);
	CHECK(imported->Edges == tree.Edges);
	const auto firstTree = captured->Tree;
	const auto replacementWorld = worlds.Universe.Create({.Name = core::Name("replacement-child")});
	const auto replacementChild =
		worlds.Universe.OpenPresentation(replacementWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	tree.Nodes[1].Producer = {
		replacementChild.World,
		replacementChild.Channel,
		replacementChild.Session,
		replacementChild.Generation
	};
	request.Key.CameraRevision++;
	issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	{
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 1);
		REQUIRE(DecodePortalImageRequest(messages.front().Payload, request, error));
	}
	for (auto &node : tree.Nodes) {
		node.Layers.Opaque.Key = request.Key;
		node.Layers.Opaque.CaptureTick++;
		for (auto &layer : node.Layers.Transparent) {
			layer.Key = request.Key;
			layer.CaptureTick++;
		}
		if (node.Layers.SpatialOverlay) {
			node.Layers.SpatialOverlay->Key = request.Key;
			node.Layers.SpatialOverlay->CaptureTick++;
		}
	}
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	deliver(wire);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	REQUIRE(source.Capture("Door"));
	CHECK(source.Capture("Door")->Tree == firstTree);
	if (explicitInvalidation)
		source.InvalidateEndpoint(child);
	else
		REQUIRE(worlds.Universe.ClosePresentation(child) == world::PresentationStatus::Ok);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK_FALSE(source.Capture("Door"));

	REQUIRE(
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("surface-capture"))
	);
	const auto replacement = source.Poll(START);
	REQUIRE(replacement.size() == 1);
	CHECK(replacement.front().Status == PortalImageStatus::Ok);
	captured = source.Capture("Door");
	REQUIRE(captured);
	CHECK(captured->Tree != firstTree);
	CHECK_FALSE(fixture.Render.FindPortalCaptureTree(firstTree));
	request.Key.CameraRevision++;
	issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	{
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 1);
		REQUIRE(DecodePortalImageRequest(messages.front().Payload, request, error));
	}
	for (auto &node : tree.Nodes) {
		node.Layers.Opaque.Key = request.Key;
		node.Layers.Opaque.Key.CameraRevision = request.Key.CameraRevision;
		for (auto &layer : node.Layers.Transparent)
			layer.Key = node.Layers.Opaque.Key;
		if (node.Layers.SpatialOverlay) node.Layers.SpatialOverlay->Key = node.Layers.Opaque.Key;
	}
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	wire.pop_back();
	deliver(wire);
	source.Poll(START);
	REQUIRE(source.Capture("Door"));
	CHECK(source.Capture("Door")->Tree == captured->Tree);
	CHECK(source.Image("Door") == replacement[0].Image);
	CHECK_FALSE(source.HasPendingUploads());
	REQUIRE(worlds.Universe.ClosePresentation(replacementChild) == world::PresentationStatus::Ok);
	source.Poll(START);
	CHECK_FALSE(source.Capture("Door"));
	CHECK(source.Image("Door") == 0);
	CHECK_FALSE(fixture.Render.FindPortalCaptureTree(captured->Tree));
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
}

TEST_CASE(
	"payload sources authenticate public endpoint tuples through exact host bindings",
	"[render][portal-runtime][portal-payload][endpoint-bindings]"
) {
	const bool nested = GENERATE(false, true);
	RuntimeWorlds worlds;
	Renderer renderer;
	const auto childWorld = worlds.Universe.Create({.Name = core::Name("binding-child-alias")});
	const auto childLocal =
		worlds.Universe.OpenPresentation(childWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	const world::PresentationAddress rootPublic{
		"published-root", std::string(PORTAL_REQUEST_CHANNEL), 700, 71
	};
	const world::PresentationAddress childPublic{
		"published-child", std::string(PORTAL_REQUEST_CHANNEL), 800, 81
	};
	world::PresentationBindings bindings{
		700, 1, {{worlds.Requests, rootPublic}}, {{childLocal, childPublic}}
	};
	PortalInboxLimits limits;
	limits.Timeout = std::chrono::milliseconds(1);
	PortalImageSource source(
		worlds.Universe,
		renderer,
		worlds.Source,
		worlds.Replies,
		limits,
		nullptr,
		PortalImageSourceDelivery::CapturePayloads
	);
	source.SetEndpointBindings(&bindings);
	auto request = Request();
	request.OrderedLayers = true;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.PixelBudget = 1024;
	request.RecursionDepth = nested ? 2 : 0;
	const auto issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(messages.size() == 1);
	std::string error;
	REQUIRE(DecodePortalImageRequest(messages[0].Payload, request, error));
	PortalCaptureTree expected;
	expected.Nodes.push_back(PayloadNode(request, rootPublic, false));
	expected.Nodes.front().Layers.SpatialOverlay = expected.Nodes.front().Layers.Transparent.front();
	expected.Nodes.front().Layers.SpatialOverlay->Depth.clear();
	expected.Nodes.front().Layers.SpatialOverlay->DepthHash = {};
	if (nested) {
		expected.Nodes.push_back(PayloadNode(request, childPublic, true));
		PortalCaptureTreeEdge edge;
		edge.PortalKey = "ChildDoor";
		PortalGeometry geometry;
		geometry.Rows.emplace_back();
		REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
		expected.Edges.push_back(std::move(edge));
	}
	bool refused = false;
	auto sender = worlds.Requests;
	SECTION("public root is checked independently of authenticated transport") {
		if (nested) {
			expected.Nodes[0].Producer.Generation++;
			refused = true;
		}
	}
	SECTION("unmapped child incarnation is refused") {
		if (nested) {
			expected.Nodes[1].Producer.Generation++;
			refused = true;
		}
	}
	SECTION("local return aliases cannot claim a canonical child identity") {
		if (nested) {
			expected.Nodes[1].Producer = {
				childLocal.World, childLocal.Channel, childLocal.Session, childLocal.Generation
			};
			refused = true;
		}
	}
	SECTION("matching public tuples do not replace outer transport authentication") {
		sender = worlds.Universe.OpenPresentation(worlds.Destination, core::Name("wrong-producer")).Address;
		refused = true;
	}
	SECTION("mapping replacement while pending refuses the old public root") {
		bindings.Revision++;
		bindings.Exports[0].Published.Generation++;
		refused = true;
	}
	SECTION("matching public tuples are accepted") {}
	std::vector<std::byte> wire;
	if (nested)
		REQUIRE(EncodePortalCaptureTree(expected, wire, error));
	else
		REQUIRE(EncodePortalImageLayerSet(expected.Nodes[0].Layers, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination, sender, worlds.Replies, issued.RequestId, wire
		) == world::PresentationStatus::Ok
	);
	const auto completions = source.Poll(START);
	if (refused) {
		CHECK(completions.empty());
		CHECK_FALSE(source.TakeTree("Door", START));
		return;
	}
	REQUIRE(completions.size() == 1);
	CHECK(completions[0].Image == 0);
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
	if (nested) {
		CHECK(source.ReferencesWorld(childLocal.World));
		CHECK_FALSE(source.RenameIdleForUnrelatedRoute(childLocal.World));
	}
	SECTION("owned payload retains canonical tuples and camera") {
		const auto captured = source.TakeTree("Door", START);
		REQUIRE(captured);
		CHECK(*captured == expected);
		const auto &root = captured->Nodes.front();
		CHECK(root.Camera.Position == request.Position);
		CHECK(root.Camera.Orientation == request.Orientation);
		CHECK(root.Layers.Opaque.Key.CameraRevision == request.Key.CameraRevision);
		CHECK(root.Layers.Opaque.ContentRevision == 22);
		CHECK(root.Layers.Opaque.CaptureTick == 21);
		CHECK(root.Layers.Transparent.size() == 2);
		for (const auto &layer : root.Layers.Transparent) {
			CHECK(layer.Key.CameraRevision == request.Key.CameraRevision);
			CHECK(layer.ContentRevision == root.Layers.Opaque.ContentRevision);
			CHECK(layer.CaptureTick == root.Layers.Opaque.CaptureTick);
		}
		REQUIRE(root.Layers.SpatialOverlay);
		CHECK(root.Layers.SpatialOverlay->Key.CameraRevision == request.Key.CameraRevision);
		CHECK(root.Layers.SpatialOverlay->ContentRevision == root.Layers.Opaque.ContentRevision);
		CHECK_FALSE(source.TakeTree("Door", START));
	}
	SECTION("authorized handoff expires at its bounded lifetime") {
		CHECK_FALSE(source.TakeTree("Door", START + std::chrono::milliseconds(2)));
	}
	SECTION("root mapping replacement retires held payload") {
		bindings.Revision++;
		bindings.Exports[0].Published.Generation++;
		CHECK_FALSE(source.TakeTree("Door", START));
	}
	SECTION("root mapping withdrawal retires held payload") {
		bindings.Revision++;
		bindings.Exports.clear();
		CHECK_FALSE(source.TakeTree("Door", START));
	}
	SECTION("child mapping withdrawal retires nested payload") {
		bindings.Revision++;
		bindings.Returns.clear();
		CHECK(source.TakeTree("Door", START).has_value() == !nested);
	}
	SECTION("local child route withdrawal retires nested payload") {
		REQUIRE(worlds.Universe.ClosePresentation(childLocal) == world::PresentationStatus::Ok);
		CHECK(source.TakeTree("Door", START).has_value() == !nested);
	}
	SECTION("local child invalidation retires aliased nested payload") {
		source.InvalidateEndpoint(childLocal);
		CHECK(source.TakeTree("Door", START).has_value() == !nested);
	}
	SECTION("shared published child survives one local invalidation") {
		if (nested) {
			const auto alternateWorld = worlds.Universe.Create({.Name = core::Name("alternate-child")});
			const auto alternate =
				worlds.Universe.OpenPresentation(alternateWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
			REQUIRE(alternate.Generation != 0);
			bindings.Exports.push_back({alternate, childPublic});
			source.InvalidateEndpoint(childLocal);
			CHECK(source.TakeTree("Door", START).has_value());
		}
	}
}

TEST_CASE(
	"producer retained body exclusion defaults to denial and rechecks authorization",
	"[render][portal-runtime][retained-body]"
) {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	auto request = Request();
	request.Key.RequestId = 1;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.PixelBudget *= 4;
	request.EyePlayer = "17";
	request.RetainedBodyPlayer = "91";
	size_t checks = 0;
	SECTION("missing trusted policy refuses before renderer setup") {}
	SECTION("policy sees exact requester and distinct account") {
		producer.SetRetainedBodyAuthorization([&](const auto &requester, std::string_view account) {
			++checks;
			CHECK(requester == worlds.Replies);
			CHECK(account == "91");
			return false;
		});
	}
	SECTION("authorization withdrawn while accepted work waits") {
		producer.SetRetainedBodyAuthorization([&](const auto &requester, std::string_view account) {
			CHECK(requester == worlds.Replies);
			CHECK(account == "91");
			return ++checks == 1;
		});
	}
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 1, wire) ==
		world::PresentationStatus::Ok
	);
	const auto progress = producer.Pump(0, 0, START);
	CHECK(progress.Rendered == 0);
	const auto replies = worlds.Universe.TakePresentation(worlds.Replies);
	REQUIRE(replies.size() == 1);
	PortalImageReply reply;
	REQUIRE(DecodePortalImageReply(replies.front().Payload, reply, error));
	CHECK(reply.Status == PortalImageStatus::Unavailable);
	CHECK(reply.Diagnostic.find("authoriz") != std::string::npos);
	CHECK(reply.Pixels.empty());
	CHECK(reply.Key == request.Key);
}
