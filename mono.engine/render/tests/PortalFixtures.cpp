// A translated doorway must show the same rays as the destination unfolded
// into this room. The reference has no PortalView or renderer warp calculation.

#include "RenderFixture.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/testing/Suite.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>

TEST_SUITE_ID("engine.render.portalfixtures")
TEST_DEPENDS("engine.render.imagecomparison")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {

	using namespace engine;
	using namespace engine::render::test;

	constexpr uint32_t WIDTH = 129;
	constexpr uint32_t HEIGHT = 97;
	constexpr float HALF_WIDTH = 1.7f;
	constexpr float HALF_HEIGHT = 1.4f;
	constexpr float DESTINATION_X = 100;

	assets::MeshData DoorwayPlane() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
			{{-0.5f, -0.5f, 0}, {0, 0, -1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, -1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, -1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, -1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6};
		mesh.ComputeBounds();
		return mesh;
	}

	scene::DrawInstance
	Plane(uint32_t source, core::Vector3 position, float halfWidth, float halfHeight, core::Color3 tint) {
		scene::DrawInstance instance;
		instance.Source = source;
		instance.Frame.Position = position;
		instance.HalfExtent = {halfWidth, halfHeight, 0.01f};
		instance.Tint = tint;
		instance.Mesh = core::Name("portal.fixture.plane");
		instance.Texture = core::Name("portal.fixture.white");
		instance.CastShadow = false;
		return instance;
	}

	graph::PipelineDocument InstallPortalFixture(render::Renderer &renderer) {
		auto document = graph::DefaultPbrDocument();
		const core::Name captureKind("portal-fixture-capture-boundary");
		graph::NodeKindSpec capture;
		capture.Kind = captureKind;
		capture.Scope = graph::NodeScope::Frame;
		capture.Queue = graph::ExecutionQueue::Cpu;
		capture.Category = graph::NodeCategory::Output;
		for (const auto resource : {"tonemapped", "portaled", "composed-image"}) {
			capture.Inputs.push_back({.Name = core::Name(resource), .Kind = graph::ResourceKind::Texture});
		}
		REQUIRE(graph::RegisterNodeKind(std::move(capture)));
		REQUIRE(renderer.InstallNodeHandler(captureKind, [](const graph::RunContext &) { return true; }));
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = captureKind,
			 .NodeKind = captureKind,
			 .Scope = graph::NodeScope::Frame}
		);
		for (const auto resource : {"tonemapped", "portaled", "composed-image"}) {
			document.Record(
				{.Kind = graph::EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(resource)}
			);
		}
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("portal.fixture.pbr"), graph));
		REQUIRE(renderer.AddMesh(core::Name("portal.fixture.plane"), DoorwayPlane()));
		assets::TextureData white;
		white.Width = 1;
		white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(core::Name("portal.fixture.white"), white));
		renderer.SetPortalDepth(1);
		return document;
	}

	struct CameraSample {
		std::string Name;
		core::Vector3 Eye;
		core::Vector3 Aim{};
		float Roll = 0;
		size_t MinimumInterior = 51;
		size_t MinimumExterior = 101;
	};

	// Pixel-centre pinhole rays intersect the authored z=0 rectangle. The
	// fixed geometric guard excludes raster edges and the destination's colour
	// discontinuities, where filtering is implementation dependent. It never
	// inspects the actual image to decide which pixels count.
	void CheckOpening(
		render::Renderer &renderer,
		const CameraSample &sample,
		float ambient,
		const graph::PipelineDocument &document,
		const render::View &view,
		float destinationZ,
		const CapturedImage &before,
		const CapturedImage &unfolded,
		const CapturedImage &actual,
		std::span<const scene::DrawInstance> destination,
		std::span<const scene::DrawInstance> referenceInstances
	) {
		std::vector<uint32_t> expectedPixels(WIDTH * HEIGHT);
		std::vector<uint32_t> actualPixels(WIDTH * HEIGHT);
		size_t inside = 0;
		size_t outside = 0;
		size_t coloured = 0;
		const double tangent = std::tan(view.Camera.FieldOfViewRadians / 2.0);
		for (uint32_t y = 0; y < HEIGHT; y++) {
			for (uint32_t x = 0; x < WIDTH; x++) {
				const core::Vector3 local{
					static_cast<float>((2 * (x + 0.5) / WIDTH - 1) * tangent * WIDTH / HEIGHT),
					static_cast<float>((1 - 2 * (y + 0.5) / HEIGHT) * tangent),
					-1,
				};
				const auto ray = view.CameraFrame.VectorToWorldSpace(local);
				if (std::abs(ray.Z) < 1e-6f) {
					continue;
				}
				const float distance = -sample.Eye.Z / ray.Z;
				const auto aperture = sample.Eye + ray * distance;
				const bool inOpening =
					distance > 0 && std::abs(aperture.X) < HALF_WIDTH && std::abs(aperture.Y) < HALF_HEIGHT;
				if (distance > 0 && (std::abs(std::abs(aperture.X) - HALF_WIDTH) < 0.08f ||
									 std::abs(std::abs(aperture.Y) - HALF_HEIGHT) < 0.08f)) {
					continue;
				}
				if (inOpening) {
					const float farDistance = (destinationZ - sample.Eye.Z) / ray.Z;
					const auto far = sample.Eye + ray * farDistance;
					if (std::abs(far.X) < 0.15f || std::abs(far.X) > 19.0f || std::abs(far.Y) > 19.0f) {
						continue;
					}
					inside++;
				} else {
					outside++;
				}
				const auto &reference = inOpening ? unfolded : before;
				const auto *referencePixel = reference.Bytes.data() + y * reference.RowStrideBytes + x * 4;
				if (inOpening &&
					std::abs(
						std::to_integer<int>(referencePixel[0]) - std::to_integer<int>(referencePixel[2])
					) > 30) {
					coloured++;
				}
				const size_t pixel = static_cast<size_t>(y) * WIDTH + x;
				std::memcpy(
					&expectedPixels[pixel], reference.Bytes.data() + y * reference.RowStrideBytes + x * 4, 4
				);
				std::memcpy(&actualPixels[pixel], actual.Bytes.data() + y * actual.RowStrideBytes + x * 4, 4);
			}
		}
		INFO(sample.Name << " inside=" << inside << " outside=" << outside);
		REQUIRE(inside >= sample.MinimumInterior);
		REQUIRE(outside >= sample.MinimumExterior);
		REQUIRE(coloured >= sample.MinimumInterior);
		std::ostringstream inputs;
		inputs << "camera=" << sample.Name << " eye=" << sample.Eye.X << ',' << sample.Eye.Y << ','
			   << sample.Eye.Z << " ambient=" << ambient << " destination-x=" << DESTINATION_X
			   << " destination-z=" << destinationZ << " aperture=" << HALF_WIDTH << ',' << HALF_HEIGHT
			   << " inside=" << inside << " outside=" << outside << " seed=0 samples=1\n"
			   << "fov=" << view.Camera.FieldOfViewRadians << " near=" << view.Camera.NearPlane
			   << " far=" << view.Camera.FarPlane << " direct=0 outdoor-ambient=0\n"
			   << "aperture-edge-guard=.08 colour-edge-guard=.15 far-bound=19\n"
			   << "masked-out pixels are zero in both comparisons\n"
			   << graph::Write(document);
		for (const auto &[name, instances] :
			 {std::pair{"destination", destination}, std::pair{"reference", referenceInstances}}) {
			for (const auto &instance : instances) {
				inputs << "\n"
					   << name << " source=" << instance.Source << " position=" << instance.Frame.Position.X
					   << ',' << instance.Frame.Position.Y << ',' << instance.Frame.Position.Z
					   << " extent=" << instance.HalfExtent.X << ',' << instance.HalfExtent.Y
					   << " tint=" << instance.Tint.R << ',' << instance.Tint.G << ',' << instance.Tint.B
					   << " transparency=" << instance.Transparency << " shader=" << instance.Shader.Text();
			}
		}
		for (const auto &light : view.Lights) {
			inputs << "\nlight position=" << light.Position.X << ',' << light.Position.Y << ','
				   << light.Position.Z << " range=" << light.Range << " colour=" << light.Colour.R << ','
				   << light.Colour.G << ',' << light.Colour.B;
		}
		inputs << "\nUI variants replace left destination with a full-span rectangle, fullbright=ambient\n";
		for (const char *name :
			 {"opaque.frag.spv", "tonemap.frag.spv", "interface_spatial.vert.spv", "interface.frag.spv"}) {
			const auto path = std::filesystem::path(SDL_GetBasePath()) / "shaders/resources" / name;
			std::ifstream shader(path, std::ios::binary);
			REQUIRE(shader.good());
			const std::vector<char> bytes{
				std::istreambuf_iterator<char>(shader), std::istreambuf_iterator<char>()
			};
			inputs << "\nshader=" << name << ' '
				   << assets::Hasher::Of(std::as_bytes(std::span(bytes))).ToHex();
		}
		// Portal radiance must retain enough precision to stay within two
		// display codes of the direct view. This catches UNORM8 radiance loss:
		// the packed .1 tint under .25 ambient lands on a half-code boundary
		// before tonemapping, where a round trip can exceed this budget.
		ImageTolerance tolerance;
		tolerance.Absolute = 2.0 / 255;
		const ImageView expected{
			WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(expectedPixels))
		};
		const ImageView observed{
			WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(actualPixels))
		};
		const auto comparison = CompareImages(expected, observed, tolerance);
		std::cout << "portal " << sample.Name << " ambient=" << ambient << " inside=" << inside
				  << " outside=" << outside << " mismatched=" << comparison.MismatchedPixels
				  << " max=" << comparison.MaximumAbsoluteError << " rmse=" << comparison.RootMeanSquareError
				  << '\n';
		CheckImage(
			renderer,
			std::string("portal-") + sample.Name + "-" + std::to_string(ambient),
			"portaled",
			inputs.str(),
			expected,
			observed,
			tolerance
		);
	}
}

TEST_CASE(
	"portal pixels match an unfolded room through both faces and oblique views",
	"[render][gpu][fixture][portal][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallPortalFixture(fixture.Render);
	const render::SceneTarget target{WIDTH, HEIGHT};
	std::vector<CameraSample> samples{
		CameraSample{"front", {0, 0, 4}},
		CameraSample{"back", {0, 0, -4}},
		CameraSample{"off-centre", {1.1f, 0.6f, 4}},
		CameraSample{"grazing", {4, 0.3f, 0.8f}},
	};
	for (const float elevation : {-50.0f, 0.0f, 50.0f}) {
		for (const float azimuth :
			 {-165.0f,
			  -135.0f,
			  -105.0f,
			  -75.0f,
			  -45.0f,
			  -15.0f,
			  15.0f,
			  45.0f,
			  75.0f,
			  105.0f,
			  135.0f,
			  165.0f}) {
			constexpr float radians = 0.01745329252f;
			const float horizontal = 4 * std::cos(elevation * radians);
			samples.push_back(
				{"azimuth-" + std::to_string(azimuth) + "-elevation-" + std::to_string(elevation),
				 {horizontal * std::sin(azimuth * radians),
				  4 * std::sin(elevation * radians),
				  horizontal * std::cos(azimuth * radians)}}
			);
		}
	}
	samples.push_back({"rolled-off-centre", {1.1f, -0.6f, 4}, {0.3f, 0.2f, 0}, 0.7f});
	samples.push_back({"near-plane-front", {0, 0, 0.05f}, {}, 0, 51, 0});
	samples.push_back({"near-plane-back", {0, 0, -0.05f}, {}, 0, 51, 0});
	samples.push_back({"look-away-front", {0, 0, 4}, {0, 0, 8}, 0, 0, 101});
	samples.push_back({"look-away-back", {0, 0, -4}, {0, 0, -8}, 0, 0, 101});
	for (const auto &sample : samples) {
		for (const float ambient : {0.25f, 0.75f, 2.0f}) {
			if (ambient > 1 && std::string_view(sample.Name) != "front") {
				continue;
			}
			for (const std::string_view appearance :
				 {"material", "custom", "depth-ui", "top-ui", "transparent"}) {
				if (appearance != "material" && ambient != 2.0f) {
					continue;
				}
				DYNAMIC_SECTION(sample.Name << " ambient=" << ambient << " appearance=" << appearance) {
					const float destinationZ = sample.Eye.Z > 0 ? -4.0f : 4.0f;
					std::array reference{
						Plane(1, {-10, 0, destinationZ}, 10, 20, {0.8f, 0.2f, 0.1f}),
						Plane(2, {10, 0, destinationZ}, 10, 20, {0.1f, 0.6f, 0.8f}),
					};
					std::vector<scene::DrawInstance> destination(reference.begin(), reference.end());
					for (auto &instance : destination) {
						instance.Frame.Position.X += DESTINATION_X;
					}
					ecs::Store interfaceWorld("portal.fixture.ui");
					std::unique_ptr<render::InterfacePass> interface;
					if (appearance == "custom") {
						const auto path =
							std::filesystem::path(SDL_GetBasePath()) / "shaders/resources/opaque.frag.spv";
						std::ifstream shader(path, std::ios::binary | std::ios::ate);
						REQUIRE(shader.good());
						const std::streamsize bytes = shader.tellg();
						REQUIRE(bytes > 0);
						REQUIRE(bytes % sizeof(uint32_t) == 0);
						std::vector<uint32_t> words(static_cast<size_t>(bytes) / sizeof(uint32_t));
						shader.seekg(0);
						REQUIRE(shader.read(reinterpret_cast<char *>(words.data()), bytes).good());
						const core::Name name("portal.fixture.custom");
						REQUIRE(fixture.Render.AddShader(name, words));
						for (auto &instance : destination) {
							instance.Shader = name;
						}
					} else if (appearance == "transparent") {
						// One layer over an opaque black wall has radiance alpha*tint.
						// The reference is an opaque wall of that analytically mixed tint.
						// A single face avoids blending coincident front/back triangles.
						auto mesh = DoorwayPlane();
						mesh.Vertices.resize(4);
						mesh.Indices.resize(6);
						const core::Name meshName("portal.fixture.single-face");
						REQUIRE(fixture.Render.AddMesh(meshName, mesh));
						destination[0].Tint = {};
						auto glass =
							Plane(4, {DESTINATION_X - 10, 0, destinationZ + .1f}, 10, 20, {1, .4f, .2f});
						glass.Mesh = meshName;
						glass.Transparency = .5f;
						destination.push_back(glass);
						// Instance opacity is packed to eight bits before either draw.
						constexpr float alpha = 128.0f / 255;
						reference[0].Tint = {alpha, .4f * alpha, .2f * alpha};
					} else if (appearance == "depth-ui" || appearance == "top-ui") {
						// Replace the left destination wall with a real spatial UI quad.
						// The independently unfolded reference remains a lit mesh.
						destination.erase(destination.begin());
						gui::RegisterGuiClasses();
						const auto collector =
							interfaceWorld.CreateInstance(gui::GuiClass("SurfaceGui"), "Sign");
						gui::SpatialCanvas spatial;
						spatial.Size = {100, 100};
						spatial.Origin = {DESTINATION_X - 20, 20, destinationZ};
						spatial.AxisX = {20, 0, 0};
						spatial.AxisY = {0, -40, 0};
						spatial.Normal = {0, 0, 1};
						spatial.Brightness = ambient;
						spatial.AlwaysOnTop = appearance == "top-ui";
						interfaceWorld.Set(collector, spatial);
						gui::DrawCommand rectangle;
						rectangle.Collector = collector;
						rectangle.Spatial = true;
						rectangle.Bounds = {{0, 0}, {100, 100}};
						rectangle.Clip = rectangle.Bounds;
						rectangle.Tint = reference[0].Tint;
						gui::DrawList list;
						list.Commands.push_back(rectangle);
						interface = std::make_unique<render::InterfacePass>();
						const auto backend = fixture.Render.Backend();
						REQUIRE(interface->Initialise(backend.Device, backend.ColourFormat));
						interface->Submit(list, {WIDTH, HEIGHT}, {WIDTH, HEIGHT}, interfaceWorld);
					}
					auto pane = Plane(3, {}, HALF_WIDTH, HALF_HEIGHT, {0.3f, 0.3f, 0.3f});
					pane.Surface = 0;
					destination.push_back(pane);
					render::PortalView portal;
					portal.Normal = {0, 0, 1};
					portal.First = {HALF_WIDTH, 0, 0};
					portal.Second = {0, HALF_HEIGHT, 0};
					portal.Warp.Frame.Position = {DESTINATION_X, 0, 0};
					std::array<render::View, 2> views;
					for (size_t index = 0; index < views.size(); index++) {
						auto &view = views[index];
						view.Slot = index;
						view.World = 920 + index;
						view.WorldName = core::Name(index == 0 ? "portal.room" : "portal.unfolded");
						view.Pipeline = core::Name("portal.fixture.pbr");
						view.Target = &target;
						view.CameraFrame = core::CFrame::LookAt(sample.Eye, sample.Aim) *
										   core::CFrame::Angles(0, 0, sample.Roll);
						view.Camera.FieldOfViewRadians = 1.0471975512f;
						view.Camera.NearPlane = 0.1f;
						view.Camera.FarPlane = 64;
						view.OverrideLighting = true;
						view.Lighting.Ambient = {ambient, ambient, ambient};
						view.Lighting.Direct = {};
					}
					views[0].Instances = destination;
					views[0].Portals = std::span(&portal, 1);
					views[1].Instances = reference;
					render::OverlayImage overlay;
					const auto frame = fixture.Render.Render(views, overlay, interface.get(), false);
					REQUIRE(frame.Ran(core::Name("portal-capture")));
					REQUIRE(frame.Ran(core::Name("portal-overlay")));
					if (sample.MinimumInterior > 0) {
						REQUIRE(frame.PortalPasses > 0);
					}
					for (const auto resource : {"tonemapped", "portaled"}) {
						for (const size_t slot : {size_t{0}, size_t{1}}) {
							const auto extent =
								fixture.Render.ResourceTextureExtent(core::Name(resource), slot);
							CHECK(extent.U == 1);
							CHECK(extent.V == 1);
							CHECK(extent.DrawnWidth == WIDTH);
							CHECK(extent.DrawnHeight == HEIGHT);
						}
					}
					const auto before = CaptureResource(
						fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
					);
					const auto unfolded = CaptureResource(
						fixture.Render, core::Name("tonemapped"), 1, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
					);
					const auto actual = CaptureResource(
						fixture.Render, core::Name("portaled"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
					);
					const std::string label = std::string(sample.Name) +
											  (appearance == "material" ? "" : "-" + std::string(appearance));
					auto labelledSample = sample;
					labelledSample.Name = label;
					CheckOpening(
						fixture.Render,
						labelledSample,
						ambient,
						document,
						views[0],
						destinationZ,
						before,
						unfolded,
						actual,
						destination,
						reference
					);
				}
			}
		}
	}
}

TEST_CASE(
	"portal pixels follow destination light edits with a fixed viewer",
	"[render][gpu][fixture][portal-lighting][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallPortalFixture(fixture.Render);
	const render::SceneTarget target{WIDTH, HEIGHT};
	const CameraSample camera{"moving-light", {0.6f, -0.3f, 4}};
	constexpr float ambient = 0.12f;
	constexpr float destinationZ = -4;
	const std::array reference{
		Plane(1, {-10, 0, destinationZ}, 10, 20, {0.8f, 0.2f, 0.1f}),
		Plane(2, {10, 0, destinationZ}, 10, 20, {0.1f, 0.6f, 0.8f}),
	};
	std::vector<scene::DrawInstance> destination(reference.begin(), reference.end());
	for (auto &instance : destination) {
		instance.Frame.Position.X += DESTINATION_X;
	}
	auto pane = Plane(3, {}, HALF_WIDTH, HALF_HEIGHT, {0.3f, 0.3f, 0.3f});
	pane.Surface = 0;
	destination.push_back(pane);
	render::PortalView portal;
	portal.Normal = {0, 0, 1};
	portal.First = {HALF_WIDTH, 0, 0};
	portal.Second = {0, HALF_HEIGHT, 0};
	portal.Warp.Frame.Position = {DESTINATION_X, 0, 0};
	std::array<render::View, 2> views;
	std::array<render::SceneLight, 2> lights;
	for (size_t slot = 0; slot < views.size(); ++slot) {
		auto &view = views[slot];
		view.Slot = slot;
		view.World = 930 + slot;
		view.WorldName = core::Name(slot == 0 ? "portal.lit" : "portal.lit-unfolded");
		view.Pipeline = core::Name("portal.fixture.pbr");
		view.Target = &target;
		view.CameraFrame = core::CFrame::LookAt(camera.Eye, {});
		view.Camera.FieldOfViewRadians = 1.0471975512f;
		view.Camera.NearPlane = 0.1f;
		view.Camera.FarPlane = 64;
		view.OverrideLighting = true;
		view.Lighting.Ambient = {ambient, ambient, ambient};
		view.Lighting.Direct = {};
		view.Lights = std::span(&lights[slot], 1);
	}
	views[0].Instances = destination;
	views[0].Portals = std::span(&portal, 1);
	views[1].Instances = reference;
	const std::array<core::Vector3, 3> positions{{{-2, 1, -1}, {2, -1, -2}, {0, 3, -0.5f}}};
	std::optional<assets::ContentHash> previous;
	for (size_t step = 0; step < positions.size(); ++step) {
		INFO("destination light step=" << step);
		lights[1].Position = positions[step];
		lights[1].Range = 12;
		lights[1].Colour = {2, 2, 2};
		lights[0] = lights[1];
		lights[0].Position.X += DESTINATION_X;
		render::OverlayImage overlay;
		const auto frame = fixture.Render.Render(views, overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("portal-capture")));
		REQUIRE(frame.PortalPasses > 0);
		const auto before = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto unfolded = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 1, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto actual = CaptureResource(
			fixture.Render, core::Name("portaled"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto hash = assets::Hasher::Of(actual.Bytes);
		if (previous) {
			CHECK(hash != *previous);
		}
		previous = hash;
		CameraSample sample = camera;
		sample.Name += '-' + std::to_string(step);
		CheckOpening(
			fixture.Render,
			sample,
			ambient,
			document,
			views[0],
			destinationZ,
			before,
			unfolded,
			actual,
			destination,
			reference
		);
	}
}
