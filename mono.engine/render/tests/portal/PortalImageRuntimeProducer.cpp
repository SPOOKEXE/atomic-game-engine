// Portal producer depth, retirement, primary body selection, lighting profile and layer capture tests.
#include "PortalImageRuntimeFixtures.hpp"

TEST_SUITE_ID("engine.render.portalimageruntimeproducer")
TEST_DEPENDS("engine.render.portalimageruntime")

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
	"producer terminal triggers cancel an in-flight copied capture once",
	"[render][gpu][portal-runtime][producer-retirement][.]"
) {
	const int trigger = GENERATE(0, 1, 2);
	CAPTURE(trigger);
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
	world::PresentationBindings replacement;
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
#if ENGINE_ASSERTS_ENABLED
	test_support::PortalRendererTerminalTrace terminals(fixture.Render);
#endif
	auto request = Request();
	request.Key.RequestId = 42;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 42, wire) ==
		world::PresentationStatus::Ok
	);
	REQUIRE(producer.Pump(0, 0, START).Rendered == 1);
	CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());
	if (trigger == 0) {
		producer.Clear();
	} else if (trigger == 1) {
		producer.SetEndpointBindings(&replacement);
		producer.SetEndpointBindings(&replacement);
	} else {
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
		// Endpoint retirement is owned by the host, which clears the producer
		// after closing its request channel.
		producer.Clear();
	}
#if ENGINE_ASSERTS_ENABLED
	std::vector<uint64_t> retired;
	for (const auto &call : terminals.Calls)
		if (call.Kind == test_support::PortalRendererTerminalKind::CancelResourceImage && call.Token != 0 &&
			call.Applied)
			retired.push_back(call.Token);
	REQUIRE(retired.size() == 1);
	CHECK(
		terminals.Count(test_support::PortalRendererTerminalKind::CancelResourceImage, retired.front()) == 1
	);
#endif
	producer.Clear();
	CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());
#if ENGINE_ASSERTS_ENABLED
	CHECK(
		terminals.Count(test_support::PortalRendererTerminalKind::CancelResourceImage, retired.front()) == 1
	);
#endif
}

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"producer timeout cancels a queued capture left pending by acquisition refusal",
	"[render][gpu][portal-runtime][producer-retirement][.]"
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
#if ENGINE_ASSERTS_ENABLED
	test_support::PortalRendererTerminalTrace terminals(fixture.Render);
#endif
	auto request = Request();
	request.Key.RequestId = 43;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 43, wire) ==
		world::PresentationStatus::Ok
	);
	struct ResetAcquisitionFailure {
		~ResetAcquisitionFailure() {
			render::test_support::SetFrameBatchAcquisitionFailureForTests(false);
		}
	} reset;
	render::test_support::SetFrameBatchAcquisitionFailureForTests(true);
	REQUIRE(producer.Pump(0, 0, START).Rendered == 1);
	CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());

	const auto expired = producer.Pump(0, 0, START + std::chrono::seconds(2));
	CHECK(expired.Rendered == 0);
	CHECK(expired.Sent == 1);
	const auto replies = worlds.Universe.TakePresentation(worlds.Replies);
	REQUIRE(replies.size() == 1);
	PortalImageReply reply;
	REQUIRE(DecodePortalImageReply(replies.front().Payload, reply, error));
	CHECK(reply.Key == request.Key);
	CHECK(reply.Status == PortalImageStatus::Failed);
	CHECK(reply.Diagnostic == "destination capture expired");
#if ENGINE_ASSERTS_ENABLED
	std::vector<uint64_t> retired;
	for (const auto &call : terminals.Calls)
		if (call.Kind == test_support::PortalRendererTerminalKind::CancelResourceImage && call.Token != 0 &&
			call.Applied)
			retired.push_back(call.Token);
	REQUIRE(retired.size() == 1);
	CHECK(
		terminals.Count(test_support::PortalRendererTerminalKind::CancelResourceImage, retired.front()) == 1
	);
#endif
	producer.Clear();
#if ENGINE_ASSERTS_ENABLED
	CHECK(
		terminals.Count(test_support::PortalRendererTerminalKind::CancelResourceImage, retired.front()) == 1
	);
#endif
	CHECK(worlds.Universe.TakePresentation(worlds.Replies).empty());
}
#endif

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"producer clear cancels an in-flight retained shadow fit once",
	"[render][gpu][portal-runtime][producer-retirement][.]"
) {
	RuntimeWorlds worlds;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) { scene::InstallServices(store); });
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	producer.SetRetainedBodyAuthorization([&](const auto &requester, std::string_view player) {
		return requester == worlds.Replies && player == "91";
	});
	test_support::PortalRendererTerminalTrace terminals(fixture.Render);
	auto request = Request();
	request.Key.RequestId = 44;
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.RetainedBodyPlayer = "91";
	request.Projection = PortalImageProjection::Eye;
	request.ClipPlane = {};
	request.PixelBudget = 4 * request.Width * request.Height;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 44, wire) ==
		world::PresentationStatus::Ok
	);
	REQUIRE(producer.Pump(0, 1, START).Rendered == 1);
	std::vector<world::PresentationMessage> completed;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
		completed = worlds.Universe.TakePresentation(worlds.Replies);
		if (completed.empty()) {
			producer.Pump(0, 1, START);
			SDL_Delay(1);
		}
	}
	REQUIRE(completed.size() == 1);
	PortalImageLayerSet layers;
	REQUIRE(DecodePortalImageLayerSet(completed.front().Payload, layers, error));
	REQUIRE(layers.Opaque.Status == PortalImageStatus::Ok);
	REQUIRE(producer.ShadowUsage().Ready == 1);

	// The completed eye owns a route. A fitted pull adds one pending renderer token.
	PortalShadowPull pull;
	pull.ParentEye = request.Key;
	pull.TargetProducer = {
		worlds.Requests.World, worlds.Requests.Channel, worlds.Requests.Session, worlds.Requests.Generation
	};
	pull.TargetEye = request.Key;
	pull.BodyBounds = core::AABB{{-1, -1, -5}, {1, 1, -3}};
	REQUIRE(EncodePortalShadowPull(pull, wire, error));
	REQUIRE(
		worlds.Universe.SendPresentation(worlds.Source, worlds.Replies, worlds.Requests, 45, wire) ==
		world::PresentationStatus::Ok
	);
	struct ResetAcquisitionFailure {
		~ResetAcquisitionFailure() {
			render::test_support::SetFrameBatchAcquisitionFailureForTests(false);
		}
	} reset;
	render::test_support::SetFrameBatchAcquisitionFailureForTests(true);
	producer.Pump(0, 1, START);
	REQUIRE(producer.HasPendingShadowFits());
	const size_t before = terminals.Calls.size();
	producer.Clear();
	CHECK_FALSE(producer.HasPendingShadowFits());
	std::vector<uint64_t> retired;
	for (const auto &call : std::span(terminals.Calls).subspan(before))
		if (call.Kind == test_support::PortalRendererTerminalKind::CancelResourceImage && call.Token != 0 &&
			call.Applied)
			retired.push_back(call.Token);
	REQUIRE(retired.size() == 1);
	CHECK(
		terminals.Count(test_support::PortalRendererTerminalKind::CancelResourceImage, retired.front()) == 1
	);
	producer.Clear();
	CHECK(
		terminals.Count(test_support::PortalRendererTerminalKind::CancelResourceImage, retired.front()) == 1
	);
}
#endif

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
	for (const int captureMode : {0, 1, 2}) {
		const auto scope =
			captureMode == 1 ? PortalImageScope::CompleteWorld : PortalImageScope::OpaqueLighting;
		const bool ordered = captureMode == 2;
		CAPTURE(scope, ordered);
		std::optional<PortalImageReply> original;
		for (const bool enabled : {false, true, false}) {
			worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
				store.GetMutable<scene::ShaderLens>(lens)->Enabled = enabled;
			});
			worlds.Universe.Tick(.01f);
			auto request = Request();
			request.Key.RequestId = ++sequence;
			request.Scope = scope;
			request.OrderedLayers = ordered;
			request.Width = request.Height = 65;
			request.PixelBudget = (ordered ? 4 : 1) * 65 * 65;
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
			if (ordered) {
				PortalImageLayerSet layers;
				REQUIRE(DecodePortalImageLayerSet(replies[0].Payload, layers, error));
				CHECK(layers.Lenses.Entries.size() == size_t(enabled ? 1 : 0));
				REQUIRE(layers.Lenses.Programs.size() == size_t(enabled ? 1 : 0));
				if (enabled) {
					const auto &captured = layers.Lenses.Entries.front();
					CHECK(layers.Lenses.Programs.front().Hash == captured.ProgramHash);
					CHECK_FALSE(layers.Lenses.Programs.front().SpirV.empty());
					CHECK(captured.Shader == "gravitational-lens");
					CHECK(
						captured.ProgramHash ==
						fixture.Render.LensShaderHash(
							core::Name(captured.Shader), worlds.Universe.NameOf(worlds.Destination)
						)
					);
					CHECK(captured.Position == std::array<float, 3>{0, 0, -2});
					CHECK(captured.Radius == 2);
					CHECK(captured.InnerRadius == .5f);
					CHECK(captured.Strength == 2);
				}
				reply = std::move(layers.Opaque);
			} else {
				REQUIRE(DecodePortalImageReply(replies[0].Payload, reply, error));
			}
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
	const int layerMode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
	const bool spatialLayers = layerMode != 0;
	const bool topLayer = layerMode >= 6;
	CAPTURE(layerMode);
	gui::RegisterGuiClasses();
	RuntimeWorlds worlds(1);
	scene::RegisterSceneClasses();
	RegisterPresentationComponents();
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		scene::InstallServices(store);
		if (spatialLayers) {
			for (const bool red : {false, true}) {
				const auto shader =
					store.CreateInstance(scene::ShaderScriptClass(), red ? "layer.red" : "layer.green");
				std::string source = "#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){";
				if (red && (layerMode == 2 || layerMode == 8)) source += "if(gl_FragCoord.x<8)discard;";
				if (red && (layerMode == 3 || layerMode == 9))
					source += "if(gl_FragCoord.x<8){colour=vec4(0);return;}";
				source += red ? "colour=vec4(16,0,0,128.0/255.0);}" : "colour=vec4(0,16,0,128.0/255.0);}";
				REQUIRE(scene::SetShaderSource(store, shader, source));
			}
		}
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
	REQUIRE(fixture.Render.AddMesh(mesh, plane, worlds.Universe.NameOf(worlds.Destination)));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(fixture.Render.AddTexture(emission, white, worlds.Universe.NameOf(worlds.Destination)));
	PortalImageProducer producer(worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests);
	PortalImageInbox inbox;
	const auto endpoint = [](const world::PresentationAddress &address) {
		return PortalEndpointView{address.World, address.Channel, address.Session, address.Generation};
	};
	uint64_t sequence = 0;
	std::vector<ecs::Entity> canvases;
	// Recovery follows overflow on the same producer, exercising capture retirement.
	for (const int panes : {0, 1, 2, 3, -3, 2, 0}) {
		if (topLayer && panes != 0 && panes != 1) continue;
		CAPTURE(panes, sequence);
		worlds.Universe.Tick(.01f);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			auto &rows = store.ResourceMutable<DrawList>()->Instances;
			rows.clear();
			for (const auto entity : canvases)
				store.Destroy(entity);
			canvases.clear();
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
				if (!spatialLayers || index == 0 || (layerMode == 4 && index % 2 == 0) ||
					(layerMode == 5 && index % 2 != 0)) {
					rows.push_back(row);
					continue;
				}
				scene::PartDesc part;
				part.Frame = row.Frame;
				part.Size = {16, 16, .0002f};
				part.Simulated = false;
				const auto anchor = scene::MakePart(store, part);
				REQUIRE(store.SetParent(anchor, store.FindFirstRoot("Workspace")));
				canvases.push_back(anchor);
				const auto canvas = store.CreateInstance(gui::GuiClass("SurfaceGui"), "Layer");
				REQUIRE(store.SetParent(canvas, anchor));
				gui::Surface surface;
				surface.On = gui::Face::Back;
				surface.AlwaysOnTop = topLayer;
				surface.CanvasSize = {100, 100};
				store.Set(canvas, surface);
				const auto label = store.CreateInstance(gui::GuiClass("ImageLabel"), "Colour");
				REQUIRE(store.SetParent(label, canvas));
				gui::Element element;
				element.Size = {1, 0, 1, 0};
				store.Set(label, element);
				gui::Background background;
				background.Transparency = 1;
				background.BorderSizePixel = 0;
				store.Set(label, background);
				gui::Picture picture;
				picture.Image = emission;
				picture.Shader = core::Name(index == 1 ? "layer.red" : "layer.green");
				store.Set(label, picture);
			}
		});
		auto request = Request();
		request.Key.RequestId = ++sequence;
		request.Scope = PortalImageScope::OpaqueLighting;
		request.OrderedLayers = true;
		request.Projection = PortalImageProjection::Eye;
		request.ClipPlane = {};
		request.Width = request.Height = 17;
		request.PixelBudget = (topLayer && layerMode != 7 ? 5 : 4) * 17 * 17;
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
		if (sequence == 3 && !topLayer) {
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
		if (panes == 3 || (layerMode == 7 && panes == 1)) {
			CHECK(accepted.Status == PortalInboxStatus::CompletedFailure);
			CHECK(inbox.Usage().HeldCount == 0);
			PortalImageReply failed;
			REQUIRE(DecodePortalImageReply(replies[0].Payload, failed, error));
			CHECK(failed.Key == request.Key);
			CHECK(failed.Status == PortalImageStatus::BudgetExceeded);
			CHECK(
				failed.Diagnostic == (layerMode == 7
										  ? "destination spatial overlay exceeds capture pixel budget"
										  : "destination transparency exceeds two ordered layers")
			);
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
		CHECK(layers.Opaque.Normal.size() == size_t(request.Width) * request.Height * 4);
		CHECK(layers.Opaque.AmbientResponse.size() == size_t(request.Width) * request.Height * 16);
		CHECK(layers.Opaque.LightingBaseline.size() == size_t(request.Width) * request.Height * 16);
		CHECK(layers.Opaque.DirectionalResponse.size() == size_t(request.Width) * request.Height * 16);
		CHECK(assets::Hasher::Of(layers.Opaque.DirectionalResponse) == layers.Opaque.DirectionalResponseHash);
		REQUIRE(layers.Transparent.size() == 2);
		for (const auto &transparent : layers.Transparent) {
			CHECK(transparent.Normal.empty());
			CHECK(transparent.AmbientResponse.empty());
			CHECK(transparent.LightingBaseline.empty());
			CHECK(transparent.DirectionalResponse.empty());
		}
		CHECK(layers.SpatialOverlay.has_value() == (topLayer && panes == 1));
		if (layers.SpatialOverlay) {
			CHECK(layers.SpatialOverlay->Depth.empty());
			CHECK(layers.SpatialOverlay->DirectionalResponse.empty());
			core::ByteReader overlay(layers.SpatialOverlay->Pixels);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto rg = glm::unpackHalf2x16(overlay.ReadUInt32());
				const auto ba = glm::unpackHalf2x16(overlay.ReadUInt32());
				const float alpha = layerMode >= 8 && pixel % 17 < 8 ? 0.f : 128.f / 255;
				CHECK(std::abs(rg.x - 16.f * alpha) < .01f);
				CHECK(rg.y == 0);
				CHECK(ba.x == 0);
				CHECK(std::abs(ba.y - alpha) < .001f);
			}
		}
		for (size_t layer = 0; layer < 3; ++layer) {
			const auto &image = layer == 0 ? layers.Opaque : layers.Transparent[layer - 1];
			core::ByteReader pixels(image.Pixels), depths(image.Depth);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto planeIndex =
					layer + (layer != 0 && (layerMode == 2 || layerMode == 3) && pixel % 17 < 8 ? 1 : 0);
				const bool occupied = layer == 0 || (!topLayer && int(planeIndex) <= std::abs(panes) &&
													 (panes >= 0 || planeIndex < 3));
				const auto rg = glm::unpackHalf2x16(pixels.ReadUInt32());
				const auto ba = glm::unpackHalf2x16(pixels.ReadUInt32());
				const auto depth = depths.ReadFloat();
				CHECK(
					std::abs(
						depth - (occupied ? (layer == 0 ? (panes < 0 ? 3.5f : 6.f) : 1.f + planeIndex) : 0.f)
					) < .003f
				);
				if (!occupied) {
					CHECK(rg == glm::vec2{});
					CHECK(ba == glm::vec2{});
				} else {
					CHECK(std::abs(ba.y - (layer == 0 ? 1.f : 128.f / 255)) < .001f);
					CHECK((layer == 0 ? ba.x : planeIndex == 1 ? rg.x : rg.y) > 7.f);
				}
			}
		}
	}
}

TEST_CASE(
	"portal source publishes layer groups only after upload", "[render][gpu][portal-runtime][layer-source][.]"
) {
	const bool overlayFirst = GENERATE(false, true);
	RuntimeWorlds worlds;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
#if ENGINE_ASSERTS_ENABLED
	test_support::PortalRendererTerminalTrace rendererTerminals(fixture.Render);
#endif
	std::ifstream programFile(
		resources::Shader("gravitational-lens.frag", resources::ShaderForm::SpirV),
		std::ios::binary | std::ios::ate
	);
	REQUIRE(programFile);
	const auto programSize = programFile.tellg();
	REQUIRE(programSize > 0);
	REQUIRE(static_cast<size_t>(programSize) % sizeof(uint32_t) == 0);
	std::vector<uint32_t> lensWords(static_cast<size_t>(programSize) / sizeof(uint32_t));
	programFile.seekg(0);
	programFile.read(reinterpret_cast<char *>(lensWords.data()), programSize);
	REQUIRE(programFile);
	const auto lensHash = assets::Hasher::Of(std::as_bytes(std::span(lensWords)));
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
		if (ordered && ((revision % 2 != 0) == overlayFirst)) {
			layers.SpatialOverlay = opaque;
			layers.SpatialOverlay->Depth.clear();
			layers.SpatialOverlay->DepthHash = {};
		}
		if (ordered && ((revision % 2 != 0) != overlayFirst)) {
			layers.Lenses.TimeSeconds = static_cast<float>(revision);
			PortalCaptureLens lens;
			lens.Shader = "captured.lens";
			lens.ProgramHash = lensHash;
			lens.Spin = static_cast<float>(revision);
			layers.Lenses.Entries.push_back(std::move(lens));
			layers.Lenses.Programs.push_back({lensHash, lensWords});
		}
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
					.Ran(core::Name("surface-capture")));
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
	// Return to the in-flight camera so this case isolates upload readiness.
	// Otherwise latest-demand coalescing correctly issues position 1 after upload.
	CHECK(issue(0).Status == PortalInboxStatus::Busy);
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
	CHECK((original->SpatialOverlayImage != 0) == overlayFirst);
	CHECK(original->Lenses.Entries.empty() == overlayFirst);
	CHECK((original->LensPrograms == 0) == overlayFirst);
	CHECK(original->Lenses.Programs.empty());
	CHECK(original->Lenses.TimeSeconds == (overlayFirst ? 0.f : 1.f));
	const std::array originalHandles{
		original->Image,
		original->TransparentImages[0],
		original->TransparentImages[1],
		original->SpatialOverlayImage
	};
	CHECK(source.CurrentImage("Door") == original->Image);
	CHECK(fixture.Render.PortalImageUsage().Images == size_t{3} + overlayFirst);
	const auto second = issue(3);
	REQUIRE(second.Status == PortalInboxStatus::Issued);
	deliver(second.RequestId, 2);
	CHECK(source.Poll(START).empty());
	CHECK(source.HasPendingUploads());
	CHECK(source.Image("Door") == original->Image);
	CHECK(source.CurrentImage("Door") == 0);
	CHECK(source.Capture("Door")->Camera.Position == original->Camera.Position);
	CHECK(source.Capture("Door")->SpatialOverlayImage == original->SpatialOverlayImage);
	CHECK(source.Capture("Door")->Lenses == original->Lenses);
	CHECK(fixture.Render.PortalImageUsage().Images == 7);
	upload();
	CHECK(source.Image("Door") == original->Image);
	const auto replaced = source.Poll(START);
	REQUIRE(replaced.size() == 1);
	CHECK(replaced[0].RequestId == second.RequestId);
	const auto current = source.Capture("Door");
	REQUIRE(current);
	CHECK(current->Image != original->Image);
	CHECK(current->Camera.Position[0] == 3);
	CHECK((current->SpatialOverlayImage != 0) != overlayFirst);
	CHECK(current->Lenses.Entries.empty() != overlayFirst);
	CHECK((current->LensPrograms == 0) != overlayFirst);
	CHECK(current->Lenses.Programs.empty());
	CHECK(current->Lenses.TimeSeconds == (overlayFirst ? 2.f : 0.f));
	const std::array currentHandles{
		current->Image,
		current->TransparentImages[0],
		current->TransparentImages[1],
		current->SpatialOverlayImage
	};
	CHECK(fixture.Render.PortalImageUsage().Images == size_t{3} + !overlayFirst);
	CHECK_FALSE(fixture.Render.DropPortalImage(original->Image));
#if ENGINE_ASSERTS_ENABLED
	for (const uint64_t handle : originalHandles)
		if (handle != 0)
			CHECK(
				rendererTerminals.AppliedCount(
					test_support::PortalRendererTerminalKind::ReleasePortalImport, handle
				) == 1
			);
	CHECK(
		rendererTerminals.AppliedCount(
			test_support::PortalRendererTerminalKind::DropPortalImage, original->Image
		) == 1
	);
#endif
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
		CHECK(source.Capture("Door")->SpatialOverlayImage == current->SpatialOverlayImage);
		CHECK(source.Capture("Door")->Lenses == current->Lenses);
		CHECK(fixture.Render.PortalImageUsage().Images == size_t{3} + !overlayFirst);
		CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	}
	const auto flattened = issue(6, false);
	REQUIRE(flattened.Status == PortalInboxStatus::Issued);
	deliver(flattened.RequestId, 4, false);
	const auto flatCompletion = source.Poll(START);
	REQUIRE(flatCompletion.size() == 1);
	CHECK(flatCompletion[0].Status == PortalImageStatus::Ok);
	CHECK(source.Capture("Door")->TransparentImages == std::array<uint64_t, 2>{});
	CHECK(source.Capture("Door")->SpatialOverlayImage == 0);
	CHECK(source.Capture("Door")->Lenses == PortalCaptureLenses{});
	CHECK(fixture.Render.PortalImageUsage().Images == 1);
	CHECK_FALSE(fixture.Render.DropPortalImage(current->Image));
#if ENGINE_ASSERTS_ENABLED
	for (const uint64_t handle : currentHandles)
		if (handle != 0)
			CHECK(
				rendererTerminals.AppliedCount(
					test_support::PortalRendererTerminalKind::ReleasePortalImport, handle
				) == 1
			);
	CHECK(
		rendererTerminals.AppliedCount(
			test_support::PortalRendererTerminalKind::DropPortalImage, current->Image
		) == 1
	);
#endif
	const uint64_t flatImage = source.Capture("Door")->Image;
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
#if ENGINE_ASSERTS_ENABLED
	CHECK(
		rendererTerminals.AppliedCount(
			test_support::PortalRendererTerminalKind::ReleasePortalImport, flatImage
		) == 1
	);
	CHECK(
		rendererTerminals.AppliedCount(
			test_support::PortalRendererTerminalKind::DropPortalImage, flatImage
		) == 1
	);
#endif
}
