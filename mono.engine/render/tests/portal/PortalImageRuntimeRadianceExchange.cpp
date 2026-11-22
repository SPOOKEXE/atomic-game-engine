// Two-renderer destination radiance exchange through owned world presentation traffic.
#include "PortalImageRuntimeFixtures.hpp"

TEST_SUITE_ID("engine.render.portalimageruntimeradiance")
TEST_DEPENDS("engine.render.portalimageruntime")

TEST_CASE(
	"two renderers exchange destination radiance through owned world presentation traffic",
	"[slow][render][gpu][portal-runtime][.]"
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
		 replaceProducer = false, retirePending = false, nestedOrdered = false, shadowRoutes = false,
		 radianceChild = false;
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
	SECTION("copied parent accepts one seam with image and radiance children") {
		nestedWorld = complete = radianceChild = true;
		expectedRed = 0;
		expectedBlue = .5;
	}
	SECTION("ordered body capture retains a cross-world child depth domain") {
		nestedWorld = nestedOrdered = true;
	}
	SECTION("retained shadows follow the delivered tree's original requester chain") {
		nestedWorld = nestedOrdered = shadowRoutes = true;
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
	SECTION("portal seam keeps lit glass, spatial UI, particles and ribbons in one composed image") {
		// These inputs are normally exercised one at a time. Keep one combined
		// capture on the production exchange path so a layer can no longer be
		// accidentally omitted while every isolated fixture still passes.
		complete = glass = spatialOnly = particleOnly = beamOnly = true;
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
	// Presentation targets use the backend format, including headless BGRA UNORM.
	const auto captureDisplay = [](Renderer &renderer, uint32_t extent) {
		auto image = render::test::CaptureResource(
			renderer, core::Name("tonemapped"), 0, extent, extent, render::test::ImageFormat::Rgba8Unorm
		);
		const auto format = renderer.Backend().ColourFormat;
		if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
			format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
			for (uint32_t row = 0; row < extent; ++row)
				for (uint32_t column = 0; column < extent; ++column)
					std::swap(
						image.Bytes[size_t(row) * image.RowStrideBytes + column * 4],
						image.Bytes[size_t(row) * image.RowStrideBytes + column * 4 + 2]
					);
		}
		return image;
	};
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
	const std::array renderers{&sourceRenderer, &destinationRenderer};
	const size_t rendererCount = resident ? 1 : renderers.size();
	for (auto *renderer : std::span(renderers).first(rendererCount)) {
		REQUIRE(renderer->AddMesh(core::Name("runtime-plane"), mesh));
		REQUIRE(renderer->AddMesh(core::Name("runtime-plane"), mesh, worlds.Universe.NameOf(worlds.Source)));
		REQUIRE(
			renderer->AddMesh(core::Name("runtime-plane"), mesh, worlds.Universe.NameOf(worlds.Destination))
		);

		// Portal parts expose both faces. A single front-facing triangle sheet would
		// disappear when the reflected camera sees its back.
		auto apertureMesh = mesh;
		apertureMesh.Indices.insert(apertureMesh.Indices.end(), {2, 1, 0, 3, 2, 0});
		REQUIRE(renderer->AddMesh(core::Name("runtime-aperture"), apertureMesh));
		REQUIRE(renderer->AddMesh(
			core::Name("runtime-aperture"), apertureMesh, worlds.Universe.NameOf(worlds.Source)
		));
		REQUIRE(renderer->AddMesh(
			core::Name("runtime-aperture"), apertureMesh, worlds.Universe.NameOf(worlds.Destination)
		));
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
			REQUIRE(
				renderer->AddMesh(core::Name("runtime-body"), bodyMesh, worlds.Universe.NameOf(worlds.Source))
			);
			REQUIRE(renderer->AddMesh(
				core::Name("runtime-body"), bodyMesh, worlds.Universe.NameOf(worlds.Destination)
			));
		}
		REQUIRE(renderer->AddTexture(core::Name("runtime-white"), white));
		REQUIRE(
			renderer->AddTexture(core::Name("runtime-white"), white, worlds.Universe.NameOf(worlds.Source))
		);
		REQUIRE(renderer->AddTexture(
			core::Name("runtime-white"), white, worlds.Universe.NameOf(worlds.Destination)
		));
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
					DrawList list;
					list.Instances = {interpolated};
					target.SetResource(std::move(list));
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
			REQUIRE(effects::EmitParticles(store, emitter, 1));
			REQUIRE(effects::RefreshEmitters(store) == 1);
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
		} else {
			store.RemoveResource<scene::ActiveCamera>();
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
		{.Kind = graph::EditKind::Reads, .Target = core::Name("tonemapped"), .Key = core::Name("source")}
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
	request.OrderedLayers = nestedOrdered;
	if (shadowRoutes) {
		request.RetainedBodyPlayer = "91";
		producer.SetRetainedBodyAuthorization([&](const auto &requester, std::string_view player) {
			return requester == worlds.Replies && player == "91";
		});
	}
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
	if (radianceChild) request.PixelBudget += 2 * 128 * 128;
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
			store.RemoveResource<scene::ActiveCamera>();
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
			REQUIRE(demands.size() == 2);
			CHECK_FALSE(demands[0].SeamRadiance);
			CHECK(demands[1].SeamRadiance);
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
		request.PixelBudget = (shortSurfaceBudget ? 1 : 2) * captureExtent * captureExtent +
							  (radianceChild ? 2 * 128 * 128 : 0);
		// Neither node authors top GUI: four ordered captures per node cover this tree.
		if (nestedOrdered) request.PixelBudget = 2 * 4 * captureExtent * captureExtent;
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
	std::optional<world::PresentationAddress> nestedShadowRequester;
	if (shadowRoutes) {
		REQUIRE(childProducer);
		childProducer->SetRetainedBodyAuthorization([&](const auto &requester, std::string_view player) {
			if (requester.World != worlds.Requests.World || player != "91") return false;
			nestedShadowRequester = requester;
			return true;
		});
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
	std::string issueEncodingError;
	if (nestedOrdered && issue.Status != PortalInboxStatus::Issued) {
		auto diagnosticRequest = request;
		diagnosticRequest.Key.RequestId = 1;
		std::vector<std::byte> diagnosticWire;
		EncodePortalImageRequest(diagnosticRequest, diagnosticWire, issueEncodingError);
	}
	INFO(issueEncodingError);
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
		// This fixture exercises a later camera request explicitly. Return the
		// retained demand to the one already in flight so Poll does not dispatch a
		// second request before that assertion.
		REQUIRE(source.Issue(worlds.Requests, request, binding, started).Status == PortalInboxStatus::Busy);
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
	if (nestedOrdered) {
		// Retain each depth domain and its authored aperture for later body composition.
		SceneTarget target{captureExtent, captureExtent};
		View uploadView;
		uploadView.World = binding.World;
		uploadView.WorldName = binding.WorldName;
		uploadView.Slot = binding.ViewSlot;
		uploadView.Target = &target;
		OverlayImage overlay;
		std::vector<PortalRuntimeCompletion> completed;
		const auto deadline = started + std::chrono::seconds(10);
		while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
			const auto now = std::chrono::steady_clock::now();
			childProducer->Pump(0, 0, now);
			producer.Pump(0, 0, now);
			if (source.HasPendingUploads())
				sourceRenderer.Render(std::span(&uploadView, 1), overlay, nullptr, false);
			completed = source.Poll(now);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		INFO(completed.front().Diagnostic);
		REQUIRE(completed.front().Status == PortalImageStatus::Ok);
		const auto captured = source.Capture("Door");
		REQUIRE(captured);
		CHECK(captured->Image != 0);
		CHECK(captured->TransparentImages[0] != 0);
		CHECK(captured->TransparentImages[1] != 0);
		CHECK(captured->CaptureLighting.has_value());
		REQUIRE(captured->Tree != 0);
		const auto *tree = sourceRenderer.FindPortalCaptureTree(captured->Tree);
		REQUIRE(tree);
		REQUIRE(tree->Nodes.size() == 2);
		REQUIRE(tree->Edges.size() == 1);
		const auto &root = tree->Nodes[0];
		const auto &child = tree->Nodes[1];
		const auto childEndpoint = worlds.Universe.LookupPresentation(worlds.Source, PORTAL_REQUEST_CHANNEL);
		CHECK(
			root.Producer == PortalCaptureTreeEndpoint{
								 worlds.Requests.World,
								 worlds.Requests.Channel,
								 worlds.Requests.Session,
								 worlds.Requests.Generation
							 }
		);
		CHECK(
			child.Producer ==
			PortalCaptureTreeEndpoint{
				childEndpoint.World, childEndpoint.Channel, childEndpoint.Session, childEndpoint.Generation
			}
		);
		CHECK(root.Camera.Position == request.Position);
		CHECK(root.Camera.Orientation == request.Orientation);
		CHECK(root.Camera.Frustum == request.Frustum);
		CHECK(root.Camera.ClipPlane == request.ClipPlane);
		CHECK(root.Camera.Projection == request.Projection);
		CHECK(root.Images[0] == captured->Image);
		CHECK(root.Images[1] == captured->TransparentImages[0]);
		CHECK(root.Images[2] == captured->TransparentImages[1]);
		CHECK(child.Images[0] != 0);
		CHECK(child.Images[1] != 0);
		CHECK(child.Images[2] != 0);
		if (shadowRoutes) {
			const auto now = std::chrono::steady_clock::now();
			const auto &rootEye = root.Binding.Expected;
			const auto &childEye = child.Binding.Expected;
			const auto local =
				producer.ResolveShadowRoute(worlds.Replies, rootEye, root.Producer, rootEye, now);
			REQUIRE(local);
			CHECK(local->Requester == worlds.Replies);
			CHECK(local->Producer == worlds.Requests);
			CHECK(local->ParentEye == rootEye);
			REQUIRE(producer.TakeShadow(worlds.Replies, rootEye, now));
			const auto route =
				producer.ResolveShadowRoute(worlds.Replies, rootEye, child.Producer, childEye, now);
			REQUIRE(route);
			REQUIRE(nestedShadowRequester);
			CHECK(route->Requester == *nestedShadowRequester);
			CHECK(route->Requester != worlds.Replies);
			CHECK(route->Producer == childEndpoint);
			CHECK(route->ParentEye == childEye);
			auto wrongRequester = worlds.Replies;
			++wrongRequester.Generation;
			CHECK_FALSE(producer.ResolveShadowRoute(wrongRequester, rootEye, child.Producer, childEye, now));
			auto wrongEye = childEye;
			++wrongEye.CameraRevision;
			CHECK_FALSE(producer.ResolveShadowRoute(worlds.Replies, rootEye, child.Producer, wrongEye, now));
			CHECK_FALSE(producer.ResolveShadowRoute(worlds.Replies, wrongEye, child.Producer, childEye, now));
			auto wrongProducer = child.Producer;
			++wrongProducer.Generation;
			CHECK_FALSE(producer.ResolveShadowRoute(worlds.Replies, rootEye, wrongProducer, childEye, now));
			CHECK_FALSE(childProducer->TakeShadow(worlds.Replies, childEye, now));
			const auto leaf = childProducer->ResolveShadowRoute(
				route->Requester, route->ParentEye, child.Producer, childEye, now
			);
			REQUIRE(leaf);
			CHECK(leaf->Requester == route->Requester);
			CHECK(leaf->Producer == childEndpoint);
			const auto childShadow = childProducer->TakeShadow(leaf->Requester, childEye, now);
			REQUIRE(childShadow);
			CHECK(childShadow->Snapshot.Eye == childEye);
			CHECK(childShadow->Snapshot.Producer == child.Producer);
			CHECK(childShadow->Snapshot.ExcludedPlayer == "91");
			CHECK(producer.ResolveShadowRoute(worlds.Replies, rootEye, child.Producer, childEye, now)
					  .has_value());
		}
		const auto &edge = tree->Edges.front();
		CHECK(edge.Parent == 0);
		CHECK(edge.Child == 1);
		PortalGeometry aperture;
		std::string geometryError;
		REQUIRE(DecodePortalGeometry(edge.Geometry, aperture, geometryError));
		REQUIRE(aperture.Rows.size() == 1);
		CHECK(aperture.Rows.front().Assets[0] == "runtime-aperture");
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			std::vector<scene::PortalSeam> seams;
			scene::GatherPortalSeams(store, seams);
			REQUIRE(seams.size() == 1);
			const auto &seam = seams.front();
			const auto through = scene::SeamMapping(seam);
			CHECK(edge.Centre == std::array<float, 3>{seam.Centre.X, seam.Centre.Y, seam.Centre.Z});
			CHECK(edge.First == std::array<float, 3>{seam.First.X, seam.First.Y, seam.First.Z});
			CHECK(edge.Second == std::array<float, 3>{seam.Second.X, seam.Second.Y, seam.Second.Z});
			const auto position =
				through.Point({request.Position[0], request.Position[1], request.Position[2]});
			CHECK(
				(core::Vector3{child.Camera.Position[0], child.Camera.Position[1], child.Camera.Position[2]} -
				 position)
					.Magnitude() < .0001f
			);
			const auto mappedOrigin = through.Point({});
			CHECK(
				(core::Vector3{edge.Position[0], edge.Position[1], edge.Position[2]} - mappedOrigin)
					.Magnitude() < .0001f
			);
			CHECK(edge.Scale == through.Scale);
		});
		CHECK(destinationRenderer.PortalImageUsage().Images == 0);
		CHECK(destinationRenderer.PortalImageUsage().PendingCpuBytes == 0);
		auto limited = request;
		limited.Key.CameraRevision++;
		limited.PixelBudget = 4 * captureExtent * captureExtent;
		const auto refused =
			source.Issue(worlds.Requests, limited, binding, std::chrono::steady_clock::now());
		REQUIRE(refused.Status == PortalInboxStatus::Issued);
		completed.clear();
		const auto refusalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (completed.empty() && std::chrono::steady_clock::now() < refusalDeadline) {
			const auto now = std::chrono::steady_clock::now();
			childProducer->Pump(0, 0, now);
			producer.Pump(0, 0, now);
			completed = source.Poll(now);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		CHECK(completed.front().Status == PortalImageStatus::BudgetExceeded);
		REQUIRE(source.Capture("Door"));
		CHECK(source.Capture("Door")->Tree == captured->Tree);
		REQUIRE(worlds.Universe.ClosePresentation(childEndpoint) == world::PresentationStatus::Ok);
		if (shadowRoutes)
			CHECK_FALSE(producer.ResolveShadowRoute(
				worlds.Replies,
				root.Binding.Expected,
				child.Producer,
				child.Binding.Expected,
				std::chrono::steady_clock::now()
			));
		source.Poll(std::chrono::steady_clock::now());
		CHECK_FALSE(source.Capture("Door"));
		CHECK(source.Image("Door") == 0);
		CHECK_FALSE(sourceRenderer.FindPortalCaptureTree(captured->Tree));
		return;
	}
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
		CAPTURE(replaceNested, timeoutNested, moveNested, appearNested, seamChange, resident);
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
	if (radianceChild) {
		// The delayed parent must complete when one seam has image and radiance children.
		return;
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
	const auto image = captureDisplay(sourceRenderer, 65);
	const auto at = size_t(32) * image.RowStrideBytes + 32 * 4;
	const auto display = [&](double radiance) {
		const double aces = (radiance * (2.51 * radiance + .03)) / (radiance * (2.43 * radiance + .59) + .14);
		const double encoded = std::pow(aces, 1 / 2.2);
		const auto format = sourceRenderer.Backend().ColourFormat;
		const bool srgb = format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
						  format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		const double stored =
			srgb ? (encoded <= .0031308 ? 12.92 * encoded : 1.055 * std::pow(encoded, 1 / 2.4) - .055)
				 : encoded;
		return int(std::lround(stored * 255));
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
		const auto updated = captureDisplay(sourceRenderer, 65);
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
		const auto direct = captureDisplay(destinationRenderer, 32);
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
		const auto changedImage = captureDisplay(sourceRenderer, 65);
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
		const auto moving = captureDisplay(sourceRenderer, 65);
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
