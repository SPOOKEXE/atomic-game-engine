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
#include <engine/render/ResourceImage.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <array>
#include <chrono>
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

	graph::PipelineDocument InstallPortalFixture(render::Renderer &renderer, bool hdr = false) {
		auto document = hdr ? graph::DefaultWorldHdrDocument() : graph::DefaultPbrDocument();
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
			"tonemapped",
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
					// The exterior oracle is a separate render with no portal.
					auto baseline = views[0];
					baseline.Portals = {};
					baseline.Damage.Scene = true;
					fixture.Render.Render(std::span(&baseline, 1), overlay, interface.get(), false);
					const auto before = CaptureResource(
						fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
					);
					const auto frame = fixture.Render.Render(views, overlay, interface.get(), false);
					REQUIRE(frame.Ran(core::Name("surface-capture")));
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

					const auto unfolded = CaptureResource(
						fixture.Render, core::Name("tonemapped"), 1, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
					);
					const auto actual = CaptureResource(
						fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
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
		// The exterior oracle is a separate render with no portal.
		auto baseline = views[0];
		baseline.Portals = {};
		baseline.Damage.Scene = true;
		fixture.Render.Render(std::span(&baseline, 1), overlay, nullptr, false);
		const auto before = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto frame = fixture.Render.Render(views, overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("surface-capture")));
		REQUIRE(frame.PortalPasses > 0);

		const auto unfolded = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 1, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto actual = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
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

TEST_CASE(
	"portal material pipelines resolve each copied world's shader owner",
	"[render][gpu][shader-owner-draw][.]"
) {
	bool transparent = false;
	SECTION("opaque") {}
	SECTION("transparent") {
		transparent = true;
	}
	FixtureDevice fixture;
	fixture.Initialise();
	InstallPortalFixture(fixture.Render);
	const core::Name first("shader:first"), second("shader:second"), foreign("shader:foreign");
	const core::Name common("shader.common"), red("shader.reference.red"), green("shader.reference.green");
	render::ShaderCompiler compiler;
	const auto program = [&](const char *colour) {
		const std::string source =
			std::string("#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=") + colour +
			";}\n";
		auto compiled = compiler.Compile(source, render::ShaderStage::Fragment, "owner.frag");
		INFO(compiled.Error);
		REQUIRE_FALSE(compiled.Failed);
		return compiled.SpirV;
	};
	const auto redWords = program("vec4(1,0,0,1)");
	const auto greenWords = program("vec4(0,1,0,1)");
	REQUIRE(fixture.Render.AddShader(red, redWords));
	REQUIRE(fixture.Render.AddShader(green, greenWords));
	REQUIRE(fixture.Render.AddShader(common, redWords));
	REQUIRE(fixture.Render.AddShader(common, redWords, first));
	const auto acceptedResources = fixture.Render.ResourceRevision();
	REQUIRE(fixture.Render.AddShader(common, redWords, first));
	CHECK(fixture.Render.ResourceRevision() == acceptedResources);
	REQUIRE(fixture.Render.AddShader(common, greenWords, second));
	CHECK(fixture.Render.HasShader(common, first));
	CHECK_FALSE(fixture.Render.HasShader(common, core::Name("shader:absent")));
	for (const auto owner : {first, second}) {
		REQUIRE(fixture.Render.AddMesh(core::Name("portal.fixture.plane"), DoorwayPlane(), owner));
		assets::TextureData white;
		white.Width = white.Height = 1;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(fixture.Render.AddTexture(core::Name("portal.fixture.white"), white, owner));
	}
	std::array rows{
		Plane(1, {DESTINATION_X - 10, 0, -4}, 10, 20, {1, 1, 1}),
		Plane(2, {DESTINATION_X + 10, 0, -4}, 10, 20, {1, 1, 1}),
		Plane(3, {}, HALF_WIDTH, HALF_HEIGHT, {.3f, .3f, .3f})
	};
	rows[0].Transparency = rows[1].Transparency = transparent ? .5f : 0;
	rows[2].Surface = 0;
	render::PortalView portal;
	portal.Normal = {0, 0, 1};
	portal.First = {HALF_WIDTH, 0, 0};
	portal.Second = {0, HALF_HEIGHT, 0};
	portal.Warp.Frame.Position = {DESTINATION_X, 0, 0};
	render::SceneTarget target{WIDTH, HEIGHT};
	render::View view;
	view.World = 955;
	view.WorldName = core::Name("shader:native");
	view.Pipeline = core::Name("portal.fixture.pbr");
	view.Target = &target;
	view.CameraFrame = core::CFrame::LookAt({0, 0, 4}, {});
	view.Camera.FieldOfViewRadians = 1.0471975512f;
	view.Camera.NearPlane = .1f;
	view.Camera.FarPlane = 64;
	view.Instances = rows;
	view.Portals = std::span(&portal, 1);
	render::OverlayImage overlay;
	const auto capture = [&] {
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		return CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
	};
	rows[0].Shader = red;
	rows[1].Shader = green;
	const auto expected = capture();
	rows[0].Shader = green;
	rows[1].Shader = red;
	const auto reversed = capture();
	REQUIRE_FALSE(CompareImages(expected.View(), reversed.View()).Passed());
	rows[0].Shader = rows[1].Shader = common;
	rows[1].SourceWorld = foreign;
	std::array bindings{render::WorldContentOwner{foreign, second}};
	view.ContentOwner = first;
	view.ForeignContentOwners = bindings;
	CHECK(CompareImages(expected.View(), capture().View()).Passed());
	view.ContentOwner = second;
	bindings[0].Owner = first;
	view.Damage = {};
	CHECK(CompareImages(reversed.View(), capture().View()).Passed());
	REQUIRE(fixture.Render.AddShader(common, greenWords, first));
	const auto replaced = capture();
	CHECK_FALSE(CompareImages(reversed.View(), replaced.View()).Passed());
	REQUIRE(fixture.Render.AddShader(common, redWords, first));
	CHECK(CompareImages(reversed.View(), capture().View()).Passed());
	CHECK_FALSE(fixture.Render.AddShader(common, {}, first));
	CHECK(CompareImages(reversed.View(), capture().View()).Passed());
	REQUIRE(fixture.Render.DropShader(common, first));
	CHECK_FALSE(fixture.Render.HasShader(common, first));
	CHECK(fixture.Render.HasShader(common, second));
	CHECK(fixture.Render.HasShader(common));
	const auto missing = capture();
	CHECK_FALSE(CompareImages(reversed.View(), missing.View()).Passed());
	REQUIRE(fixture.Render.AddShader(common, redWords, first));
	CHECK(CompareImages(reversed.View(), capture().View()).Passed());
	gui::RegisterGuiClasses();
	scene::RegisterSceneClasses();
	ecs::Store prepared("prepared.material");
	const auto source = prepared.CreateInstance(scene::ShaderScriptClass(), common.Text());
	const auto material = prepared.CreateInstance(scene::MaterialClass(), "Material");
	prepared.GetMutable<scene::MaterialRef>(material)->Shader = common;
	const auto redSource =
		"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(1,0,0,1);}";
	const auto greenSource =
		"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(0,1,0,1);}";
	REQUIRE(scene::SetShaderSource(prepared, source, redSource));
	render::ShaderLibrary library;
	const auto prepare = [&] {
		return render::PrepareWorldShaders(prepared, first, library, fixture.Render);
	};
	prepare();
	CHECK_FALSE(prepare());
	CHECK(CompareImages(reversed.View(), capture().View()).Passed());
	REQUIRE(scene::SetShaderSource(prepared, source, greenSource));
	library.Refresh(prepared, first);
	library.Refresh(prepared, first);
	CHECK(prepare());
	CHECK(CompareImages(replaced.View(), capture().View()).Passed());
	REQUIRE(scene::SetShaderSource(prepared, source, "not a shader"));
	CHECK_FALSE(prepare());
	CHECK(CompareImages(replaced.View(), capture().View()).Passed());
	prepared.GetMutable<scene::MaterialRef>(material)->Shader = {};
	CHECK(prepare());
	CHECK(CompareImages(missing.View(), capture().View()).Passed());
	prepared.GetMutable<scene::MaterialRef>(material)->Shader = common;
	REQUIRE(scene::SetShaderSource(prepared, source, redSource));
	CHECK(prepare());
	CHECK(CompareImages(reversed.View(), capture().View()).Passed());
	const std::array survivingRows{rows[0], rows[2]};
	view.Instances = survivingRows;
	view.Damage.Scene = true;
	const auto surviving = capture();
	view.Instances = rows;
	view.Damage = {};
	fixture.Render.DropContentOwner(first);
	CHECK_FALSE(fixture.Render.HasShader(common, first));
	CHECK(fixture.Render.HasShader(common, second));
	CHECK(fixture.Render.HasShader(common));
	const auto retired = capture();
	CheckImage(
		fixture.Render,
		"shader-owner-retirement",
		transparent ? "transparent" : "opaque",
		"owner-scoped material pipeline retirement",
		surviving.View(),
		retired.View()
	);
}

TEST_CASE("lens pipelines stay within each view's content owner", "[render][gpu][lens-owner-draw][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto lensDocument = InstallPortalFixture(fixture.Render);
	const core::Name first("lens:first"), second("lens:second"), name("lens.common");
	render::ShaderCompiler compiler;
	const auto compile = [&](const char *colour) {
		const auto source =
			std::string("#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=") + colour +
			";}\n";
		auto result = compiler.Compile(source, render::ShaderStage::Fragment, "lens-owner.frag");
		INFO(result.Error);
		REQUIRE_FALSE(result.Failed);
		return result.SpirV;
	};
	const auto red = compile("vec4(1,0,0,1)");
	const auto green = compile("vec4(0,1,0,1)");
	REQUIRE(fixture.Render.AddLensShader(name, red));
	REQUIRE(fixture.Render.AddLensShader(name, red, first));
	const auto acceptedResources = fixture.Render.ResourceRevision();
	REQUIRE(fixture.Render.AddLensShader(name, red, first));
	CHECK(fixture.Render.ResourceRevision() == acceptedResources);
	REQUIRE(fixture.Render.AddLensShader(name, green, second));
	CHECK_FALSE(fixture.Render.HasLensShader(name, core::Name("absent")));
	render::SceneTarget target{WIDTH, HEIGHT};
	std::array<render::View, 2> views;
	for (size_t slot = 0; slot < views.size(); ++slot) {
		auto &view = views[slot];
		view.World = 970 + slot;
		view.WorldName = slot == 0 ? first : second;
		view.Slot = slot;
		view.Target = &target;
		view.Pipeline = core::Name("portal.fixture.pbr");
		view.CameraFrame = core::CFrame::LookAt({0, 0, 4}, {});
		view.Camera.NearPlane = .1f;
		view.Camera.FarPlane = 64;
		view.OverrideLighting = true;
		view.Lighting.ShaderLensCount = 1;
		view.Lighting.ShaderLenses[0].Shader = name;
		view.Lighting.ShaderLenses[0].Radius = 100;
		view.Lighting.ShaderLenses[0].Strength = 1;
	}
	render::OverlayImage overlay;
	const auto draw = [&] { fixture.Render.Render(views, overlay, nullptr, false); };
	const auto capture = [&](size_t slot) {
		return CaptureResource(
			fixture.Render, core::Name("tonemapped"), slot, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
	};
	draw();
	const auto expectedRed = capture(0);
	REQUIRE(fixture.Render.AddLensShader(name, green));
	draw();
	const auto expectedGreen = capture(0);
	REQUIRE_FALSE(CompareImages(expectedRed.View(), expectedGreen.View()).Passed());
	views[0].ContentOwner = first;
	views[1].ContentOwner = second;
	for (auto &view : views)
		view.Damage = {};
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(0).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	views[0].LensContentOwner = second;
	views[1].LensContentOwner = first;
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	for (auto &view : views)
		view.LensContentOwner.reset();
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(0).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	const core::Name timedName("lens.captured-time");
	const auto timed = compiler.Compile(
		R"glsl(#version 450
layout(location=0) out vec4 colour;
struct Lens {vec4 a;vec4 b;vec4 c;vec4 d;vec4 e;};
layout(set=3,binding=0) uniform LensPass {mat4 vp;mat4 inverseVp;vec4 target;vec4 eye;vec4 timeCount;Lens lenses[16];} pass;
void main(){colour=vec4(1-pass.timeCount.x,pass.timeCount.x,0,1);}
)glsl",
		render::ShaderStage::Fragment,
		"lens-time.frag"
	);
	REQUIRE_FALSE(timed.Failed);
	REQUIRE(fixture.Render.AddLensShader(timedName, timed.SpirV, first));
	views[0].Lighting.ShaderLenses[0].Shader = timedName;
	views[0].LensTimeSeconds = 0;
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(0).View()).Passed());
	views[0].LensTimeSeconds = 1;
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	views[0].LensTimeSeconds = 0;
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(0).View()).Passed());
	views[0].LensTimeSeconds.reset();
	views[0].Lighting.ShaderLenses[0].Shader = name;

	std::swap(views[0].ContentOwner, views[1].ContentOwner);
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	CHECK_FALSE(fixture.Render.AddLensShader(name, {}, first));
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	REQUIRE(fixture.Render.AddLensShader(name, green, first));
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	REQUIRE(fixture.Render.DropLensShader(name, first));
	draw();
	const auto missing = capture(1);
	CHECK_FALSE(CompareImages(expectedGreen.View(), missing.View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	REQUIRE(fixture.Render.AddLensShader(name, red, first));
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	fixture.Render.DropContentOwner(first);
	CHECK_FALSE(fixture.Render.HasLensShader(name, first));
	CHECK(fixture.Render.HasLensShader(name, second));
	CHECK(fixture.Render.HasLensShader(name));
	draw();
	CHECK(CompareImages(missing.View(), capture(1).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	gui::RegisterGuiClasses();
	scene::RegisterSceneClasses();
	ecs::Store prepared("prepared.lens");
	const auto source = prepared.CreateInstance(scene::LensShaderClass(), name.Text());
	const auto effect = prepared.CreateInstance(ecs::Classes::Find(core::Name("ShaderLens")), "Lens");
	prepared.GetMutable<scene::ShaderLens>(effect)->Shader = name;
	REQUIRE(
		scene::SetShaderSource(
			prepared,
			source,
			"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(1,0,0,1);}"
		)
	);
	render::ShaderLibrary library;
	const auto prepare = [&] {
		return render::PrepareWorldShaders(prepared, first, library, fixture.Render);
	};
	CHECK(prepare());
	CHECK_FALSE(prepare());
	CHECK_FALSE(fixture.Render.HasShader(name, first));
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	REQUIRE(
		scene::SetShaderSource(
			prepared,
			source,
			"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(0,1,0,1);}"
		)
	);
	library.RefreshLenses(prepared, first);
	library.RefreshLenses(prepared, first);
	CHECK(prepare());
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	REQUIRE(scene::SetShaderSource(prepared, source, "not a shader"));
	CHECK_FALSE(prepare());
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	prepared.Destroy(source);
	CHECK(prepare());
	draw();
	CHECK(CompareImages(missing.View(), capture(1).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());

	// Compare graph compositions with one fused shader over the same scene.
	// The swizzle and offsets make order observable without a tone-map oracle.
	const core::Name scaleName("lens.chain.scale"), swizzleName("lens.chain.swizzle"),
		fusedName("lens.chain.fused");
	const auto compileSample = [&](const std::string &body) {
		const auto source = std::string(R"(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 colour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D sceneDepth;
void main(){vec3 value=texture(sceneColour,uv).rgb;
)") + body + "colour=vec4(value,1);}\n";
		auto result = compiler.Compile(source, render::ShaderStage::Fragment, "lens-chain.frag");
		INFO(result.Error);
		REQUIRE_FALSE(result.Failed);
		return result.SpirV;
	};
	const std::string scale = "value=value*0.5+vec3(0.125,0.25,0.375);\n";
	const std::string swizzle = "value=value.brg*0.5+vec3(0.5,0.125,0);\n";
	REQUIRE(fixture.Render.AddLensShader(scaleName, compileSample(scale)));
	REQUIRE(fixture.Render.AddLensShader(swizzleName, compileSample(swizzle)));
	const auto install = [&](const graph::PipelineDocument &document) {
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(fixture.Render.SetPipeline(core::Name("portal.fixture.pbr"), graph));
	};
	for (const bool repeated : {false, true}) {
		graph::PipelineDocument chainDocument;
		bool resourcesAdded = false;
		bool lensSeen = false;
		for (auto edit : lensDocument.Edits()) {
			if (repeated && edit.Kind == graph::EditKind::AddNode && !resourcesAdded) {
				for (const auto resource : {"lens-repeat-colour", "lens-repeat-scratch"})
					chainDocument.Record(
						{.Kind = graph::EditKind::AddResource,
						 .Name = core::Name(resource),
						 .Resource = graph::ResourceKind::Colour,
						 .Format = graph::ResourceFormat::RGBA16F}
					);
				resourcesAdded = true;
			}
			if (repeated && edit.Kind == graph::EditKind::AddNode && lensSeen) {
				chainDocument.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("lens-repeat"),
					 .NodeKind = core::Name("shader-lenses"),
					 .Scope = graph::NodeScope::View}
				);
				chainDocument.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name("lens-b"),
					 .Key = core::Name("colour")}
				);
				chainDocument.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name("linear-depth"),
					 .Key = core::Name("depth")}
				);
				chainDocument.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("lens-repeat-colour"),
					 .Key = core::Name("colour")}
				);
				chainDocument.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("lens-repeat-scratch"),
					 .Key = core::Name("scratch")}
				);
				lensSeen = false;
			}
			if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("shader-lenses"))
				lensSeen = true;
			if (repeated && edit.Kind == graph::EditKind::Reads && edit.Target == core::Name("lens-b"))
				edit.Target = core::Name("lens-repeat-colour");
			chainDocument.Record(edit);
		}
		for (uint32_t count = 0; count <= 3; ++count) {
			CAPTURE(repeated, count);
			std::string fused;
			for (uint32_t pass = 0; pass < (repeated ? 2u : 1u); ++pass)
				for (uint32_t lens = 0; lens < count; ++lens)
					fused += lens == 1 ? swizzle : scale;
			REQUIRE(fixture.Render.AddLensShader(fusedName, compileSample(fused)));
			auto &view = views[0];
			view.ContentOwner = {};
			view.Lighting.ShaderLensCount = count == 0 ? 0 : 1;
			view.Lighting.ShaderLenses[0].Shader = fusedName;
			install(lensDocument);
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
			const auto expected = capture(0);
			view.Lighting.ShaderLensCount = count;
			for (uint32_t lens = 0; lens < count; ++lens) {
				view.Lighting.ShaderLenses[lens] = view.Lighting.ShaderLenses[0];
				view.Lighting.ShaderLenses[lens].Shader = lens == 1 ? swizzleName : scaleName;
			}
			install(chainDocument);
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
			CheckImage(
				fixture.Render,
				"lens-graph-chain",
				repeated ? "repeated" : "single",
				"authored lens order and graph output",
				expected.View(),
				capture(0).View(),
				// Each graph step rounds to half precision before the final 8-bit output.
				{.Absolute = 1.0 / 255.0, .Region = {}}
			);
		}
	}
	const core::Name depthBoundary("lens-depth-capture-boundary");
	graph::NodeKindSpec depthCapture;
	depthCapture.Kind = depthBoundary;
	depthCapture.Scope = graph::NodeScope::Frame;
	depthCapture.Queue = graph::ExecutionQueue::Cpu;
	depthCapture.Category = graph::NodeCategory::Output;
	for (const auto resource : {"linear-depth", "lens-zero-depth"})
		depthCapture.Inputs.push_back(
			{.Name = core::Name(resource),
			 .Kind = graph::ResourceKind::Colour,
			 .Format = graph::ResourceFormat::R32F}
		);
	REQUIRE(graph::RegisterNodeKind(std::move(depthCapture)));
	REQUIRE(fixture.Render.InstallNodeHandler(depthBoundary, [](const graph::RunContext &) { return true; }));
	const core::Name depthName("lens.chain.depth");
	REQUIRE(fixture.Render.AddLensShader(
		depthName, compileSample("value=vec3(texture(sceneDepth,uv).r/64.0,0,0);\n")
	));
	for (const bool remapDepth : {false, true}) {
		for (const uint32_t divisor : {1u, 2u}) {
			CAPTURE(remapDepth, divisor);
			graph::PipelineDocument depthDocument;
			bool resourcesAdded = false;
			bool inLens = false;
			for (auto edit : lensDocument.Edits()) {
				if (edit.Kind == graph::EditKind::AddResource &&
					(edit.Name == core::Name("lens-b") || edit.Name == core::Name("lens-scratch")))
					edit.Divisor = divisor;
				if (edit.Kind == graph::EditKind::AddNode && !resourcesAdded) {
					depthDocument.Record(
						{.Kind = graph::EditKind::AddResource,
						 .Name = core::Name("lens-zero-depth"),
						 .Resource = graph::ResourceKind::Colour,
						 .Format = graph::ResourceFormat::R32F,
						 .Divisor = 2}
					);
					resourcesAdded = true;
				}
				if (edit.Kind == graph::EditKind::AddNode) {
					inLens = edit.Name == core::Name("shader-lenses");
					if (inLens) {
						depthDocument.Record(
							{.Kind = graph::EditKind::AddNode,
							 .Name = core::Name("lens-depth-source"),
							 .NodeKind = core::Name("depth-linearise"),
							 .Scope = graph::NodeScope::View}
						);
						depthDocument.Record(
							{.Kind = graph::EditKind::Reads,
							 .Target = core::Name("depth"),
							 .Key = core::Name("depth")}
						);
						depthDocument.Record(
							{.Kind = graph::EditKind::Writes,
							 .Target = core::Name("lens-zero-depth"),
							 .Key = core::Name("linear")}
						);
						depthDocument.Record(
							{.Kind = graph::EditKind::Set, .Key = core::Name("background"), .Value = "zero"}
						);
					}
				}
				if (remapDepth && inLens && edit.Kind == graph::EditKind::Reads &&
					edit.Key == core::Name("depth"))
					edit.Target = core::Name("lens-zero-depth");
				depthDocument.Record(edit);
			}
			// Keep both depth images live so pool reuse cannot hide a wrong sampler binding.
			depthDocument.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = depthBoundary,
				 .NodeKind = depthBoundary,
				 .Scope = graph::NodeScope::Frame}
			);
			for (const auto resource : {"linear-depth", "lens-zero-depth"})
				depthDocument.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name(resource),
					 .Key = core::Name(resource)}
				);

			auto &view = views[0];
			view.Lighting.ShaderLensCount = 1;
			view.Lighting.ShaderLenses[0].Shader = fusedName;
			REQUIRE(fixture.Render.AddLensShader(
				fusedName, compileSample(remapDepth ? "value=vec3(0);" : "value=vec3(1,0,0);")
			));
			install(lensDocument);
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
			const auto expected = capture(0);
			view.Lighting.ShaderLenses[0].Shader = depthName;
			install(depthDocument);
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
			CheckImage(
				fixture.Render,
				"lens-graph-depth",
				remapDepth ? "alternate" : "default",
				"declared depth sampler and graph-sized lens targets",
				expected.View(),
				capture(0).View()
			);
			CHECK(
				fixture.Render.ResourceTexture(core::Name("linear-depth"), 0) !=
				fixture.Render.ResourceTexture(core::Name("lens-zero-depth"), 0)
			);
			const auto extent = fixture.Render.ResourceTextureExtent(core::Name("lens-b"), 0);
			CHECK(extent.DrawnWidth == WIDTH / divisor);
			CHECK(extent.DrawnHeight == HEIGHT / divisor);
		}
	}
}

TEST_CASE("gravitational lens keeps inward curvature across spin phases", "[render][gpu][lens-gradient][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	auto lensDocument = InstallPortalFixture(fixture.Render);
	const core::Name pipeline("portal.fixture.pbr"), captureNode("lens-export");
	lensDocument.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = captureNode,
		 .NodeKind = core::Name("capture"),
		 .Scope = graph::NodeScope::Frame}
	);
	lensDocument.Record(
		{.Kind = graph::EditKind::Reads, .Target = core::Name("lens-b"), .Key = core::Name("source")}
	);
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(lensDocument, graph, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(pipeline, graph));
	const core::Name gradient("lens.gradient");
	render::ShaderCompiler compiler;
	const auto program = compiler.Compile(
		R"glsl(#version 450
layout(location=0) out vec4 colour;
void main(){float blue=gl_FragCoord.x/64.0;colour=vec4(0,0,blue,1);}
)glsl",
		render::ShaderStage::Fragment,
		"lens-gradient.frag"
	);
	INFO(program.Error);
	REQUIRE_FALSE(program.Failed);
	REQUIRE(fixture.Render.AddShader(gradient, program.SpirV));

	render::SceneTarget target{65, 65};
	render::View view;
	view.Target = &target;
	view.Pipeline = pipeline;
	view.CameraFrame = core::CFrame::LookAt({0, 0, 4}, {});
	view.Camera.NearPlane = .1f;
	view.Camera.FarPlane = 64;
	view.OverrideLighting = true;
	auto plane = Plane(1, {0, 0, 0}, 16, 16, {1, 1, 1});
	plane.Shader = gradient;
	view.Instances = std::span(&plane, 1);
	render::OverlayImage overlay;
	const auto capture = [&] {
		const uint64_t token = fixture.Render.QueueResourceImage(pipeline, captureNode, 0);
		REQUIRE(token != 0);
		const render::FrameResult rendered =
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(rendered.Ran(captureNode));
		if (view.Lighting.ShaderLensCount != 0) REQUIRE(rendered.Ran(core::Name("shader-lenses")));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto image = fixture.Render.TakeResourceImage(token)) return std::move(*image);
			SDL_Delay(1);
		}
		FAIL("lens HDR capture did not complete");
		return render::ResourceImage{};
	};
	const auto baseline = capture();
	REQUIRE(baseline.Status == render::ResourceImageStatus::Ok);
	REQUIRE(baseline.Width == 65);
	REQUIRE(baseline.Height == 65);
	REQUIRE(baseline.RowStride == 65 * 8);
	REQUIRE(baseline.Pixels.size() == size_t(65) * 65 * 8);

	scene::RegisterSceneClasses();
	ecs::Store shaders("lens-gradient");
	const auto lensEntity = shaders.CreateInstance(ecs::Classes::Find(core::Name("ShaderLens")), "Lens");
	shaders.GetMutable<scene::ShaderLens>(lensEntity)->Shader = core::Name("gravitational-lens");
	render::ShaderLibrary library;
	REQUIRE(library.RefreshLenses(shaders) == 1);
	const auto *cooked = library.FindLens(core::Name("gravitational-lens"));
	REQUIRE(cooked != nullptr);
	REQUIRE(fixture.Render.AddLensShader(core::Name("gravitational-lens"), cooked->SpirV));
	view.Lighting.ShaderLensCount = 1;
	view.Lighting.ShaderLenses[0].Frame = core::CFrame({0, 0, 2});
	view.Lighting.ShaderLenses[0].Shader = core::Name("gravitational-lens");
	view.Lighting.ShaderLenses[0].Radius = 2.0f;
	view.Lighting.ShaderLenses[0].InnerRadius = .4f;
	view.Lighting.ShaderLenses[0].Strength = 2.0f;
	view.Lighting.ShaderLenses[0].Spin = .25f;
	const auto sample = [](const render::ResourceImage &image, size_t x, size_t y) {
		const auto bytes = std::span(image.Pixels).subspan(y * image.RowStride + x * 8, 8);
		core::ByteReader reader(bytes);
		const glm::vec2 redGreen = glm::unpackHalf2x16(reader.ReadUInt32());
		const glm::vec2 blueAlpha = glm::unpackHalf2x16(reader.ReadUInt32());
		return glm::vec4{redGreen, blueAlpha};
	};
	const auto correctedBlue = [&](const render::ResourceImage &image, size_t x) {
		const glm::vec4 colour = sample(image, x, 32);
		CHECK(std::isfinite(colour.r));
		CHECK(std::isfinite(colour.g));
		CHECK(std::isfinite(colour.b));
		return colour.b - .12f * colour.r;
	};
	const float leftBaseline = correctedBlue(baseline, 24);
	const float rightBaseline = correctedBlue(baseline, 40);
	const float axisBaseline = correctedBlue(baseline, 32);
	for (const float time : {0.0f, 12.5663706f}) {
		view.LensTimeSeconds = time;
		view.Damage.Scene = true;
		const auto lensed = capture();
		REQUIRE(lensed.Status == render::ResourceImageStatus::Ok);
		for (size_t y = 0; y < 65; y++)
			for (size_t x = 0; x < 65; x++) {
				const glm::vec4 colour = sample(lensed, x, y);
				CHECK(std::isfinite(colour.r));
				CHECK(std::isfinite(colour.g));
				CHECK(std::isfinite(colour.b));
				CHECK(std::isfinite(colour.a));
			}
		CHECK(correctedBlue(lensed, 24) > leftBaseline + .01f);
		CHECK(correctedBlue(lensed, 40) < rightBaseline - .01f);
		CHECK(std::abs(correctedBlue(lensed, 32) - axisBaseline) < .01f);
	}
}

TEST_CASE(
	"postprocess grades stay within each view's content owner", "[render][gpu][postprocess-owner-draw][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallPortalFixture(fixture.Render);
	const core::Name first("grade:first"), second("grade:second"), name("grade.common");
	render::ShaderCompiler compiler;
	const auto compile = [&](const char *colour) {
		const auto source =
			std::string("#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=") + colour +
			";}\n";
		auto result = compiler.Compile(source, render::ShaderStage::Fragment, "grade-owner.frag");
		INFO(result.Error);
		REQUIRE_FALSE(result.Failed);
		return result.SpirV;
	};
	const auto red = compile("vec4(1,0,0,1)");
	const auto green = compile("vec4(0,1,0,1)");
	REQUIRE(fixture.Render.SetPostProcessShader(name, red));
	REQUIRE(fixture.Render.SetPostProcessShader(name, red, first));
	const auto acceptedResources = fixture.Render.ResourceRevision();
	REQUIRE(fixture.Render.SetPostProcessShader(name, red, first));
	CHECK(fixture.Render.ResourceRevision() == acceptedResources);
	REQUIRE(fixture.Render.SetPostProcessShader(name, green, second));
	CHECK_FALSE(fixture.Render.PostProcessShaderName(core::Name("absent")).IsValid());
	render::SceneTarget target{WIDTH, HEIGHT};
	std::array<render::View, 2> views;
	for (size_t slot = 0; slot < views.size(); ++slot) {
		auto &view = views[slot];
		view.World = 970 + slot;
		view.WorldName = slot == 0 ? first : second;
		view.Slot = slot;
		view.Target = &target;
		view.Pipeline = core::Name("portal.fixture.pbr");
		view.CameraFrame = core::CFrame::LookAt({0, 0, 4}, {});
		view.Camera.NearPlane = .1f;
		view.Camera.FarPlane = 64;
		view.OverrideLighting = true;
	}
	render::OverlayImage overlay;
	const auto draw = [&] { fixture.Render.Render(views, overlay, nullptr, false); };
	const auto capture = [&](size_t slot) {
		return CaptureResource(
			fixture.Render, core::Name("tonemapped"), slot, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
	};
	draw();
	const auto expectedRed = capture(0);
	REQUIRE(fixture.Render.SetPostProcessShader(name, green));
	draw();
	const auto expectedGreen = capture(0);
	REQUIRE_FALSE(CompareImages(expectedRed.View(), expectedGreen.View()).Passed());
	views[0].ContentOwner = first;
	views[1].ContentOwner = second;
	for (auto &view : views)
		view.Damage = {};
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(0).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	std::swap(views[0].ContentOwner, views[1].ContentOwner);
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	CHECK_FALSE(fixture.Render.SetPostProcessShader(name, {}, first));
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	REQUIRE(fixture.Render.SetPostProcessShader(name, green, first));
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	fixture.Render.ClearPostProcessShader(first);
	draw();
	const auto missing = capture(1);
	CHECK_FALSE(CompareImages(expectedGreen.View(), missing.View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	REQUIRE(fixture.Render.SetPostProcessShader(name, red, first));
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	fixture.Render.DropContentOwner(first);
	CHECK_FALSE(fixture.Render.PostProcessShaderName(first).IsValid());
	CHECK(fixture.Render.PostProcessShaderName(second) == name);
	CHECK(fixture.Render.PostProcessShaderName() == name);
	draw();
	CHECK(CompareImages(missing.View(), capture(1).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	gui::RegisterGuiClasses();
	scene::RegisterSceneClasses();
	ecs::Store prepared("prepared.grade");
	const auto source = prepared.CreateInstance(scene::ShaderScriptClass(), name.Text());
	scene::SetPostProcessShader(prepared, name);
	REQUIRE(
		scene::SetShaderSource(
			prepared,
			source,
			"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(1,0,0,1);}"
		)
	);
	render::ShaderLibrary library;
	const auto prepare = [&] {
		return render::PrepareWorldShaders(prepared, first, library, fixture.Render);
	};
	CHECK(prepare());
	CHECK_FALSE(prepare());
	CHECK_FALSE(fixture.Render.HasShader(name, first));
	draw();
	CHECK(CompareImages(expectedRed.View(), capture(1).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
	REQUIRE(
		scene::SetShaderSource(
			prepared,
			source,
			"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(0,1,0,1);}"
		)
	);
	library.Refresh(prepared, first);
	library.Refresh(prepared, first);
	CHECK(prepare());
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	REQUIRE(scene::SetShaderSource(prepared, source, "not a shader"));
	CHECK_FALSE(prepare());
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	CHECK(render::PrepareWorldShaders(prepared, first, library, fixture.Render, nullptr, false));
	draw();
	CHECK(CompareImages(missing.View(), capture(1).View()).Passed());
	CHECK(prepare());
	draw();
	CHECK(CompareImages(expectedGreen.View(), capture(1).View()).Passed());
	prepared.Destroy(source);
	CHECK(prepare());
	draw();
	CHECK(CompareImages(missing.View(), capture(1).View()).Passed());
	CHECK(CompareImages(expectedGreen.View(), capture(0).View()).Passed());
}

TEST_CASE("interface shaders follow the submitted content owner", "[render][gpu][interface-owner-draw][.]") {
	gui::RegisterGuiClasses();
	const bool hdr = GENERATE(false, true);
	int placement = 0;
	SECTION("screen") {}
	SECTION("depth-tested spatial") {
		placement = 1;
	}
	SECTION("always-on-top spatial") {
		placement = 2;
	}
	CAPTURE(placement, hdr);
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallPortalFixture(fixture.Render, hdr);
	const auto display = std::find_if(document.Edits().begin(), document.Edits().end(), [](const auto &edit) {
		return edit.Kind == graph::EditKind::AddResource && edit.Name == core::Name("display");
	});
	REQUIRE(display != document.Edits().end());
	REQUIRE(display->Format == graph::ResourceFormat::RGBA16F);
	render::InterfacePass interface;
	const auto backend = fixture.Render.Backend();
	REQUIRE(interface.Initialise(backend.Device, backend.ColourFormat));
	const core::Name first("interface:first"), second("interface:second"), name("interface.common");
	render::ShaderCompiler compiler;
	const auto sourceOf = [](const char *colour) {
		const auto source = std::string(
								"#version 450\nlayout(location=2) in vec2 canvas;\n"
								"layout(set=3,binding=0) uniform Batch { vec4 clip; } batch;\n"
								"layout(location=0) out vec4 colour;\nvoid main(){"
								"if(canvas.x<batch.clip.x || canvas.y<batch.clip.y || canvas.x>batch.clip.z "
								"|| canvas.y>batch.clip.w) discard;colour="
							) +
							colour + ";}\n";
		return source;
	};
	const auto compile = [&](const char *colour) {
		auto result =
			compiler.Compile(sourceOf(colour), render::ShaderStage::Fragment, "interface-owner.frag");
		INFO(result.Error);
		REQUIRE_FALSE(result.Failed);
		return result.SpirV;
	};
	const auto red = compile("vec4(1,0,0,1)");
	const auto green = compile("vec4(0,1,0,1)");
	REQUIRE(interface.AddShaderVariant(name, red));
	REQUIRE(interface.AddShaderVariant(name, red, first));
	REQUIRE(interface.AddShaderVariant(name, green, second));
	CHECK_FALSE(interface.HasShaderVariant(name, core::Name("absent")));
	ecs::Store world("interface.owner");
	gui::DrawCommand rectangle;
	rectangle.Bounds = {{0, 0}, {WIDTH, HEIGHT}};
	rectangle.Clip = rectangle.Bounds;
	rectangle.Clip.Max.X *= .5f;
	rectangle.Tint = {1, 1, 1};
	rectangle.Shader = name;
	if (placement != 0) {
		const auto collector = world.CreateInstance(gui::GuiClass("SurfaceGui"), "Surface");
		gui::SpatialCanvas spatial;
		spatial.Size = {WIDTH, HEIGHT};
		spatial.Origin = {-1, 1, 0};
		spatial.AxisX = {2, 0, 0};
		spatial.AxisY = {0, -2, 0};
		spatial.Normal = {0, 0, 1};
		spatial.AlwaysOnTop = placement == 2;
		world.Set(collector, spatial);
		rectangle.Collector = collector;
		rectangle.Spatial = true;
	}

	gui::DrawList list;
	list.Commands.push_back(rectangle);
	const std::array occluders{Plane(1, {0, -.6f, 1}, 2, .5f, {.2f, .3f, .4f})};
	render::SceneTarget target{WIDTH, HEIGHT};
	render::View view;
	view.CameraFrame = core::CFrame::LookAt({0, 0, 4}, {});
	view.World = 989;
	view.WorldName = core::Name("interface.world");
	view.Target = &target;
	view.Instances = occluders;
	view.Pipeline = core::Name("portal.fixture.pbr");
	render::OverlayImage overlay;
	const auto capture = [&](render::InterfacePass *selected = nullptr) {
		if (selected == nullptr) selected = &interface;
		view.Damage.GameInterface = true;
		selected->Submit(list, {WIDTH, HEIGHT}, {WIDTH, HEIGHT}, world, 71);
		fixture.Render.Render(std::span(&view, 1), overlay, selected, false);
		if (placement == 0) REQUIRE(selected->LastBatchCount() > 0);
		return CaptureResource(
			fixture.Render, core::Name("composed-image"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
	};
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto background = CaptureResource(
		fixture.Render, core::Name("composed-image"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
	);
	const auto expectedRed = capture();
	REQUIRE(interface.AddShaderVariant(name, green));
	const auto expectedGreen = capture();
	REQUIRE_FALSE(CompareImages(expectedRed.View(), expectedGreen.View()).Passed());
	const auto matchesBackground = [&](uint32_t x, uint32_t y) {
		const size_t offset = y * expectedGreen.RowStrideBytes + x * 4;

		return std::equal(
			expectedGreen.Bytes.begin() + offset,
			expectedGreen.Bytes.begin() + offset + 4,
			background.Bytes.begin() + offset
		);
	};
	CHECK_FALSE(matchesBackground(WIDTH / 2 - 4, HEIGHT / 2));
	CHECK(matchesBackground(WIDTH / 2 + 4, HEIGHT / 2));
	CHECK(matchesBackground(WIDTH / 2 - 4, HEIGHT / 2 + 8) == (placement == 1));

	interface.SetContentOwner(first);
	CHECK(CompareImages(expectedRed.View(), capture().View()).Passed());
	CHECK(interface.LastUploadedBytes() == 0);
	interface.SetContentOwner(second);
	CHECK(CompareImages(expectedGreen.View(), capture().View()).Passed());
	CHECK(interface.LastUploadedBytes() == 0);
	interface.SetContentOwner(first);
	CHECK_FALSE(interface.AddShaderVariant(name, {}, first));
	CHECK(CompareImages(expectedRed.View(), capture().View()).Passed());
	REQUIRE(interface.AddShaderVariant(name, green, first));
	CHECK(CompareImages(expectedGreen.View(), capture().View()).Passed());
	REQUIRE(interface.DropShaderVariant(name, first));
	const auto missing = capture();
	CHECK_FALSE(CompareImages(expectedGreen.View(), missing.View()).Passed());
	CHECK(interface.HasShaderVariant(name, second));
	CHECK(interface.HasShaderVariant(name));
	REQUIRE(interface.AddShaderVariant(name, red, first));
	CHECK(CompareImages(expectedRed.View(), capture().View()).Passed());
	CHECK(interface.DropContentOwner({}) == 0);
	CHECK(interface.DropContentOwner(first) == 1);
	CHECK(interface.DropContentOwner(first) == 0);
	CHECK(CompareImages(missing.View(), capture().View()).Passed());
	interface.SetContentOwner(second);
	CHECK(CompareImages(expectedGreen.View(), capture().View()).Passed());
	CHECK(interface.HasShaderVariant(name));

	const auto source = world.CreateInstance(scene::ShaderScriptClass(), name.Text());
	REQUIRE(scene::SetShaderSource(world, source, sourceOf("vec4(1,0,0,1)")));
	const auto label = world.CreateInstance(gui::GuiClass("ImageLabel"), "ShaderDemand");
	world.GetMutable<gui::Picture>(label)->Shader = name;
	render::ShaderLibrary library;
	const std::array demand{name};
	REQUIRE(library.Refresh(world, first) == 1);
	REQUIRE(library.Refresh(world, first) == 0);
	REQUIRE(library.Changed().empty());
	CHECK(interface.RefreshShaders(demand, library, first) == 1);
	interface.SetContentOwner(first);
	CHECK(CompareImages(expectedRed.View(), capture().View()).Passed());
	CHECK(interface.RefreshShaders(demand, library, first) == 0);

	render::InterfacePass lagging;
	REQUIRE(lagging.Initialise(backend.Device, backend.ColourFormat));
	lagging.SetContentOwner(first);
	CHECK(render::PrepareWorldShaders(world, first, library, fixture.Render, &lagging));
	CHECK_FALSE(fixture.Render.HasShader(name, first));
	CHECK(CompareImages(expectedRed.View(), capture(&lagging).View()).Passed());
	CHECK(render::PrepareWorldShaders(world, second, library, fixture.Render, &lagging));
	const auto warmedResources = fixture.Render.ResourceRevision();
	CHECK(render::PrepareWorldShaders(world, first, library, fixture.Render, &lagging));
	CHECK(fixture.Render.ResourceRevision() == warmedResources);
	CHECK_FALSE(render::PrepareWorldShaders(world, first, library, fixture.Render, &lagging));
	CHECK(CompareImages(expectedRed.View(), capture(&lagging).View()).Passed());
	CHECK(lagging.LastUploadedBytes() == 0);

	REQUIRE(scene::SetShaderSource(world, source, sourceOf("vec4(0,1,0,1)")));
	REQUIRE(library.Refresh(world, first) == 1);
	CHECK(interface.RefreshShaders(demand, library, first) == 1);
	REQUIRE(library.Refresh(world, first) == 0);
	CHECK(lagging.RefreshShaders(demand, library, first) == 1);
	CHECK(CompareImages(expectedGreen.View(), capture(&lagging).View()).Passed());
	CHECK(lagging.RefreshShaders(demand, library, first) == 0);
	REQUIRE(scene::SetShaderSource(world, source, "not a shader"));
	CHECK(library.Refresh(world, first) == 0);
	CHECK(lagging.RefreshShaders(demand, library, first) == 0);
	CHECK(CompareImages(expectedGreen.View(), capture(&lagging).View()).Passed());
	CHECK(interface.RefreshShaders({}, library, first) == 1);
	CHECK_FALSE(interface.HasShaderVariant(name, first));
	CHECK(interface.HasShaderVariant(name, second));
	CHECK(lagging.HasShaderVariant(name, first));
	CHECK(interface.RefreshShaders(demand, library, first) == 1);
	CHECK(CompareImages(expectedGreen.View(), capture().View()).Passed());
	world.Destroy(source);
	REQUIRE(library.Refresh(world, first) == 1);
	REQUIRE(library.Refresh(world, first) == 0);
	CHECK(lagging.RefreshShaders(demand, library, first) == 1);
	CHECK_FALSE(lagging.HasShaderVariant(name, first));
	CHECK(CompareImages(missing.View(), capture(&lagging).View()).Passed());
}
