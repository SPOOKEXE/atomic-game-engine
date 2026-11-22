// Portal image runtime transport, demand coalescing, reply endpoints and capture camera tests.
#include "PortalImageRuntimeFixtures.hpp"

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

TEST_CASE("portal requests restart without accepting old replies", "[render][portal-transport-restart]") {
	RuntimeWorlds worlds;
	Renderer sourceRenderer, destinationRenderer;
	PortalImageSource source(worlds.Universe, sourceRenderer, worlds.Source, worlds.Replies);
	PortalImageProducer producer(worlds.Universe, destinationRenderer, worlds.Destination, worlds.Requests);
	CHECK(source.RenameIdle());
	CHECK(producer.RenameIdle());
	const auto first = source.Issue(worlds.Requests, Request(), Binding(), START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	CHECK_FALSE(source.RenameIdle());
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

TEST_CASE("retained shadow replies protect their target world", "[render][portal-runtime]") {
	const bool mapped = GENERATE(false, true);
	const bool successful = GENERATE(false, true);
	const bool preRetired = GENERATE(false, true);
	const bool removedAlias = GENERATE(false, true);
	RuntimeWorlds worlds;
	const auto target = worlds.Universe.Create({.Name = core::Name("reply-target")});
	const auto unrelated = worlds.Universe.Create({.Name = core::Name("reply-unrelated")});
	const auto localTarget =
		worlds.Universe.OpenPresentation(target, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
	REQUIRE(localTarget.Generation != 0);
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	const world::PresentationAddress publicTarget{
		"public-reply-target", std::string(PORTAL_REQUEST_CHANNEL), 1, 1
	};
	world::PresentationBindings bindings{1, 1, {}, {{localTarget, publicTarget}}};
	if (mapped) {
		const auto otherWorld = worlds.Universe.Create({.Name = core::Name("other-alias")});
		const auto otherLocal =
			worlds.Universe.OpenPresentation(otherWorld, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
		auto otherPublic = publicTarget;
		++otherPublic.Generation;
		bindings.Exports.push_back({otherLocal, otherPublic});
		source.SetEndpointBindings(&bindings);
	}
	PortalShadowPull pull;
	pull.ParentEye = {1, "Door", 1, 1};
	const auto &targetEndpoint = mapped ? publicTarget : localTarget;
	pull.TargetProducer = {
		targetEndpoint.World, targetEndpoint.Channel, targetEndpoint.Session, targetEndpoint.Generation
	};
	pull.TargetEye = {2, "Door", 1, 1};
	std::vector<std::byte> wire;
	std::string error;
	PortalShadowPacket packet{pull, successful ? PortalImageStatus::Ok : PortalImageStatus::Unavailable, {}};
	if (successful) {
		PortalShadowSnapshot snapshot;
		snapshot.Producer = pull.TargetProducer;
		snapshot.Eye = pull.TargetEye;
		snapshot.EyePixelHash = assets::Hasher::Of(std::as_bytes(std::span("eye", 3)));
		snapshot.DepthHash = assets::Hasher::Of(std::as_bytes(std::span("depth", 5)));
		snapshot.SourceBounds = snapshot.DomainBounds = {-1, -1, -1, 1, 1, 1};
		snapshot.LightViewProjection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		REQUIRE(EncodePortalShadowManifest(snapshot, packet.Payload, error));
	}
	REQUIRE(EncodePortalShadowPacket(packet, wire, error));
	if (preRetired) {
		REQUIRE(worlds.Universe.ClosePresentation(localTarget) == world::PresentationStatus::Ok);
		REQUIRE(
			worlds.Universe.OpenPresentation(target, core::Name(PORTAL_REQUEST_CHANNEL)).Address.Generation !=
			localTarget.Generation
		);
		if (mapped && removedAlias) {
			source.InvalidateEndpoint(publicTarget);
			bindings.Returns.clear();
			bindings.Exports.clear();
		} else if (mapped) {
			++bindings.Returns.front().Published.Generation;
		}
	}
	const std::array<std::string_view, 1> references{"reply-target"};
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination,
			worlds.Requests,
			worlds.Replies,
			1,
			wire,
			world::PresentationPriority::Routine,
			references
		) == world::PresentationStatus::Ok
	);
	std::vector<std::byte> expectedWire;
	REQUIRE(EncodePortalShadowPull(pull, expectedWire, error));
	const std::array<PortalShadowReplyExpectation, 1> expected{
		{{worlds.Requests, worlds.Replies, 1, expectedWire}}
	};
	const auto live = preRetired && mapped && removedAlias && successful
						  ? std::span<const PortalShadowReplyExpectation>{}
						  : std::span<const PortalShadowReplyExpectation>{expected};
	CHECK(source.PollExpected(START, live).empty());
	if (preRetired) {
		CHECK(source.TakeShadowReply().has_value() == !successful);
		return;
	}
	CHECK(source.ReferencesWorld("reply-target"));
	CHECK_FALSE(source.RenameIdleForUnrelatedRoute("reply-target"));
	CHECK(source.RenameIdleForUnrelatedRoute("reply-unrelated"));
	if (mapped) CHECK(source.RenameIdleForUnrelatedRoute("other-alias"));
	CHECK(worlds.Universe.Rename(unrelated, core::Name("reply-renamed")) == world::WorldStatus::Ok);
	CHECK(worlds.Universe.NameOf(target) == core::Name("reply-target"));
	SECTION("taking the reply releases its target") {
		REQUIRE(source.TakeShadowReply());
		CHECK_FALSE(source.ReferencesWorld("reply-target"));
	}
	SECTION("target invalidation releases the reply") {
		source.InvalidateEndpoint(localTarget);
		CHECK_FALSE(source.TakeShadowReply());
		CHECK_FALSE(source.ReferencesWorld("reply-target"));
	}
	SECTION("successful reply is discarded when its target disappears") {
		REQUIRE(worlds.Universe.ClosePresentation(localTarget) == world::PresentationStatus::Ok);
		CHECK(source.TakeShadowReply().has_value() == !successful);
	}
}

TEST_CASE("shadow replies may carry an upstream-only target", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	PortalShadowPull pull;
	pull.ParentEye = {1, "Door", 1, 1};
	pull.TargetProducer = {"upstream-only", std::string(PORTAL_REQUEST_CHANNEL), 1, 1};
	pull.TargetEye = {2, "Door", 1, 1};
	PortalShadowSnapshot snapshot;
	snapshot.Producer = pull.TargetProducer;
	snapshot.Eye = pull.TargetEye;
	snapshot.EyePixelHash = assets::Hasher::Of(std::as_bytes(std::span("eye", 3)));
	snapshot.DepthHash = assets::Hasher::Of(std::as_bytes(std::span("depth", 5)));
	snapshot.SourceBounds = snapshot.DomainBounds = {-1, -1, -1, 1, 1, 1};
	snapshot.LightViewProjection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	PortalShadowPacket packet{pull, PortalImageStatus::Ok, {}};
	std::string error;
	REQUIRE(EncodePortalShadowManifest(snapshot, packet.Payload, error));
	std::vector<std::byte> wire;
	REQUIRE(EncodePortalShadowPacket(packet, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Destination, worlds.Requests, worlds.Replies, 1, wire) ==
		world::PresentationStatus::Ok
	);
	std::vector<std::byte> expectedWire;
	REQUIRE(EncodePortalShadowPull(pull, expectedWire, error));
	const std::array<PortalShadowReplyExpectation, 1> expected{
		{{worlds.Requests, worlds.Replies, 1, expectedWire}}
	};
	auto wrong = expected;
	wrong[0].Requester.World = "another-receiver";
	CHECK(source.PollExpected(START, wrong).empty());
	CHECK_FALSE(source.TakeShadowReply());
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Destination, worlds.Requests, worlds.Replies, 1, wire) ==
		world::PresentationStatus::Ok
	);
	CHECK(source.PollExpected(START, expected).empty());
	CHECK(source.ReferencesWorld("upstream-only"));
	CHECK(source.PollExpected(START, {}).empty());
	CHECK_FALSE(source.TakeShadowReply());
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Destination, worlds.Requests, worlds.Replies, 1, wire) ==
		world::PresentationStatus::Ok
	);
	CHECK(source.PollExpected(START, expected).empty());
	CHECK(source.ReferencesWorld("upstream-only"));
	CHECK(source.TakeShadowReply().has_value());
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
	CHECK_FALSE(producer.RenameIdle());
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

TEST_CASE("portal source coalesces the latest in-flight camera demand", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	auto first = Request();
	first.Key.CameraRevision = 17;
	first.Position = {0, 1, 2};
	first.Orientation[3] = 1;
	auto second = first;
	second.Key.CameraRevision = 18;
	second.Position = {1, 0, 0};
	PortalGeometry secondGeometry;
	secondGeometry.Rows.emplace_back();
	secondGeometry.Rows.front().Name = "second";
	std::string geometryError;
	REQUIRE(EncodePortalGeometry(secondGeometry, second.Geometry, geometryError));
	auto latest = first;
	latest.Key.CameraRevision = 19;
	latest.Position = {2, 0, 0};
	latest.Orientation = {0, 0, .70710677f, .70710677f};
	latest.Frustum = {-.12f, .16f, -.08f, .11f, .2f, 300};
	PortalGeometry latestGeometry;
	latestGeometry.Rows.emplace_back();
	latestGeometry.Rows.front().Name = "latest";
	REQUIRE(EncodePortalGeometry(latestGeometry, latest.Geometry, geometryError));
	REQUIRE(source.Issue(worlds.Requests, first, Binding(), START).Status == PortalInboxStatus::Issued);

	SECTION("deferred recursive demand retains unknown world dependencies") {
		auto deeper = first;
		++deeper.Key.CameraRevision;
		deeper.RecursionDepth = 1;
		deeper.PixelBudget = 1024;
		CHECK(source.Issue(worlds.Requests, deeper, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(source.ReferencesWorld("unknown-descendant"));
		CHECK_FALSE(source.RenameIdleForUnrelatedRoute("unknown-descendant"));
	}
	SECTION("deferred leaf demand permits unrelated rename") {
		const auto unrelated = worlds.Universe.Create({.Name = core::Name("deferred-unrelated")});
		CHECK(source.Issue(worlds.Requests, second, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK_FALSE(source.RenameIdleForUnrelatedRoute(worlds.Replies.World));
		CHECK_FALSE(source.RenameIdleForUnrelatedRoute(worlds.Requests.World));
		REQUIRE(source.RenameIdleForUnrelatedRoute("deferred-unrelated"));
		CHECK(worlds.Universe.Rename(unrelated, core::Name("deferred-renamed")) == world::WorldStatus::Ok);
	}

	SECTION("completion immediately sends only the latest distinct demand") {
		CHECK(source.Issue(worlds.Requests, second, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(source.Issue(worlds.Requests, latest, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(source.Issue(worlds.Requests, latest, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
		CHECK(producer.Pump(0, 0, START).Requests == 1);
		const auto completed = source.Poll(START);
		REQUIRE(completed.size() == 1);
		CHECK(completed.front().RequestId != 0);
		CHECK(completed.front().Status == PortalImageStatus::Unavailable);
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);

		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 1);
		PortalImageRequest decoded;
		std::string error;
		REQUIRE(DecodePortalImageRequest(messages.front().Payload, decoded, error));
		CHECK(decoded.Geometry == latest.Geometry);
		uint64_t expectedCameraRevision = latest.Key.CameraRevision;
		for (const auto byte : assets::Hasher::Of(latest.Geometry).Digest)
			expectedCameraRevision = scene::MixSignature(expectedCameraRevision, byte);
		CHECK(decoded.Key.CameraRevision == expectedCameraRevision);
		CHECK(decoded.Position == latest.Position);
		CHECK(decoded.Orientation == latest.Orientation);
		CHECK(decoded.Frustum == latest.Frustum);
		// Poll already issued the retained latest demand, so a repeated request
		// joins that in-flight capture instead of opening another one.
		CHECK(source.Issue(worlds.Requests, latest, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Source,
				worlds.Replies,
				worlds.Requests,
				messages.front().Correlation,
				messages.front().Payload
			) == world::PresentationStatus::Ok
		);
		CHECK(producer.Pump(0, 0, START + std::chrono::milliseconds(1)).Requests == 1);
		REQUIRE(source.Poll(START + std::chrono::milliseconds(1)).size() == 1);
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
		CHECK(source.Poll(START + std::chrono::milliseconds(2)).empty());
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	}

	SECTION("transfer eye supersedes a routine request and rejects its stale reply") {
		auto transfer = latest;
		transfer.TransferEye = true;
		CHECK(source.Issue(worlds.Requests, transfer, Binding(), START).Status == PortalInboxStatus::Issued);
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 2);
		const auto routine = std::find_if(messages.begin(), messages.end(), [](const auto &message) {
			PortalImageRequest decoded;
			std::string error;
			return DecodePortalImageRequest(message.Payload, decoded, error) && !decoded.TransferEye;
		});
		const auto urgent = std::find_if(messages.begin(), messages.end(), [](const auto &message) {
			PortalImageRequest decoded;
			std::string error;
			return DecodePortalImageRequest(message.Payload, decoded, error) && decoded.TransferEye;
		});
		REQUIRE(routine != messages.end());
		REQUIRE(urgent != messages.end());
		CHECK(routine->Correlation != urgent->Correlation);
		PortalImageRequest decoded;
		std::string error;
		REQUIRE(DecodePortalImageRequest(urgent->Payload, decoded, error));
		CHECK(decoded.TransferEye);
		CHECK(decoded.Position == transfer.Position);
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Destination, worlds.Requests, worlds.Replies, routine->Correlation, routine->Payload
			) == world::PresentationStatus::Ok
		);
		CHECK(source.Poll(START).empty());
	}

	SECTION("endpoint invalidation drops the retained demand") {
		CHECK(source.Issue(worlds.Requests, second, Binding(), START).Status == PortalInboxStatus::Busy);
		source.InvalidateEndpoint(worlds.Requests);
		CHECK(producer.Pump(0, 0, START).Requests == 1);
		CHECK(source.Poll(START).empty());
		CHECK(producer.Pump(0, 0, START + std::chrono::milliseconds(1)).Requests == 0);
	}

	SECTION("returning to the in-flight camera drops older movement") {
		CHECK(source.Issue(worlds.Requests, latest, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(source.Issue(worlds.Requests, first, Binding(), START).Status == PortalInboxStatus::Busy);
		CHECK(producer.Pump(0, 0, START).Requests == 1);
		REQUIRE(source.Poll(START).size() == 1);
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	}

	SECTION("a sampling-only change is retained") {
		auto sampled = Binding();
		sampled.Sampling[3][0] = .25f;
		CHECK(source.Issue(worlds.Requests, first, sampled, START).Status == PortalInboxStatus::Busy);
		CHECK(producer.Pump(0, 0, START).Requests == 1);
		REQUIRE(source.Poll(START).size() == 1);
		CHECK(worlds.Universe.PresentationQueueUsage().Messages == 1);
	}

	SECTION("seam changes still supersede immediately") {
		auto changed = first;
		changed.Key.SeamRevision = 1;
		const auto superseding = source.Issue(worlds.Requests, changed, Binding(), START);
		CHECK(superseding.Status == PortalInboxStatus::Issued);
		CHECK(superseding.RequestId != 0);
		const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
		REQUIRE(messages.size() == 2);
	}

	SECTION("a replacement endpoint still supersedes immediately") {
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
		const auto replacement =
			worlds.Universe.OpenPresentation(worlds.Destination, core::Name(PORTAL_REQUEST_CHANNEL));
		REQUIRE(replacement.Status == world::PresentationStatus::Ok);
		CHECK(source.Issue(replacement.Address, first, Binding(), START).Status == PortalInboxStatus::Issued);
	}

	SECTION("bounded transport pressure retries the retained latest demand") {
		RuntimeWorlds limited(1);
		Renderer limitedRenderer;
		PortalImageSource limitedSource(limited.Universe, limitedRenderer, limited.Source, limited.Replies);
		PortalImageProducer limitedProducer(
			limited.Universe, limitedRenderer, limited.Destination, limited.Requests
		);
		REQUIRE(
			limitedSource.Issue(limited.Requests, first, Binding(), START).Status == PortalInboxStatus::Issued
		);
		CHECK(
			limitedSource.Issue(limited.Requests, latest, Binding(), START).Status == PortalInboxStatus::Busy
		);
		CHECK(limitedProducer.Pump(0, 0, START).Requests == 1);
		const std::array occupied{std::byte{0}};
		REQUIRE(
			limited.Universe.SendPresentation(
				limited.Source, limited.Replies, limited.Requests, 999, occupied
			) == world::PresentationStatus::Ok
		);
		REQUIRE(limitedSource.Poll(START).size() == 1);
		REQUIRE(limited.Universe.TakePresentation(limited.Requests).size() == 1);
		CHECK(limitedSource.Poll(START + std::chrono::milliseconds(1)).empty());
		const auto retried = limited.Universe.TakePresentation(limited.Requests);
		REQUIRE(retried.size() == 1);
		PortalImageRequest decoded;
		std::string error;
		REQUIRE(DecodePortalImageRequest(retried.front().Payload, decoded, error));
		CHECK(decoded.Geometry == latest.Geometry);
	}

	SECTION("deferred work expires while presentation pressure blocks retry") {
		RuntimeWorlds expiringWorlds(1);
		Renderer expiringRenderer;
		PortalInboxLimits limits;
		limits.Timeout = std::chrono::milliseconds(1);
		PortalImageSource expiring(
			expiringWorlds.Universe, expiringRenderer, expiringWorlds.Source, expiringWorlds.Replies, limits
		);
		REQUIRE(
			expiring.Issue(expiringWorlds.Requests, first, Binding(), START).Status ==
			PortalInboxStatus::Issued
		);
		CHECK(
			expiring.Issue(expiringWorlds.Requests, latest, Binding(), START).Status ==
			PortalInboxStatus::Busy
		);
		PortalImageProducer expiringProducer(
			expiringWorlds.Universe, expiringRenderer, expiringWorlds.Destination, expiringWorlds.Requests
		);
		CHECK(expiringProducer.Pump(0, 0, START).Requests == 1);
		const std::array occupied{std::byte{0}};
		REQUIRE(
			expiringWorlds.Universe.SendPresentation(
				expiringWorlds.Source, expiringWorlds.Replies, expiringWorlds.Requests, 999, occupied
			) == world::PresentationStatus::Ok
		);
		REQUIRE(expiring.Poll(START).size() == 1);
		CHECK(expiringWorlds.Universe.PresentationQueueUsage().Messages == 1);
		CHECK(expiring.Poll(START + limits.Timeout).empty());
		REQUIRE(expiringWorlds.Universe.TakePresentation(expiringWorlds.Requests).size() == 1);
		CHECK(expiring.Poll(START + std::chrono::milliseconds(2)).empty());
		CHECK(expiringWorlds.Universe.PresentationQueueUsage().Messages == 0);
	}
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

TEST_CASE("source clear retires its resident reservation", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalResidentImages images(renderer);
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies, {}, &images);
	auto request = Request();
	const auto issued = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(issued.Status == PortalInboxStatus::Issued);
	request.Key.RequestId = issued.RequestId;
	CHECK(images.Contains(worlds.Replies, worlds.Requests, request, START));

	source.Clear();
	CHECK_FALSE(images.Contains(worlds.Replies, worlds.Requests, request, START));
	source.Clear();
}

TEST_CASE("source rejects a resident table owned by another renderer", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer, other;
	PortalResidentImages images(other);
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies, {}, &images);
	const auto request = Request();
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Invalid);
	CHECK_FALSE(images.Contains(worlds.Replies, worlds.Requests, request, START));
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
