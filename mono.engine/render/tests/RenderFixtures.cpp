// Analytic planes exercise actual graph images. The oracle uses pinhole rays,
// independent of ResolveCamera, renderer matrices and shader projection code.

#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/resources/ShaderAssembly.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/content/MaterialLayout.hpp>
#include <engine/scene/content/TileSets.hpp>
#include <engine/testing/Suite.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.render.fixtures")
TEST_DEPENDS("engine.render.imagecomparison")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {

	using namespace engine;
	using namespace engine::render::test;

	struct Plane {
		float X = 0;
		float Y = 0;
		float Distance = 4;
		float HalfWidth = 1;
		float HalfHeight = 1;
		uint32_t Colour = 0xFF0000FFu;
	};

	struct ExpectedImages {
		std::vector<uint32_t> Albedo;
		std::vector<float> Depth;
	};

	struct FittedLens {
		double Left = -1;
		double Right = 1;
		double Bottom = -1;
		double Top = 1;
		bool Orthographic = false;
		core::Vector3 ClipNormal{};
		double ClipDistance = 0;
	};

	// Scalar ray intersections stay independent of the fitted GPU matrix.
	ExpectedImages ProjectFittedPlanes(
		std::span<const Plane> planes,
		uint32_t width,
		uint32_t height,
		const scene::Camera &camera,
		const core::CFrame &eye,
		const FittedLens &lens
	) {
		ExpectedImages images;
		images.Albedo.resize(static_cast<size_t>(width) * height);
		images.Depth.resize(images.Albedo.size(), camera.FarPlane);
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				const size_t pixel = static_cast<size_t>(y) * width + x;
				const double nearX = lens.Left + (lens.Right - lens.Left) * (x + 0.5) / width;
				const double nearY = lens.Top - (lens.Top - lens.Bottom) * (y + 0.5) / height;
				for (const Plane &plane : planes) {
					const double depth = plane.Distance + eye.Position.Z;
					if (depth < camera.NearPlane || depth >= images.Depth[pixel]) {
						continue;
					}
					const double scale = lens.Orthographic ? 1 : depth / camera.NearPlane;
					const double worldX = eye.Position.X + nearX * scale;
					const double worldY = eye.Position.Y + nearY * scale;
					const double clip = lens.ClipNormal.X * worldX + lens.ClipNormal.Y * worldY -
										lens.ClipNormal.Z * plane.Distance;
					if (clip < lens.ClipDistance || std::abs(worldX - plane.X) >= plane.HalfWidth ||
						std::abs(worldY - plane.Y) >= plane.HalfHeight) {
						continue;
					}
					images.Albedo[pixel] = plane.Colour;
					images.Depth[pixel] = static_cast<float>(depth);
				}
			}
		}
		return images;
	}

	// Intersects pixel-centre rays with finite front-facing planes and chooses
	// the nearest in the lens interval. Row zero is the top of the image.
	ExpectedImages ProjectPlanes(
		std::span<const Plane> planes,
		uint32_t width,
		uint32_t height,
		const scene::Camera &camera,
		const core::CFrame &eye
	) {
		ExpectedImages images;
		images.Albedo.resize(static_cast<size_t>(width) * height);
		images.Depth.resize(images.Albedo.size(), camera.FarPlane);
		const double tangent = std::tan(static_cast<double>(camera.FieldOfViewRadians) / 2);
		const double aspect = static_cast<double>(width) / height;
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				const size_t pixel = static_cast<size_t>(y) * width + x;
				for (const Plane &plane : planes) {
					const double depth = plane.Distance + eye.Position.Z;
					if (depth < camera.NearPlane || depth >= images.Depth[pixel]) {
						continue;
					}
					const double worldX =
						eye.Position.X + (2 * (x + 0.5) / width - 1) * tangent * aspect * depth;
					const double worldY = eye.Position.Y + (1 - 2 * (y + 0.5) / height) * tangent * depth;
					if (std::abs(worldX - plane.X) < plane.HalfWidth &&
						std::abs(worldY - plane.Y) < plane.HalfHeight) {
						images.Albedo[pixel] = plane.Colour;
						images.Depth[pixel] = static_cast<float>(depth);
					}
				}
			}
		}
		return images;
	}

	assets::MeshData PlaneMesh() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	std::vector<scene::DrawInstance> DrawPlanes(std::span<const Plane> planes) {
		std::vector<scene::DrawInstance> instances;
		for (const Plane &plane : planes) {
			scene::DrawInstance instance;
			instance.Source = static_cast<uint32_t>(instances.size() + 1);
			instance.Frame.Position = {plane.X, plane.Y, -plane.Distance};
			instance.HalfExtent = {plane.HalfWidth, plane.HalfHeight, 0.01f};
			instance.Tint = {
				static_cast<float>(plane.Colour & 255u) / 255,
				static_cast<float>((plane.Colour >> 8) & 255u) / 255,
				static_cast<float>((plane.Colour >> 16) & 255u) / 255
			};
			instance.Mesh = core::Name("fixture.plane");
			instance.Texture = core::Name("fixture.white");
			instance.CastShadow = false;
			instances.push_back(instance);
		}
		return instances;
	}

	graph::PipelineDocument InstallFixture(render::Renderer &renderer) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		// This final consumer keeps captured intermediates alive through every graph write.
		const core::Name captureKind("fixture-capture-boundary");
		graph::NodeKindSpec capture;
		capture.Kind = captureKind;
		capture.Scope = graph::NodeScope::Frame;
		capture.Queue = graph::ExecutionQueue::Cpu;
		capture.Category = graph::NodeCategory::Output;
		for (const auto resource :
			 {"albedo", "linear-depth", "composed-image", "object-ids", "semantic-ids", "part-ids"}) {
			capture.Inputs.push_back({.Name = core::Name(resource), .Kind = graph::ResourceKind::Texture});
		}
		REQUIRE(graph::RegisterNodeKind(std::move(capture)));
		REQUIRE(renderer.InstallNodeHandler(captureKind, [](const graph::RunContext &) { return true; }));
		document.Record({
			.Kind = graph::EditKind::AddNode,
			.Name = captureKind,
			.NodeKind = captureKind,
			.Scope = graph::NodeScope::Frame,
		});
		for (const auto resource :
			 {"albedo", "linear-depth", "composed-image", "object-ids", "semantic-ids", "part-ids"}) {
			document.Record({
				.Kind = graph::EditKind::Reads,
				.Target = core::Name(resource),
				.Key = core::Name(resource),
			});
		}
		const std::array captures{
			std::pair{"fixture-lit-capture", "lit"},
			std::pair{"fixture-god-rays-capture", "god-rays"},
		};
		for (const auto &[name, resource] : captures) {
			document.Record({
				.Kind = graph::EditKind::AddNode,
				.Name = core::Name(name),
				.NodeKind = core::Name("capture"),
				.Scope = graph::NodeScope::Frame,
			});
			document.Record({
				.Kind = graph::EditKind::Reads,
				.Target = core::Name(resource),
				.Key = core::Name("source"),
			});
		}
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("fixture.pbr"), graph));
		REQUIRE(renderer.AddMesh(core::Name("fixture.plane"), PlaneMesh()));
		assets::TextureData white;
		white.Width = 1;
		white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(core::Name("fixture.white"), white));
		return document;
	}

	void CheckPlanes(
		render::Renderer &renderer,
		const graph::PipelineDocument &document,
		std::string_view name,
		std::span<const Plane> planes,
		const render::View &view
	) {
		const uint32_t width = view.Target->Width;
		const uint32_t height = view.Target->Height;
		const ExpectedImages expected = ProjectPlanes(planes, width, height, view.Camera, view.CameraFrame);
		std::ostringstream inputs;
		inputs << "slot=" << view.Slot << " world=" << view.World << " fov=" << view.Camera.FieldOfViewRadians
			   << " near=" << view.Camera.NearPlane << " far=" << view.Camera.FarPlane
			   << " eye=" << view.CameraFrame.Position.X << ',' << view.CameraFrame.Position.Y << ','
			   << view.CameraFrame.Position.Z << "\nseed=0 samples=1 projection=perspective\n";
		for (const Plane &plane : planes) {
			inputs << "plane=" << plane.X << ',' << plane.Y << ',' << plane.Distance << ',' << plane.HalfWidth
				   << ',' << plane.HalfHeight << ',' << plane.Colour << '\n';
		}
		inputs << graph::Write(document);
		const CapturedImage albedo = CaptureResource(
			renderer, core::Name("albedo"), view.Slot, width, height, ImageFormat::Rgba8Unorm
		);
		CheckImage(
			renderer,
			name,
			"albedo",
			inputs.str(),
			{width, height, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(expected.Albedo))},
			albedo.View()
		);
		const CapturedImage depth = CaptureResource(
			renderer, core::Name("linear-depth"), view.Slot, width, height, ImageFormat::R32Float
		);
		// D24 quantization propagates through the perspective inverse. At far=32
		// and near=.25, 0.001 covers its worst-case depth error with margin.
		ImageTolerance depthTolerance;
		depthTolerance.Absolute = 0.001;
		CheckImage(
			renderer,
			name,
			"linear-depth",
			inputs.str(),
			{width, height, ImageFormat::R32Float, std::as_bytes(std::span(expected.Depth))},
			depth.View(),
			depthTolerance
		);
	}
}

TEST_CASE(
	"pinhole oracle fixes image orientation, depth order and odd target pitch", "[render][gpu][fixture][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array planes{
		Plane{-1.1f, 0.7f, 4, 0.72f, 0.46f, 0xFF0000FFu},
		Plane{0.65f, -0.5f, 3, 0.52f, 0.65f, 0xFF00FF00u},
		Plane{0.65f, -0.5f, 6, 1.5f, 1.5f, 0xFFFF0000u},
	};
	const auto instances = DrawPlanes(planes);
	render::View view;
	view.World = 901;
	view.WorldName = core::Name("fixture.projection");
	view.Pipeline = core::Name("fixture.pbr");
	view.Instances = instances;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	for (const render::SceneTarget target : {render::SceneTarget{65, 37}, {113, 71}, {1, 1}}) {
		view.Target = &target;
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("gbuffer")));
		REQUIRE(frame.Ran(core::Name("depth-linearise")));
		CheckPlanes(fixture.Render, document, "pinhole-" + std::to_string(target.Width), planes, view);
	}
}

TEST_CASE(
	"scaled depth exports resample the active rectangle of a padded target",
	"[render][gpu][fixture][projection][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto basis = InstallFixture(fixture.Render);
	graph::PipelineDocument document;
	const core::Name exported("fixture-scaled-depth");
	constexpr uint32_t outputWidth = 32, outputHeight = 16;
	const auto addExport = [&] {
		document.Record({
			.Kind = graph::EditKind::AddResource,
			.Name = exported,
			.Resource = graph::ResourceKind::Colour,
			.Format = graph::ResourceFormat::R32F,
			.Width = outputWidth,
			.Height = outputHeight,
		});
		document.Record({
			.Kind = graph::EditKind::AddNode,
			.Name = core::Name("fixture-scaled-depth-export"),
			.NodeKind = core::Name("depth-linearise"),
			.Scope = graph::NodeScope::View,
		});
		document.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name("depth"), .Key = core::Name("depth")}
		);
		document.Record({.Kind = graph::EditKind::Writes, .Target = exported, .Key = core::Name("linear")});
		document.Record({.Kind = graph::EditKind::Set, .Key = core::Name("background"), .Value = "zero"});
	};
	for (const auto &edit : basis.Edits()) {
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("present")) addExport();
		document.Record(edit);
	}
	graph::NodeKindSpec boundary;
	boundary.Kind = core::Name("fixture-scaled-depth-boundary");
	boundary.Scope = graph::NodeScope::Frame;
	boundary.Queue = graph::ExecutionQueue::Cpu;
	boundary.Category = graph::NodeCategory::Output;
	boundary.Inputs.push_back({.Name = core::Name("depth"), .Kind = graph::ResourceKind::Texture});
	REQUIRE(graph::RegisterNodeKind(boundary));
	REQUIRE(fixture.Render.InstallNodeHandler(boundary.Kind, [](const graph::RunContext &) { return true; }));
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = boundary.Kind,
		 .NodeKind = boundary.Kind,
		 .Scope = graph::NodeScope::Frame}
	);
	document.Record({.Kind = graph::EditKind::Reads, .Target = exported, .Key = core::Name("depth")});
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(core::Name("fixture.scaled-depth"), pipeline));
	const std::array planes{
		Plane{-1.1f, 0.7f, 4, 0.72f, 0.46f, 0xFF0000FFu},
		Plane{0.65f, -0.5f, 3, 0.52f, 0.65f, 0xFF00FF00u},
		Plane{0.65f, -0.5f, 6, 1.5f, 1.5f, 0xFFFF0000u},
	};
	const auto instances = DrawPlanes(planes);
	// Two fraction bits on each axis keep corner weights exact at four bits.
	const render::SceneTarget target{80, 40};
	render::View view;
	view.World = 907;
	view.Pipeline = core::Name("fixture.scaled-depth");
	view.Instances = instances;
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Submitted);
	REQUIRE(frame.Ran(core::Name("fixture-scaled-depth-export")));
	// G-buffer attachments share the allocation; albedo reports its active fraction before publication.
	const auto extent = fixture.Render.ResourceTextureExtent(core::Name("albedo"));
	CHECK(extent.U < 1.0f);
	CHECK(extent.V < 1.0f);
	const auto source = ProjectPlanes(planes, target.Width, target.Height, view.Camera, {});
	const double nearPlane = view.Camera.NearPlane, farPlane = view.Camera.FarPlane;
	// A D24-quantized reference interpolates raw depth before distance, rather than blending metres.
	const auto rawAt = [&](int x, int y) {
		x = std::clamp(x, 0, static_cast<int>(target.Width) - 1);
		y = std::clamp(y, 0, static_cast<int>(target.Height) - 1);
		const double distance = source.Depth[static_cast<size_t>(y) * target.Width + x];
		const double raw =
			farPlane / (farPlane - nearPlane) - farPlane * nearPlane / ((farPlane - nearPlane) * distance);
		constexpr double d24 = 16777215.0;
		return std::round(raw * d24) / d24;
	};
	std::vector<float> expected(outputWidth * outputHeight);
	size_t blendedPixels = 0;
	for (uint32_t y = 0; y < outputHeight; ++y) {
		for (uint32_t x = 0; x < outputWidth; ++x) {
			const double sourceX = (x + 0.5) * target.Width / outputWidth - 0.5;
			const double sourceY = (y + 0.5) * target.Height / outputHeight - 0.5;
			const int left = static_cast<int>(std::floor(sourceX)),
					  top = static_cast<int>(std::floor(sourceY));
			const double horizontal = sourceX - left, vertical = sourceY - top;
			const double upper = std::lerp(rawAt(left, top), rawAt(left + 1, top), horizontal);
			const double lower = std::lerp(rawAt(left, top + 1), rawAt(left + 1, top + 1), horizontal);
			const double raw = std::lerp(upper, lower, vertical);
			blendedPixels += raw != rawAt(left, top);
			expected[y * outputWidth + x] = static_cast<float>(
				raw >= 0.999999 ? 0.0 : farPlane * nearPlane / (farPlane - raw * (farPlane - nearPlane))
			);
		}
	}
	CHECK(blendedPixels > 0);
	const auto actual =
		CaptureResource(fixture.Render, exported, 0, outputWidth, outputHeight, ImageFormat::R32Float);
	ImageTolerance tolerance;
	tolerance.Absolute = 0.001;
	CheckImage(
		fixture.Render,
		"scaled-depth",
		"linear-depth",
		graph::Write(document),
		{outputWidth, outputHeight, ImageFormat::R32Float, std::as_bytes(std::span(expected))},
		actual.View(),
		tolerance
	);
}

TEST_CASE(
	"default PBR preserves distinct part tints through the display transform", "[render][gpu][colour][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallFixture(fixture.Render);
	const std::array planes{
		Plane{-1.2f, 0.0f, 4.0f, .8f, 1.0f, 0xFF0000FFu},
		Plane{1.2f, 0.0f, 4.0f, .8f, 1.0f, 0xFF00FF00u},
	};
	const auto instances = DrawPlanes(planes);
	CHECK(instances[0].EmissiveStrength == 0.0f);
	CHECK(instances[1].EmissiveStrength == 0.0f);
	render::SceneTarget target{96, 64};
	render::View view;
	view.World = 905;
	view.WorldName = core::Name("fixture.part-colour");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = instances;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {1.0f, 1.0f, 1.0f};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	render::OverlayImage overlay;
	const uint64_t litToken =
		fixture.Render.QueueResourceImage(view.Pipeline, core::Name("fixture-lit-capture"), view.Slot);
	const uint64_t godRaysToken =
		fixture.Render.QueueResourceImage(view.Pipeline, core::Name("fixture-god-rays-capture"), view.Slot);
	REQUIRE(litToken != 0);
	REQUIRE(godRaysToken != 0);
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Ran(core::Name("gbuffer")));
	REQUIRE(frame.Ran(core::Name("deferred-lighting")));
	REQUIRE(frame.Ran(core::Name("tonemap")));

	const auto sample = [](const CapturedImage &image, uint32_t x, uint32_t y) {
		const std::byte *pixel = image.Bytes.data() + y * image.RowStrideBytes + x * 4;
		return std::array{
			std::to_integer<uint8_t>(pixel[0]),
			std::to_integer<uint8_t>(pixel[1]),
			std::to_integer<uint8_t>(pixel[2]),
		};
	};
	const CapturedImage albedo = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	const CapturedImage display = CapturePresentationResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, target.Width, target.Height
	);
	const auto redAlbedo = sample(albedo, 38, 32);
	const auto greenAlbedo = sample(albedo, 58, 32);
	const auto redDisplay = sample(display, 38, 32);
	const auto greenDisplay = sample(display, 58, 32);
	const auto await = [&fixture](uint64_t token) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto image = fixture.Render.TakeResourceImage(token)) return std::move(*image);
			SDL_Delay(1);
		}
		FAIL("HDR fixture image did not complete");
		return render::ResourceImage{};
	};
	const render::ResourceImage lit = await(litToken);
	const render::ResourceImage godRays = await(godRaysToken);
	const auto sampleHdr = [](const render::ResourceImage &image, uint32_t x, uint32_t y) {
		REQUIRE(image.Status == render::ResourceImageStatus::Ok);
		REQUIRE(image.Format == render::ResourceImageFormat::RGBA16_Float);
		const std::byte *pixel = image.Pixels.data() + y * image.RowStride + x * 8;
		uint32_t redGreen, blueAlpha;
		std::memcpy(&redGreen, pixel, sizeof(redGreen));
		std::memcpy(&blueAlpha, pixel + sizeof(redGreen), sizeof(blueAlpha));
		const glm::vec2 redGreenValue = glm::unpackHalf2x16(redGreen);
		const glm::vec2 blueAlphaValue = glm::unpackHalf2x16(blueAlpha);
		return std::array{redGreenValue.x, redGreenValue.y, blueAlphaValue.x};
	};
	const auto redLit = sampleHdr(lit, 38, 32);
	const auto greenLit = sampleHdr(lit, 58, 32);
	const auto redGodRays = sampleHdr(godRays, 38, 32);
	const auto greenGodRays = sampleHdr(godRays, 58, 32);
	INFO(
		"red albedo=" << int(redAlbedo[0]) << ',' << int(redAlbedo[1]) << ',' << int(redAlbedo[2])
					  << " display=" << int(redDisplay[0]) << ',' << int(redDisplay[1]) << ','
					  << int(redDisplay[2])
	);
	INFO(
		"green albedo=" << int(greenAlbedo[0]) << ',' << int(greenAlbedo[1]) << ',' << int(greenAlbedo[2])
						<< " display=" << int(greenDisplay[0]) << ',' << int(greenDisplay[1]) << ','
						<< int(greenDisplay[2])
	);
	INFO(
		"red lit=" << redLit[0] << ',' << redLit[1] << ',' << redLit[2] << " god-rays=" << redGodRays[0]
				   << ',' << redGodRays[1] << ',' << redGodRays[2]
	);
	INFO(
		"green lit=" << greenLit[0] << ',' << greenLit[1] << ',' << greenLit[2]
					 << " god-rays=" << greenGodRays[0] << ',' << greenGodRays[1] << ',' << greenGodRays[2]
	);
	CHECK(redAlbedo[0] > redAlbedo[1] + 80);
	CHECK(redAlbedo[0] > redAlbedo[2] + 80);
	CHECK(greenAlbedo[1] > greenAlbedo[0] + 80);
	CHECK(greenAlbedo[1] > greenAlbedo[2] + 80);
	CHECK(redLit[0] > .95f);
	CHECK(std::abs(redLit[1]) < .02f);
	CHECK(std::abs(redLit[2]) < .02f);
	CHECK(greenLit[1] > .95f);
	CHECK(std::abs(greenLit[0]) < .02f);
	CHECK(std::abs(greenLit[2]) < .02f);
	CHECK(redGodRays[0] > .95f);
	CHECK(std::abs(redGodRays[1]) < .02f);
	CHECK(std::abs(redGodRays[2]) < .02f);
	CHECK(greenGodRays[1] > .95f);
	CHECK(std::abs(greenGodRays[0]) < .02f);
	CHECK(std::abs(greenGodRays[2]) < .02f);
	CHECK(redDisplay[0] > redDisplay[1] + 40);
	CHECK(redDisplay[0] > redDisplay[2] + 40);
	CHECK(greenDisplay[1] > greenDisplay[0] + 40);
	CHECK(greenDisplay[1] > greenDisplay[2] + 40);
}

TEST_CASE("completed camera submission publishes bounded visibility facts", "[render][gpu][visibility][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallFixture(fixture.Render);
	std::array instances{
		DrawPlanes(std::array{Plane{0, 0, 4, .5f, .5f}}).front(),
		DrawPlanes(std::array{Plane{8, 0, 4, .5f, .5f}}).front(),
		DrawPlanes(std::array{Plane{0, 0, 5, .5f, .5f}}).front(),
	};
	instances[0].Source = 101;
	instances[1].Source = 202;
	instances[2].Source = 303;
	instances[2].Transparency = .5f;
	render::SceneTarget target{64, 64};
	render::View view;
	view.Slot = 17;
	view.World = 17;
	view.WorldName = core::Name("visibility.fixture");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32;
	view.Instances = instances;
	render::OverlayImage overlay;
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Ran(core::Name("gbuffer")));
	const auto snapshot = fixture.Render.Visibility();
	REQUIRE(snapshot.Valid);
	CHECK(snapshot.ViewSlot == view.Slot);
	CHECK(snapshot.World == view.WorldName);
	REQUIRE(snapshot.Observations.size() == 3);
	CHECK(snapshot.Observations[0].Entity == 101);
	CHECK(snapshot.Observations[0].State == render::VisibilityState::SubmittedOpaqueOrMasked);
	CHECK(snapshot.Observations[1].Entity == 202);
	CHECK(snapshot.Observations[1].State == render::VisibilityState::FrustumCulled);
	CHECK(snapshot.Observations[2].Entity == 303);
	CHECK(snapshot.Observations[2].State == render::VisibilityState::SubmittedBlended);

	view.VisibilityFrame = view.CameraFrame;
	view.CameraFrame = core::CFrame(core::Vector3{8, 0, 0});
	view.Damage.Scene = true;
	const auto lockedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(lockedFrame.Submitted);
	CHECK(lockedFrame.DrawCalls > 0);
	const auto locked = fixture.Render.Visibility();
	REQUIRE(locked.Valid);
	REQUIRE(locked.Observations.size() == 3);
	CHECK(locked.Observations[0].State == render::VisibilityState::SubmittedOpaqueOrMasked);
	CHECK(locked.Observations[1].State == render::VisibilityState::FrustumCulled);

	view.VisibilityFrame.reset();
	view.Damage.Scene = true;
	const auto unlockedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(unlockedFrame.Submitted);
	CHECK(unlockedFrame.DrawCalls > 0);
	const auto unlocked = fixture.Render.Visibility();
	REQUIRE(unlocked.Valid);
	REQUIRE(unlocked.Observations.size() == 3);
	CHECK(unlocked.Observations[0].State == render::VisibilityState::FrustumCulled);
	CHECK(unlocked.Observations[1].State == render::VisibilityState::SubmittedOpaqueOrMasked);
	view.CameraFrame = {};

	view.Instances = {};
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto empty = fixture.Render.Visibility();
	CHECK(empty.Valid);
	CHECK(empty.Observations.empty());

	view.Instances = instances;
	view.Projection = glm::mat4{std::numeric_limits<float>::infinity()};
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK_FALSE(fixture.Render.Visibility().Valid);

	view.Projection.reset();
	render::View second = view;
	second.Slot = 18;
	second.World = 18;
	second.WorldName = core::Name("visibility.second");
	std::array views{view, second};
	fixture.Render.Render(views, overlay, nullptr, false);
	const auto finalView = fixture.Render.Visibility();
	REQUIRE(finalView.Valid);
	CHECK(finalView.ViewSlot == second.Slot);
	CHECK(finalView.World == second.WorldName);

	graph::RenderGraph forwardGraph;
	core::Name forwardOffender;
	REQUIRE(
		graph::Build(graph::DefaultForwardTierCDocument(), forwardGraph, forwardOffender) ==
		graph::PipelineDocumentStatus::Ok
	);
	REQUIRE(fixture.Render.SetPipeline(core::Name("visibility.forward"), forwardGraph));
	view.Pipeline = core::Name("visibility.forward");
	view.Instances = instances;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto forward = fixture.Render.Visibility();
	REQUIRE(forward.Valid);
	REQUIRE(forward.Observations.size() == 3);
	CHECK(forward.Observations[0].Entity == 101);
	CHECK(forward.Observations[0].State == render::VisibilityState::SubmittedOpaqueOrMasked);

	render::ShaderCompiler compiler;
	const auto program = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour; void main(){colour=vec4(1);}",
		render::ShaderStage::Fragment,
		"visibility-authored.frag"
	);
	INFO(program.Error);
	REQUIRE_FALSE(program.Failed);
	const core::Name authoredShader("visibility.authored");
	REQUIRE(fixture.Render.AddShader(authoredShader, program.SpirV));
	instances[0].Shader = authoredShader;
	view.Pipeline = core::Name("fixture.pbr");
	view.Instances = std::span(instances).first(1);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto authored = fixture.Render.Visibility();
	REQUIRE(authored.Valid);
	REQUIRE(authored.Observations.size() == 1);
	CHECK(authored.Observations[0].Entity == 101);
	CHECK(authored.Observations[0].State == render::VisibilityState::SubmittedOpaqueOrMasked);
}

TEST_CASE(
	"rendered assets follow native and copied world content owners", "[render][gpu][content-binding][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const core::Name first("session:first"), second("session:second"), foreignWorld("world:foreign");
	const core::Name red("reference.red"), green("reference.green"), common("shared-spelling.texture");
	const auto upload = [&](core::Name name, uint32_t colour, core::Name owner = {}) {
		assets::TextureData texture;
		texture.Width = texture.Height = 1;
		for (size_t channel = 0; channel < 4; ++channel)
			texture.Pixels.push_back(std::byte((colour >> (channel * 8)) & 255u));
		REQUIRE(fixture.Render.AddTexture(name, texture, owner));
	};
	upload(red, 0xFF0000FFu);
	upload(green, 0xFF00FF00u);
	upload(common, 0xFF0000FFu, first);
	upload(common, 0xFF00FF00u, second);
	auto instances = DrawPlanes(
		std::array{Plane{-1, 0, 4, .65f, .65f, 0xFFFFFFFFu}, Plane{1, 0, 4, .65f, .65f, 0xFFFFFFFFu}}
	);
	bool namedMesh = false;
	SECTION("shared built-in meshes still split same-name texture runs") {
		for (auto &row : instances)
			row.Mesh = core::Name(assets::BuiltinName(assets::BuiltinMesh::Cube));
	}
	SECTION("named meshes use their own world's residency") {
		namedMesh = true;
		auto mesh = PlaneMesh();
		assets::Submesh material;
		material.IndexCount = 6;
		material.Texture = common.Text();
		mesh.Submeshes.push_back(std::move(material));
		REQUIRE(fixture.Render.AddMesh(core::Name("fixture.plane"), mesh, first));
		REQUIRE(fixture.Render.AddMesh(core::Name("fixture.plane"), mesh, second));
	}
	render::SceneTarget target{65, 37};
	render::View view;
	view.World = 931;
	view.WorldName = core::Name("world:native");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32;
	view.Instances = instances;
	render::OverlayImage overlay;
	const auto capture = [&] {
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("gbuffer")));
		return CaptureResource(
			fixture.Render,
			core::Name("albedo"),
			view.Slot,
			target.Width,
			target.Height,
			ImageFormat::Rgba8Unorm
		);
	};
	instances[0].Texture = red;
	instances[1].Texture = green;
	const auto expected = capture();
	instances[0].Texture = green;
	instances[1].Texture = red;
	const auto reversed = capture();
	CHECK_FALSE(CompareImages(expected.View(), reversed.View()).Passed());
	instances[1].Texture = green;
	const auto sameOwner = capture();
	view.Instances = std::span(instances).first(1);
	const auto retired = capture();
	view.Instances = instances;
	instances[0].Texture = instances[1].Texture = namedMesh ? core::Name{} : common;
	instances[1].SourceWorld = foreignWorld;
	std::array bindings{render::WorldContentOwner{foreignWorld, second}};
	view.ContentOwner = first;
	view.ForeignContentOwners = bindings;
	const auto scoped = capture();
	CheckImage(
		fixture.Render,
		"content-binding",
		"native-and-copied",
		graph::Write(document),
		expected.View(),
		scoped.View()
	);
	view.ContentOwner = second;
	bindings[0].Owner = first;
	view.Damage = {};
	const auto swapped = capture();
	CheckImage(
		fixture.Render,
		"content-binding",
		"owner-switch",
		graph::Write(document),
		reversed.View(),
		swapped.View()
	);
	bindings[0].Owner = second;
	const auto rebound = capture();
	CheckImage(
		fixture.Render,
		"content-binding",
		"foreign-owner-switch",
		graph::Write(document),
		sameOwner.View(),
		rebound.View()
	);
	if (namedMesh) {
		bindings[0].Owner = first;
		const auto survivor = fixture.Render.TextureHandle(common, second);
		uint64_t settledBufferBytes = 0;
		for (size_t cycle = 0; cycle < 12; ++cycle) {
			INFO("owner retirement cycle " << cycle);
			view.Damage.Scene = true;
			const auto submitted = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
			REQUIRE(submitted.Ran(core::Name("gbuffer")));
			view.Damage = {};
			const auto before = fixture.Render.MemoryStatistics();
			// Retire before any readback wait. The submitted draw still owns its reads.
			fixture.Render.DropContentOwner(first);
			const auto after = fixture.Render.MemoryStatistics();
			CHECK(after.TextureBytes + 4 == before.TextureBytes);
			CHECK(after.Textures + 1 == before.Textures);
			CHECK(after.ReleasedBytes == before.ReleasedBytes + 4);
			CHECK(fixture.Render.TextureHandle(common, first) == nullptr);
			CHECK(fixture.Render.TextureHandle(common, second) == survivor);
			const auto inFlight = CaptureResource(
				fixture.Render,
				core::Name("albedo"),
				view.Slot,
				target.Width,
				target.Height,
				ImageFormat::Rgba8Unorm
			);
			CHECK(CompareImages(reversed.View(), inFlight.View()).Passed());
			const auto removed = capture();
			CheckImage(
				fixture.Render,
				"content-binding",
				"retired-mesh",
				graph::Write(document),
				retired.View(),
				removed.View()
			);
			// Reusing the owner must invalidate quiet views and refill its retired ranges.
			upload(common, 0xFF0000FFu, first);
			auto mesh = PlaneMesh();
			assets::Submesh material;
			material.IndexCount = 6;
			material.Texture = common.Text();
			mesh.Submeshes.push_back(std::move(material));
			REQUIRE(fixture.Render.AddMesh(core::Name("fixture.plane"), mesh, first));
			const auto restored = capture();
			CHECK(CompareImages(reversed.View(), restored.View()).Passed());
			const auto resident = fixture.Render.MemoryStatistics();
			CHECK(resident.TextureBytes == before.TextureBytes);
			CHECK(resident.Textures == before.Textures);
			if (cycle == 3) settledBufferBytes = resident.BufferBytes;
			if (cycle > 3) CHECK(resident.BufferBytes == settledBufferBytes);
		}
	}
}

TEST_CASE("graph images replace stale geometry after edits and camera motion", "[render][gpu][fixture][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	std::array planes{Plane{0.7f, 0.35f, 3, 0.62f, 0.39f, 0xFF0000FFu}};
	render::SceneTarget target{79, 53};
	render::View view;
	view.World = 902;
	view.WorldName = core::Name("fixture.edits");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	for (uint32_t step = 0; step < 4; step++) {
		if (step == 1) {
			planes[0].X = -0.65f;
			planes[0].Colour = 0xFF00FF00u;
		}
		if (step == 2) {
			view.CameraFrame.Position = {0.35f, -0.2f, 0.5f};
		}
		const std::span<const Plane> visible = step == 3 ? std::span<const Plane>{} : std::span(planes);
		const auto instances = DrawPlanes(visible);
		view.Instances = instances;
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CheckPlanes(fixture.Render, document, "edit-" + std::to_string(step), visible, view);
	}
}

TEST_CASE("static scene residency stays bounded and settles after edits", "[render][gpu][fixture][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallFixture(fixture.Render);
	std::array planes{
		Plane{-.70f, -.35f, 4, .09f, .09f, 0xFF0000FFu},
		Plane{-.50f, -.25f, 4, .09f, .09f, 0xFF00FF00u},
		Plane{-.30f, -.15f, 4, .09f, .09f, 0xFFFF0000u},
		Plane{-.10f, -.05f, 4, .09f, .09f, 0xFFFFFF00u},
		Plane{.10f, .05f, 4, .09f, .09f, 0xFFFF00FFu},
		Plane{.30f, .15f, 4, .09f, .09f, 0xFF00FFFFu},
		Plane{.50f, .25f, 4, .09f, .09f, 0xFFFFFFFFu},
		Plane{.70f, .35f, 4, .09f, .09f, 0xFF8080FFu},
	};
	auto instances = DrawPlanes(planes);
	render::SceneTarget target{79, 53};
	render::View view;
	view.World = 907;
	view.WorldName = core::Name("fixture.residency");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Instances = instances;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	const auto render = [&] { return fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false); };
	const auto checkSettled = [&](const render::GpuMemoryStatistics &baseline) {
		const auto frame = render();
		CHECK(frame.UploadedBytes == 0);
		const auto memory = fixture.Render.MemoryStatistics();
		CHECK(memory.BufferAllocations == baseline.BufferAllocations);
		CHECK(memory.TransferBufferAllocations == baseline.TransferBufferAllocations);
		CHECK(memory.TextureAllocations == baseline.TextureAllocations);
	};

	// Warm the index versions and the first changed-row transfer before measuring reuse.
	for (size_t frame = 0; frame < 8; ++frame) {
		if (frame == 3) {
			planes[0].X += .01f;
			instances = DrawPlanes(planes);
			view.Instances = instances;
		}
		REQUIRE(render().Submitted);
	}
	const auto warm = fixture.Render.MemoryStatistics();
	for (size_t frame = 0; frame < 8; ++frame) {
		checkSettled(warm);
	}

	planes[1].X += .12f;
	instances = DrawPlanes(planes);
	view.Instances = instances;
	const auto oneRow = render();
	INFO("one changed row uploaded " << oneRow.UploadedBytes << " bytes");
	REQUIRE(oneRow.UploadedBytes > 0);
	for (size_t frame = 0; frame < 8; ++frame) {
		checkSettled(warm);
	}

	planes[0].Y += .11f;
	planes.back().Y -= .11f;
	instances = DrawPlanes(planes);
	view.Instances = instances;
	const auto distantRows = render();
	INFO("two distant changed rows uploaded " << distantRows.UploadedBytes << " bytes");
	CHECK(distantRows.UploadedBytes == oneRow.UploadedBytes * 2);
	for (size_t frame = 0; frame < 8; ++frame) {
		checkSettled(warm);
	}
}

TEST_CASE(
	"instance prefix batches match full uploads and recover cancelled frames",
	"[render][gpu][fixture][instance-prefix][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");
	const auto document = InstallFixture(fixture.Render);
	graph::PipelineDocument transfer;
	for (const auto &edit : document.Edits()) {
		transfer.Record(edit);
		if (edit.Kind == graph::EditKind::AddNode &&
			(edit.NodeKind == core::Name("mesh-residency") || edit.NodeKind == core::Name("delta-upload"))) {
			transfer.Record({.Kind = graph::EditKind::Set, .Key = core::Name("queue"), .Value = "transfer"});
		}
	}
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(transfer, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(core::Name("fixture.full-upload"), pipeline));

	std::vector<Plane> planes;
	for (size_t index = 0; index < 512; index++) {
		planes.push_back(
			{-1.55f + .1f * (index % 32),
			 -.75f + .1f * (index / 32),
			 4,
			 .035f,
			 .035f,
			 index % 2 == 0 ? 0xFF0000FFu : 0xFF00FF00u}
		);
	}
	auto instances = DrawPlanes(planes);
	const render::SceneTarget target{83, 59};
	std::array<render::View, 2> views;
	for (size_t index = 0; index < views.size(); index++) {
		auto &view = views[index];
		view.World = 920 + index;
		view.WorldName = core::Name(index == 0 ? "fixture.prefix" : "fixture.full");
		view.Slot = index;
		view.Pipeline = core::Name(index == 0 ? "fixture.pbr" : "fixture.full-upload");
		view.Instances = instances;
		view.Target = &target;
		view.Camera.FieldOfViewRadians = 1.5707963267948966f;
		view.Camera.NearPlane = .25f;
		view.Camera.FarPlane = 32;
	}
	render::OverlayImage overlay;
	const auto render = [&] { return fixture.Render.Render(views, overlay, nullptr, false); };
	const auto move = [&] {
		for (auto &instance : instances)
			instance.Frame.Position.X += .1f;
		for (auto &view : views) {
			view.Instances = instances;
			view.Damage.Objects = true;
		}
	};
	const auto samePixels = [&] {
		const auto compact = CaptureResource(
			fixture.Render, core::Name("albedo"), 0, target.Width, target.Height, ImageFormat::Rgba8Unorm
		);
		const auto full = CaptureResource(
			fixture.Render, core::Name("albedo"), 1, target.Width, target.Height, ImageFormat::Rgba8Unorm
		);
		CHECK(compact.Bytes == full.Bytes);
	};
	for (size_t frame = 0; frame < 8; frame++)
		REQUIRE(render().Submitted);
	CHECK(render().UploadedBytes == 0);
	const auto before = CaptureResource(
		fixture.Render, core::Name("albedo"), 0, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);

	move();
	const auto compact = render();
	REQUIRE(compact.Submitted);
	CHECK(compact.UploadedBytes == 512 * (36 + 80));
	CHECK(compact.ComputeDispatches == 1);
	const auto after = CaptureResource(
		fixture.Render, core::Name("albedo"), 0, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	CHECK(after.Bytes != before.Bytes);
	samePixels();

	// One changed tail escalates the complete batch, even when every other row only moved.
	move();
	instances[0].HalfExtent.X += .005f;
	const auto changedTail = render();
	REQUIRE(changedTail.Submitted);
	CHECK(changedTail.UploadedBytes == 512 * 80 * 2);
	CHECK(changedTail.ComputeDispatches == 0);
	samePixels();

	move();
	instances.back().Source += 1000;
	const auto newIdentity = render();
	REQUIRE(newIdentity.Submitted);
	CHECK(newIdentity.ComputeDispatches == 0);
	samePixels();
	for (size_t frame = 0; frame < 8; frame++)
		REQUIRE(render().Submitted);

#if ENGINE_ASSERTS_ENABLED
	struct ResetFailure {
		~ResetFailure() {
			render::test_support::SetFrameBatchSubmissionFailureForTests(false);
		}
	} reset;
	move();
	render::test_support::SetFrameBatchSubmissionFailureForTests(true);
	const auto cancelled = render();
	CHECK_FALSE(cancelled.Submitted);
	CHECK(cancelled.ComputeDispatches == 1);
	render::test_support::SetFrameBatchSubmissionFailureForTests(false);
	const auto recovered = render();
	REQUIRE(recovered.Submitted);
	CHECK(recovered.ComputeDispatches == 0);
	CHECK(recovered.UploadedBytes >= 512 * 80 * 2);
	samePixels();
#endif
	move();
	const auto resumed = render();
	REQUIRE(resumed.Submitted);
	CHECK(resumed.UploadedBytes == 512 * (36 + 80));
	CHECK(resumed.ComputeDispatches == 1);
	samePixels();
}

TEST_CASE("pinhole reference has known coverage and rejects clipped planes", "[render][fixture-math]") {
	scene::Camera camera;
	camera.FieldOfViewRadians = 1.5707963267948966f;
	camera.NearPlane = 0.25f;
	camera.FarPlane = 32;
	const std::array planes{Plane{0, 0, 2, 1, 1, 0xFF0000FFu}};
	const ExpectedImages expected = ProjectPlanes(planes, 4, 4, camera, {});
	const std::vector<uint32_t> colours{
		0,
		0,
		0,
		0,
		0,
		0xFF0000FFu,
		0xFF0000FFu,
		0,
		0,
		0xFF0000FFu,
		0xFF0000FFu,
		0,
		0,
		0,
		0,
		0,
	};
	CHECK(expected.Albedo == colours);
	CHECK(expected.Depth[0] == 32);
	CHECK(expected.Depth[5] == 2);
	camera.NearPlane = 2.5f;
	CHECK(ProjectPlanes(planes, 4, 4, camera, {}).Albedo == std::vector<uint32_t>(16));
	camera.NearPlane = 0.25f;
	camera.FarPlane = 1.5f;
	CHECK(ProjectPlanes(planes, 4, 4, camera, {}).Albedo == std::vector<uint32_t>(16));
}

TEST_CASE(
	"graph projection honours changed field of view and clipping distances", "[render][gpu][fixture][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array planes{
		Plane{0, 0, 0.2f, 2, 2, 0xFFFFFFFFu},
		Plane{-0.6f, 0.4f, 4, 0.47f, 0.32f, 0xFF0000FFu},
		Plane{0.5f, -0.3f, 5, 0.54f, 0.62f, 0xFF00FF00u},
		Plane{0, 0, 20, 20, 20, 0xFFFF0000u},
	};
	const auto instances = DrawPlanes(planes);
	const render::SceneTarget target{97, 61};
	render::View view;
	view.World = 903;
	view.WorldName = core::Name("fixture.lens");
	view.Pipeline = core::Name("fixture.pbr");
	view.Instances = instances;
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.0471975511965977f;
	view.Camera.NearPlane = 0.5f;
	view.Camera.FarPlane = 12;
	render::OverlayImage overlay;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "changed-lens", planes, view);
}

TEST_CASE(
	"fitted camera projections preserve clipping and linear depth", "[render][gpu][fixture][projection][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array planes{
		Plane{-1, 0.5f, 4, 0.8f, 0.6f, 0xFF0000FFu},
		Plane{3.7f, -0.3f, 3, 0.6f, 0.7f, 0xFF00FF00u},
		Plane{0.2f, -0.4f, 6, 4, 3, 0xFFFF0000u},
		Plane{0, 0, 0.1f, 20, 20, 0xFFFFFFFFu},
	};
	const auto instances = DrawPlanes(planes);
	const render::SceneTarget target{107, 73};
	render::View view;
	view.World = 906;
	view.WorldName = core::Name("fixture.fitted-camera");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Instances = instances;
	view.Camera.NearPlane = 0.5f;
	view.Camera.FarPlane = 32;
	view.Camera.FieldOfViewRadians = 1.0471975511965977f;
	render::OverlayImage overlay;
	for (const bool placed : {false, true}) {
		// Apply the same rigid placement to camera and geometry. The independent
		// local-space pinhole oracle is unchanged, while graph culling must use
		// both the fitted projection and the non-identity camera transform.
		view.CameraFrame = placed ? core::CFrame::LookAt({30, 4, 10}, {29, 3, 6}) : core::CFrame{};
		auto placedInstances = instances;
		for (auto &instance : placedInstances) {
			instance.Frame = view.CameraFrame * instance.Frame;
		}
		view.Instances = placedInstances;
		for (const bool orthographic : {false, true}) {
			for (const bool clipped : {false, true}) {
				// The oblique replacement tilts the far plane as well. All fixture
				// geometry lies well inside that plane; the independent near half-space
				// is the boundary tested here.
				if (orthographic && clipped) {
					continue;
				}
				FittedLens lens;
				lens.Orthographic = orthographic;
				lens.Left = orthographic ? -2.5 : -0.3;
				lens.Right = orthographic ? 4.5 : 0.9;
				lens.Bottom = orthographic ? -2 : -0.4;
				lens.Top = orthographic ? 2 : 0.6;
				if (clipped) {
					lens.ClipNormal = {0.25f, 0.1f, -1};
					lens.ClipDistance = 4.2;
				}
				view.Projection = orthographic ? glm::ortho(
													 static_cast<float>(lens.Left),
													 static_cast<float>(lens.Right),
													 static_cast<float>(lens.Bottom),
													 static_cast<float>(lens.Top),
													 view.Camera.NearPlane,
													 view.Camera.FarPlane
												 )
											   : glm::frustum(
													 static_cast<float>(lens.Left),
													 static_cast<float>(lens.Right),
													 static_cast<float>(lens.Bottom),
													 static_cast<float>(lens.Top),
													 view.Camera.NearPlane,
													 view.Camera.FarPlane
												 );
				if (clipped) {
					view.Projection = scene::ObliqueProjection(
						*view.Projection,
						view.CameraFrame,
						view.CameraFrame.VectorToWorldSpace(lens.ClipNormal),
						static_cast<float>(lens.ClipDistance) +
							view.CameraFrame.VectorToWorldSpace(lens.ClipNormal)
								.Dot(view.CameraFrame.Position)
					);
				}
				const ExpectedImages expected =
					ProjectFittedPlanes(planes, target.Width, target.Height, view.Camera, {}, lens);
				REQUIRE(std::count_if(expected.Albedo.begin(), expected.Albedo.end(), [](uint32_t pixel) {
							return pixel != 0;
						}) > 100);
				const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
				REQUIRE(frame.Ran(core::Name("gbuffer")));
				REQUIRE(frame.Ran(core::Name("depth-linearise")));
				const std::string name = std::string(
											 orthographic ? "orthographic"
											 : clipped	  ? "oblique"
														  : "off-axis"
										 ) +
										 (placed ? "-placed" : "");
				std::ostringstream inputs;
				inputs << "projection=" << name << " extents=" << lens.Left << ',' << lens.Right << ','
					   << lens.Bottom << ',' << lens.Top << " near=" << view.Camera.NearPlane
					   << " far=" << view.Camera.FarPlane << " clip=" << lens.ClipNormal.X << ','
					   << lens.ClipNormal.Y << ',' << lens.ClipNormal.Z << ',' << lens.ClipDistance
					   << "\ncamera=" << view.CameraFrame.Position.X << ',' << view.CameraFrame.Position.Y
					   << ',' << view.CameraFrame.Position.Z << " placed=" << placed << "\nseed=0 samples=1\n"
					   << graph::Write(document);
				const CapturedImage albedo = CaptureResource(
					fixture.Render,
					core::Name("albedo"),
					0,
					target.Width,
					target.Height,
					ImageFormat::Rgba8Unorm
				);
				CheckImage(
					fixture.Render,
					name,
					"albedo",
					inputs.str(),
					{target.Width,
					 target.Height,
					 ImageFormat::Rgba8Unorm,
					 std::as_bytes(std::span(expected.Albedo))},
					albedo.View()
				);
				const CapturedImage depth = CaptureResource(
					fixture.Render,
					core::Name("linear-depth"),
					0,
					target.Width,
					target.Height,
					ImageFormat::R32Float
				);
				ImageTolerance tolerance;
				tolerance.Absolute = 0.001;
				CheckImage(
					fixture.Render,
					name,
					"linear-depth",
					inputs.str(),
					{target.Width,
					 target.Height,
					 ImageFormat::R32Float,
					 std::as_bytes(std::span(expected.Depth))},
					depth.View(),
					tolerance
				);
			}
		}
	}
	view.CameraFrame = {};
	view.Instances = instances;
	for (const float invalid : {0.0f, std::numeric_limits<float>::infinity()}) {
		view.Projection = glm::mat4{invalid};
		const auto refused = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK_FALSE(refused.Ran(core::Name("gbuffer")));
	}
	view.Projection.reset();
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "fitted-to-perspective", planes, view);
}

TEST_CASE("batched cameras keep world residency and view images separate", "[render][gpu][fixture][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	std::array firstPlanes{Plane{-0.5f, 0.4f, 3, 0.43f, 0.37f, 0xFF0000FFu}};
	std::array secondPlanes{Plane{0.6f, -0.35f, 4, 0.57f, 0.46f, 0xFFFF0000u}};
	auto firstInstances = DrawPlanes(firstPlanes);
	auto secondInstances = DrawPlanes(secondPlanes);
	const render::SceneTarget target{83, 59};
	std::array<render::View, 3> views;
	for (size_t index = 0; index < views.size(); index++) {
		auto &view = views[index];
		view.Slot = index;
		view.World = index == 1 ? 905 : 904;
		view.WorldName = core::Name(index == 1 ? "fixture.world-b" : "fixture.world-a");
		view.Pipeline = core::Name("fixture.pbr");
		view.Instances = index == 1 ? std::span(secondInstances) : std::span(firstInstances);
		view.Damage.Objects = true;
		view.Target = &target;
		view.Camera.FieldOfViewRadians = 1.5707963267948966f;
		view.Camera.NearPlane = 0.25f;
		view.Camera.FarPlane = 32;
	}
	views[2].CameraFrame.Position.X = 0.4f;
	render::OverlayImage overlay;
	fixture.Render.Render(views, overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "world-a-first", firstPlanes, views[0]);
	CheckPlanes(fixture.Render, document, "world-b-first", secondPlanes, views[1]);
	CheckPlanes(fixture.Render, document, "world-a-second-camera", firstPlanes, views[2]);
	views[0].Damage = {};
	views[2].Damage = {};
	secondPlanes[0].Y = 0.6f;
	secondInstances = DrawPlanes(secondPlanes);
	views[1].Instances = secondInstances;
	views[1].Damage.Objects = true;
	fixture.Render.Render(views, overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "world-a-retained", firstPlanes, views[0]);
	CheckPlanes(fixture.Render, document, "world-b-edited", secondPlanes, views[1]);
	firstPlanes[0].X += .1f;
	secondPlanes[0].X -= .1f;
	firstInstances = DrawPlanes(firstPlanes);
	secondInstances = DrawPlanes(secondPlanes);
	views[0].Instances = views[2].Instances = firstInstances;
	views[1].Instances = secondInstances;
	for (auto &view : views) {
		view.Damage.Scene = true;
		view.Damage.Objects = true;
	}
	const auto prefixes = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(prefixes.Submitted);
	CHECK(prefixes.ComputeDispatches == 2);
	CheckPlanes(fixture.Render, document, "world-a-edited", firstPlanes, views[0]);
	CheckPlanes(fixture.Render, document, "world-b-edited-prefix", secondPlanes, views[1]);
	CheckPlanes(fixture.Render, document, "world-a-edited-second-camera", firstPlanes, views[2]);
}

TEST_CASE(
	"authored opaque colour and discard survive occlusion phases", "[render][gpu][authored-occlusion][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	graph::PipelineDocument occlusion;
	for (const auto &edit : document.Edits()) {
		occlusion.Record(edit);
		if (edit.Kind == graph::EditKind::AddNode && edit.NodeKind == core::Name("cull-frustum"))
			occlusion.Record(
				{.Kind = graph::EditKind::Set, .Key = core::Name("culling"), .Value = "occlusion"}
			);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(occlusion, graph, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(core::Name("fixture.occlusion"), graph));
	render::ShaderCompiler compiler;
	const auto program = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour;\n"
		"void main(){if(gl_FragCoord.x<64)discard;colour=vec4(0,0,1,1);}",
		render::ShaderStage::Fragment,
		"cutout.frag"
	);
	INFO(program.Error);
	REQUIRE_FALSE(program.Failed);
	const core::Name shader("fixture.cutout");
	REQUIRE(fixture.Render.AddShader(shader, program.SpirV));
	// Large rows enter the early phase. Small rows enter the late phase, with
	// both authored and ordinary materials on each side of the cutout.
	const std::array planes{
		Plane{0, 0, 6, 8, 8, 0xFF00FF00u},
		Plane{0, 0, 4, 4, 4, 0xFFFFFFFFu},
		Plane{1, 0, 2, .25f, 1, 0xFF0000FFu},
		Plane{-1, 0, 3, .1f, .1f, 0xFF00FFFFu},
		Plane{1.3f, .8f, 3, .1f, .1f, 0xFFFFFFFFu}
	};
	auto rows = DrawPlanes(planes);
	rows[1].Shader = rows[4].Shader = shader;
	render::SceneTarget target{129, 97};
	render::View view;
	view.World = 906;
	view.WorldName = core::Name("fixture.authored-occlusion");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Instances = rows;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	uint32_t drawCalls = 0;
	const auto capture = [&] {
		view.Damage.Scene = true;
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("gbuffer")));
		drawCalls = frame.DrawCalls;
		return std::array{
			CaptureResource(
				fixture.Render, core::Name("composed-image"), 0, 129, 97, ImageFormat::Rgba8Unorm
			),
			CaptureResource(fixture.Render, core::Name("linear-depth"), 0, 129, 97, ImageFormat::R32Float)
		};
	};
	const auto expected = capture();
	const auto ordinaryPassDraws = drawCalls;
	rows[1].Shader = rows[4].Shader = {};
	const auto ordinary = capture();
	REQUIRE_FALSE(CompareImages(expected[0].View(), ordinary[0].View()).Passed());
	REQUIRE_FALSE(CompareImages(expected[1].View(), ordinary[1].View()).Passed());
	rows[1].Shader = rows[4].Shader = shader;
	view.Pipeline = core::Name("fixture.occlusion");
	for (int repeat = 0; repeat < 2; ++repeat) {
		const auto actual = capture();
		CHECK(drawCalls > ordinaryPassDraws);
		CheckImage(
			fixture.Render,
			"authored-occlusion",
			"colour",
			graph::Write(occlusion),
			expected[0].View(),
			actual[0].View()
		);
		CheckImage(
			fixture.Render,
			"authored-occlusion",
			"depth",
			graph::Write(occlusion),
			expected[1].View(),
			actual[1].View()
		);
	}
}

TEST_CASE("a UDIM set draws each tile's texture on its own UV tile", "[render][gpu][udim][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallFixture(fixture.Render);
	// Seven quads in a row; quad k carries UVs inside tile 1001 + k, as a body's tiles do.
	constexpr uint32_t TILES = 7;
	constexpr uint32_t TILE_SIZE = 4096;
	assets::MeshData strip;
	for (uint32_t tile = 0; tile < TILES; ++tile) {
		const float left = -3.5f + float(tile), u = float(tile);
		const auto base = static_cast<uint32_t>(strip.Vertices.size());
		strip.Vertices.push_back({{left, -0.5f, 0}, {0, 0, 1}, {u + 0.02f, 0.98f}});
		strip.Vertices.push_back({{left + 1, -0.5f, 0}, {0, 0, 1}, {u + 0.98f, 0.98f}});
		strip.Vertices.push_back({{left + 1, 0.5f, 0}, {0, 0, 1}, {u + 0.98f, 0.02f}});
		strip.Vertices.push_back({{left, 0.5f, 0}, {0, 0, 1}, {u + 0.02f, 0.02f}});
		for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
			strip.Indices.push_back(base + index);
	}
	strip.ComputeBounds();
	REQUIRE(fixture.Render.AddMesh(core::Name("fixture.udim-strip"), strip));
	// Each tile a distinct solid colour at full tier size.
	const auto colourOf = [](uint32_t tile) {
		return std::array<uint8_t, 4>{
			uint8_t(tile & 1 ? 255 : 30), uint8_t(tile & 2 ? 255 : 30), uint8_t(tile & 4 ? 255 : 30), 255
		};
	};
	std::vector<scene::UdimTile> tiles;
	for (uint32_t tile = 0; tile < TILES; ++tile) {
		// Tile 1001 is the Ultra face tile at 8192; the rest are High tier 4096.
		const uint32_t size = tile == 0 ? 2 * TILE_SIZE : TILE_SIZE;
		assets::TextureData texture;
		texture.Width = texture.Height = size;
		texture.Format = assets::TextureFormat::RGBA8_LINEAR;
		texture.Pixels.resize(size_t(size) * size * 4);
		const auto colour = colourOf(tile + 1);
		for (size_t offset = 0; offset < texture.Pixels.size(); ++offset)
			texture.Pixels[offset] = std::byte(colour[offset % 4]);
		const core::Name name("fixture.udim." + std::to_string(1001 + tile));
		REQUIRE(fixture.Render.AddTexture(name, texture));
		tiles.push_back({uint16_t(1001 + tile), name});
	}
	scene::DrawInstance body;
	body.Source = 1;
	body.Frame.Position = {0, 0, -4};
	body.HalfExtent = {3.5f, 0.5f, 0.01f};
	body.Tint = {1, 1, 1};
	body.Mesh = core::Name("fixture.udim-strip");
	body.Texture = core::Name(scene::ComposeTileSet(tiles));
	body.CastShadow = false;
	render::SceneTarget target{TILES * 16 + 1, 33};
	render::View view;
	view.World = 932;
	view.WorldName = core::Name("world:udim");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32;
	view.Instances = std::span(&body, 1);
	render::OverlayImage overlay;
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Ran(core::Name("gbuffer")));
	const auto albedo = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	// A 90 degree vertical lens at distance 4 sees half-height 4 and half-width 4 * aspect.
	const double halfWidth = 4.0 * target.Width / target.Height;
	for (uint32_t tile = 0; tile < TILES; ++tile) {
		const double world = -3.5 + tile + 0.5;
		const auto x = static_cast<uint32_t>((world / halfWidth * 0.5 + 0.5) * target.Width);
		const size_t offset = size_t(target.Height / 2) * albedo.RowStrideBytes + size_t(x) * 4;
		const auto expected = colourOf(tile + 1);
		INFO("tile " << 1001 + tile << " at column " << x);
		// Bright and dark channels stay apart; the albedo target may store them encoded.
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(
				(std::to_integer<uint8_t>(albedo.Bytes[offset + channel]) > 200) == (expected[channel] == 255)
			);
	}
}

namespace {
	// One plane facing the camera, captured after tonemapping.
	struct PlaneScene {
		FixtureDevice Fixture;
		render::SceneTarget Target{129, 65};
		render::View View;
		render::OverlayImage Overlay;
		scene::DrawInstance Surface;
		std::vector<scene::DrawInstance> Drawn;

		explicit PlaneScene(core::Name texture) {
			Fixture.Initialise();
			InstallFixture(Fixture.Render);
			Surface = DrawPlanes(std::array{Plane{0, 0, 4, 2.5f, 2.5f, 0xFFFFFFFFu}}).front();
			Surface.Texture = texture;
			View.World = 933;
			View.WorldName = core::Name("world:skin");
			View.Pipeline = core::Name("fixture.pbr");
			View.Target = &Target;
			View.Camera.FieldOfViewRadians = 1.5707963267948966f;
			View.Camera.NearPlane = .25f;
			View.Camera.FarPlane = 32;
		}
		// Drawn with the plane, such as a shadow caster.
		std::vector<scene::DrawInstance> Others;
		CapturedImage Capture() {
			Drawn = Others;
			Drawn.push_back(Surface);
			View.Instances = Drawn;
			View.Damage.Scene = true;
			const auto frame = Fixture.Render.Render(std::span(&View, 1), Overlay, nullptr, false);
			REQUIRE(frame.Ran(core::Name("deferred-lighting")));
			return CapturePresentationResource(
				Fixture.Render, core::Name("composed-image"), View.Slot, Target.Width, Target.Height
			);
		}
		std::array<int, 3> At(const CapturedImage &image, uint32_t x, uint32_t y) const {
			const size_t offset = size_t(y) * image.RowStrideBytes + size_t(x) * 4;
			return {
				std::to_integer<int>(image.Bytes[offset]),
				std::to_integer<int>(image.Bytes[offset + 1]),
				std::to_integer<int>(image.Bytes[offset + 2])
			};
		}
	};
}

namespace {
	// Appends a closed surface of revolution about +Y to mesh: a sphere when
	// halfHeight is zero, else a capped cylinder. UVs fill UDIM tile `tile`
	// (1001 + u) with a small inset, as a body's seven tiles do.
	void
	AppendPart(assets::MeshData &mesh, core::Vector3 centre, float radius, float halfHeight, uint32_t tile) {
		constexpr uint32_t AROUND = 24, ALONG = 16;
		const auto base = static_cast<uint32_t>(mesh.Vertices.size());
		const float tileU = float(tile - 1001);
		for (uint32_t row = 0; row <= ALONG; ++row) {
			const float t = float(row) / ALONG;
			for (uint32_t column = 0; column <= AROUND; ++column) {
				const float angle = float(column) / AROUND * 6.2831853f;
				core::Vector3 normal, position;
				if (halfHeight == 0.0f) {
					const float polar = t * 3.14159265f;
					normal = {
						std::sin(polar) * std::sin(angle), std::cos(polar), std::sin(polar) * std::cos(angle)
					};
					position = centre + normal * radius;
				} else {
					normal = {std::sin(angle), 0.0f, std::cos(angle)};
					position =
						centre + normal * radius + core::Vector3{0.0f, halfHeight * (1.0f - 2.0f * t), 0.0f};
				}
				mesh.Vertices.push_back(
					{{position.X, position.Y, position.Z},
					 {normal.X, normal.Y, normal.Z},
					 {tileU + 0.02f + 0.96f * float(column) / AROUND, 0.02f + 0.96f * t}}
				);
			}
		}
		for (uint32_t row = 0; row < ALONG; ++row)
			for (uint32_t column = 0; column < AROUND; ++column) {
				const uint32_t a = base + row * (AROUND + 1) + column, b = a + 1, c = a + AROUND + 1,
							   d = c + 1;
				// Counter-clockwise seen from outside, matching PlaneMesh.
				for (const uint32_t index : {a, c, b, b, c, d})
					mesh.Indices.push_back(index);
			}
	}

	// Centres a mesh on its bounds so an instance's HalfExtent places it unscaled.
	core::Vector3 Centre(assets::MeshData &mesh) {
		mesh.ComputeBounds();
		const core::Vector3 middle = (mesh.Minimum + mesh.Maximum) * 0.5f;
		for (auto &vertex : mesh.Vertices) {
			vertex.Position[0] -= middle.X;
			vertex.Position[1] -= middle.Y;
			vertex.Position[2] -= middle.Z;
		}
		mesh.ComputeBounds();
		return middle;
	}
}

TEST_CASE("a body with seven UDIM tiles per slot shows each tile on its part", "[render][gpu][udim][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallFixture(fixture.Render);
	struct Part {
		core::Vector3 Centre;
		float Radius;
		float HalfHeight;
	};
	// Head 1001, torso 1002, arms 1003 and 1004, legs 1005 and 1006, right hand 1007.
	const std::array<Part, 7> parts{{
		{{0.0f, 1.55f, 0.0f}, 0.24f, 0.0f},
		{{0.0f, 0.75f, 0.0f}, 0.3f, 0.45f},
		{{-0.55f, 0.85f, 0.0f}, 0.1f, 0.38f},
		{{0.55f, 0.85f, 0.0f}, 0.1f, 0.38f},
		{{-0.16f, -0.25f, 0.0f}, 0.12f, 0.5f},
		{{0.16f, -0.25f, 0.0f}, 0.12f, 0.5f},
		{{0.55f, 0.32f, 0.0f}, 0.12f, 0.0f},
	}};
	assets::MeshData body;
	for (uint32_t index = 0; index < parts.size(); ++index)
		AppendPart(body, parts[index].Centre, parts[index].Radius, parts[index].HalfHeight, 1001 + index);
	const core::Vector3 middle = Centre(body);
	REQUIRE(fixture.Render.AddMesh(core::Name("fixture.body"), body));
	const auto colourOf = [](uint32_t tile) {
		return std::array<uint8_t, 4>{
			uint8_t(tile & 1 ? 255 : 20), uint8_t(tile & 2 ? 255 : 20), uint8_t(tile & 4 ? 255 : 20), 255
		};
	};
	// Colour and normal sets; tile 1001 is the Ultra 8192 face tile, the rest 4096.
	std::vector<scene::UdimTile> colours, normals;
	for (uint32_t tile = 0; tile < parts.size(); ++tile) {
		const uint32_t size = tile == 0 ? 8192 : 4096;
		assets::TextureData texture;
		texture.Width = texture.Height = size;
		texture.Format = assets::TextureFormat::RGBA8_LINEAR;
		texture.Pixels.resize(size_t(size) * size * 4);
		const auto colour = colourOf(tile + 1);
		for (size_t offset = 0; offset < texture.Pixels.size(); ++offset)
			texture.Pixels[offset] = std::byte(colour[offset % 4]);
		const core::Name colourName("fixture.body.albedo." + std::to_string(1001 + tile));
		REQUIRE(fixture.Render.AddTexture(colourName, texture));
		colours.push_back({uint16_t(1001 + tile), colourName});
		const std::array<uint8_t, 4> flat{128, 128, 255, 255};
		for (size_t offset = 0; offset < texture.Pixels.size(); ++offset)
			texture.Pixels[offset] = std::byte(flat[offset % 4]);
		const core::Name normalName("fixture.body.normal." + std::to_string(1001 + tile));
		REQUIRE(fixture.Render.AddTexture(normalName, texture));
		normals.push_back({uint16_t(1001 + tile), normalName});
	}
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = core::Vector3{0.0f, 0.0f, -3.0f} + middle;
	instance.HalfExtent = (body.Maximum - body.Minimum) * 0.5f;
	instance.Tint = {1, 1, 1};
	instance.Mesh = core::Name("fixture.body");
	instance.Texture = core::Name(scene::ComposeTileSet(colours));
	instance.NormalMap = core::Name(scene::ComposeTileSet(normals));
	instance.CastShadow = false;
	render::SceneTarget target{257, 193};
	render::View view;
	view.World = 934;
	view.WorldName = core::Name("world:body");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = .25f;
	view.Camera.FarPlane = 32;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("gbuffer")));
	const auto albedo = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	// A 90 degree vertical lens at distance 3 maps a world unit to half the height over 3.
	const double scale = target.Height * 0.5 / 3.0;
	for (uint32_t tile = 0; tile < parts.size(); ++tile) {
		// The front of each part, nearest the camera.
		const auto &part = parts[tile];
		const double depth = 3.0 - part.Radius;
		const double perUnit = target.Height * 0.5 / depth;
		const auto x = static_cast<uint32_t>(target.Width * 0.5 + part.Centre.X * perUnit);
		const auto y = static_cast<uint32_t>(target.Height * 0.5 - part.Centre.Y * perUnit);
		const size_t offset = size_t(y) * albedo.RowStrideBytes + size_t(x) * 4;
		const auto expected = colourOf(tile + 1);
		INFO("tile " << 1001 + tile << " at " << x << "," << y << " (scale " << scale << ")");
		for (size_t channel = 0; channel < 3; ++channel)
			CHECK(
				(std::to_integer<uint8_t>(albedo.Bytes[offset + channel]) > 200) == (expected[channel] == 255)
			);
	}
}

TEST_CASE(
	"authored surfaces draw in the G-buffer with their own normals and light", "[render][gpu][skin][.]"
) {
	PlaneScene plane(core::Name("fixture.white"));
	scene::WorldLighting sun;
	sun.Direction = core::Vector3{0.0f, 0.0f, -1.0f};
	sun.Direct = {0.5f, 0.5f, 0.5f};
	sun.Ambient = {0, 0, 0};
	sun.OutdoorAmbient = {0, 0, 0};
	plane.Fixture.Render.SetLighting(sun);
	plane.View.Lighting = sun;
	const auto install = [&](const char *name, std::string source) {
		const auto assembled = render::AssembleShader(name, source, {});
		REQUIRE(assembled.Error.empty());
		render::ShaderCompiler compiler;
		compiler.SetOptimise(true);
		const auto forward = compiler.Compile(assembled.Forward, render::ShaderStage::Fragment, name);
		const auto gbuffer = compiler.Compile(assembled.GBuffer, render::ShaderStage::Fragment, name);
		INFO(forward.Error << gbuffer.Error);
		REQUIRE_FALSE(forward.Failed);
		REQUIRE_FALSE(gbuffer.Failed);
		REQUIRE(plane.Fixture.Render.AddMaterialShader(core::Name(name), forward.SpirV, {}, gbuffer.SpirV));
	};
	install(
		"surface.red", "void Surface(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = vec3(1, 0, 0); }"
	);
	install(
		"surface.flat",
		"void Surface(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = vec3(0, 0, 1);"
		" o.Normal = normalize(vec3(1, 0, 1)); o.User = 0.5; }\n"
		"vec3 Light(SurfaceOutput s, LightSample l) { return vec3(0, l.IsSun ? 1.0 : 0.0, 0) * l.Colour; }"
	);
	const uint32_t x = plane.Target.Width / 2, y = plane.Target.Height / 2;
	const auto plain = plane.At(plane.Capture(), x, y);
	CHECK(plain[0] == plain[1]);

	// Surface only: the engine lights it.
	plane.Surface.Shader = core::Name("surface.red");
	const auto red = plane.At(plane.Capture(), x, y);
	CHECK(red[0] > 60);
	CHECK(red[1] < red[0] / 3);

	// Surface and Light: the author lights it, and the G-buffer keeps the surface.
	plane.Surface.Shader = core::Name("surface.flat");
	const auto lit = plane.At(plane.Capture(), x, y);
	CHECK(lit[1] > 60);
	CHECK(lit[0] < 10);
	CHECK(lit[2] < 10);
	// Its G-buffer normal is the author's, and the marker tells deferred lighting
	// to leave the pixel alone.
	const auto normal = CaptureResource(
		plane.Fixture.Render,
		core::Name("normal"),
		plane.View.Slot,
		plane.Target.Width,
		plane.Target.Height,
		ImageFormat::Rgba8Unorm
	);
	uint32_t texel = 0;
	std::memcpy(&texel, normal.Bytes.data() + size_t(y) * normal.RowStrideBytes + size_t(x) * 4, 4);
	const auto axis = [&](uint32_t shift) { return float(texel >> shift & 1023u) / 1023.0f * 2.0f - 1.0f; };
	CHECK(std::abs(axis(0) - 0.7071f) < 0.01f);
	CHECK(std::abs(axis(20) - 0.7071f) < 0.01f);
	CHECK((texel >> 30) == 1u);

	// The forward module draws the same light when the surface is transparent.
	plane.Surface.Transparency = 0.02f;
	const auto blended = plane.At(plane.Capture(), x, y);
	CHECK(blended[1] > 60);
	CHECK(blended[2] < 20);
}

TEST_CASE("an authored surface keeps the part's ids and mesh coordinates", "[render][gpu][material][.]") {
	PlaneScene plane(core::Name("fixture.white"));
	const auto assembled = render::AssembleShader(
		"ids",
		"void Surface(SurfaceInput i, inout SurfaceOutput o) { o.Albedo = vec3(1, 0, 0); }\n"
		"vec3 Light(SurfaceOutput s, LightSample l) { return s.Albedo * l.Colour; }\n",
		{}
	);
	REQUIRE(assembled.Error.empty());
	render::ShaderCompiler compiler;
	compiler.SetOptimise(true);
	const auto forward = compiler.Compile(assembled.Forward, render::ShaderStage::Fragment, "ids");
	const auto gbuffer = compiler.Compile(assembled.GBuffer, render::ShaderStage::Fragment, "ids");
	REQUIRE_FALSE(gbuffer.Failed);
	REQUIRE(plane.Fixture.Render.AddMaterialShader(core::Name("ids"), forward.SpirV, {}, gbuffer.SpirV));
	plane.Surface.ObjectLabel = 41;
	plane.Surface.SemanticLabel = 42;
	plane.Surface.PartLabel = 43;
	const auto ids = [&] {
		(void)plane.Capture();
		std::array<CapturedImage, 3> held;
		const std::array names{"object-ids", "semantic-ids", "part-ids"};
		for (size_t index = 0; index < names.size(); ++index)
			held[index] = CaptureResource(
				plane.Fixture.Render,
				core::Name(names[index]),
				plane.View.Slot,
				plane.Target.Width,
				plane.Target.Height,
				ImageFormat::R32Float
			);
		return held;
	};
	const auto engine = ids();
	uint32_t centre = 0;
	std::memcpy(
		&centre,
		engine[0].Bytes.data() + size_t(plane.Target.Height / 2) * engine[0].RowStrideBytes +
			size_t(plane.Target.Width / 2) * 4,
		4
	);
	CHECK(centre != 0);
	plane.Surface.Shader = core::Name("ids");
	const auto authored = ids();
	for (size_t index = 0; index < engine.size(); ++index) {
		INFO("plane " << index);
		CHECK(engine[index].Bytes == authored[index].Bytes);
	}
}

TEST_CASE("authored parameters and textures come from the part", "[render][gpu][material][.]") {
	PlaneScene plane(core::Name("fixture.white"));
	const std::string source =
		"#pragma parameter vec3 tint = 0, 1, 0\n"
		"#pragma parameter float gain = 1\n"
		"#pragma texture mask\n"
		"void Surface(SurfaceInput i, inout SurfaceOutput o) {}\n"
		"vec3 Light(SurfaceOutput s, LightSample l) {\n"
		"	if (!l.IsSun) return vec3(0);\n"
		"	return tint * gain * (maskBound() ? texture(mask, vec2(0.5)).rgb : vec3(1));\n"
		"}\n";
	const auto assembled = render::AssembleShader("values", source, {});
	REQUIRE(assembled.Error.empty());
	render::ShaderCompiler compiler;
	compiler.SetOptimise(true);
	const auto forward = compiler.Compile(assembled.Forward, render::ShaderStage::Fragment, "values");
	const auto gbuffer = compiler.Compile(assembled.GBuffer, render::ShaderStage::Fragment, "values");
	INFO(forward.Error << gbuffer.Error);
	REQUIRE_FALSE(gbuffer.Failed);
	REQUIRE(plane.Fixture.Render.AddMaterialShader(core::Name("values"), forward.SpirV, {}, gbuffer.SpirV));
	scene::WorldLighting dark;
	dark.Ambient = {0, 0, 0};
	dark.OutdoorAmbient = {0, 0, 0};
	plane.Fixture.Render.SetLighting(dark);
	plane.View.Lighting = dark;
	plane.Surface.Shader = core::Name("values");
	const uint32_t x = plane.Target.Width / 2, y = plane.Target.Height / 2;
	const auto defaults = plane.At(plane.Capture(), x, y);
	CHECK(defaults[1] > 100);
	CHECK(defaults[0] < 10);

	const auto layout = scene::ParseMaterialLayout(source);
	scene::SurfaceAppearance appearance;
	const auto set = [&](const char *name, std::vector<float> values) {
		REQUIRE(scene::SetMaterialParameter(appearance, *layout.Find(name), values.data(), values.size()));
		std::copy(
			std::begin(appearance.MaterialParameters),
			std::end(appearance.MaterialParameters),
			std::begin(plane.Surface.MaterialParameters)
		);
		plane.Surface.MaterialParameterMask = appearance.MaterialParameterMask;
	};
	set("tint", {1, 0, 0});
	const auto red = plane.At(plane.Capture(), x, y);
	CHECK(red[0] > 100);
	CHECK(red[1] < 10);
	// A zero that was set is a zero, not the default.
	set("gain", {0});
	CHECK(plane.At(plane.Capture(), x, y)[0] < 10);
	set("gain", {});
	CHECK(plane.At(plane.Capture(), x, y)[0] > 100);

	assets::TextureData blue;
	blue.Width = blue.Height = 1;
	blue.Format = assets::TextureFormat::RGBA8;
	blue.Pixels = {std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
	REQUIRE(plane.Fixture.Render.AddTexture(core::Name("values.blue"), blue));
	plane.Surface.MaterialTexture0 = core::Name("values.blue");
	set("tint", {1, 1, 1});
	const auto masked = plane.At(plane.Capture(), x, y);
	CHECK(masked[2] > 100);
	CHECK(masked[0] < 10);
}

TEST_CASE("authored passes chain, read the user tag and run on demand", "[render][gpu][material][.]") {
	PlaneScene plane(core::Name("fixture.white"));
	render::ShaderCompiler compiler;
	compiler.SetOptimise(true);
	const auto compile = [&](const char *name, const char *source) {
		const auto assembled = render::AssembleShader(name, source, {});
		REQUIRE(assembled.Error.empty());
		auto compiled = compiler.Compile(assembled.Forward, render::ShaderStage::Fragment, name);
		INFO(compiled.Error);
		REQUIRE_FALSE(compiled.Failed);
		return std::pair(assembled, compiled);
	};
	const char *const tagged = "void Surface(SurfaceInput i, inout SurfaceOutput o) { o.User = 0.5; }\n"
							   "vec3 Light(SurfaceOutput s, LightSample l) { return vec3(0); }\n";
	const auto [surface, forward] = compile("tagged", tagged);
	const auto gbuffer = compiler.Compile(surface.GBuffer, render::ShaderStage::Fragment, "tagged");
	REQUIRE(plane.Fixture.Render.AddMaterialShader(core::Name("tagged"), forward.SpirV, {}, gbuffer.SpirV));
	const auto add = [&](const char *name, const char *source) {
		const auto [assembled, compiled] = compile(name, source);
		CHECK(assembled.Pass);
		REQUIRE(plane.Fixture.Render.AddPassShader(core::Name(name), compiled.SpirV));
	};
	add("first", "vec4 Pass(PassInput i) { return vec4(User(i.UV) > 0.4 ? 1.0 : 0.0, 0, 0, 1); }");
	add("second",
		"#pragma parameter float gain = 1\n"
		"vec4 Pass(PassInput i) { return vec4(0, 0, Previous(i.UV).r * gain, Previous(i.UV).r); }");
	scene::WorldLighting lighting;
	lighting.Ambient = lighting.OutdoorAmbient = {0, 0, 0};
	lighting.Passes[0] = {
		.Shader = core::Name("first"), .Demand = core::Name("tagged"), .Scale = 0.5f, .Composite = false
	};
	lighting.Passes[1] = {.Shader = core::Name("second"), .Demand = core::Name("tagged")};
	lighting.PassCount = 2;
	const auto light = [&] {
		plane.Fixture.Render.SetLighting(lighting);
		plane.View.Lighting = lighting;
	};
	light();
	const uint32_t x = plane.Target.Width / 2, y = plane.Target.Height / 2;
	// Nothing drawn asks for them.
	const auto idle = plane.At(plane.Capture(), x, y);
	CHECK(idle[0] == idle[2]);
	plane.Surface.Shader = core::Name("tagged");
	const auto blue = plane.At(plane.Capture(), x, y);
	CHECK(blue[2] > 100);
	CHECK(blue[0] < 10);
	lighting.Passes[1].Parameters[0] = 0.0f;
	lighting.Passes[1].ParameterMask = 1;
	light();
	CHECK(plane.At(plane.Capture(), x, y)[2] < 10);
}

TEST_CASE("a UDIM tile whose graph output is not ready draws the default", "[render][gpu][udim][.]") {
	PlaneScene plane(core::Name("fixture.white"));
	scene::WorldLighting bright;
	bright.Ambient = {1, 1, 1};
	bright.Direct = {0, 0, 0};
	plane.Fixture.Render.SetLighting(bright);
	plane.View.Lighting = bright;
	plane.Surface.Texture = core::Name("udim:1001=imagegraph-instance://pending#albedo");
	const auto image = plane.Capture();
	const auto centre = plane.At(image, plane.Target.Width / 2, plane.Target.Height / 2);
	INFO(centre[0] << " " << centre[1] << " " << centre[2]);
	// Neither the discarded background nor the magenta marker.
	CHECK(centre[1] > 100);
	CHECK(std::abs(centre[0] - centre[1]) < 20);
}

TEST_CASE("a material texture takes a UDIM set", "[render][gpu][material][udim][.]") {
	PlaneScene body(core::Name("fixture.white"));
	scene::WorldLighting dark;
	dark.Direct = {0, 0, 0};
	dark.Ambient = {0, 0, 0};
	dark.OutdoorAmbient = {0, 0, 0};
	body.Fixture.Render.SetLighting(dark);
	body.View.Lighting = dark;
	const auto solid = [](uint8_t value) {
		assets::TextureData map;
		map.Width = map.Height = 1;
		map.Format = assets::TextureFormat::RGBA8_LINEAR;
		map.Pixels.assign(4, std::byte{value});
		return map;
	};
	REQUIRE(body.Fixture.Render.AddTexture(core::Name("thin"), solid(8)));
	REQUIRE(body.Fixture.Render.AddTexture(core::Name("thick"), solid(255)));
	// Two quads: tile 1001 on the left and 1002 on the right.
	assets::MeshData strip;
	for (uint32_t tile = 0; tile < 2; ++tile) {
		const float left = -1.0f + float(tile), u = float(tile);
		const auto base = static_cast<uint32_t>(strip.Vertices.size());
		strip.Vertices.push_back({{left, -1, 0}, {0, 0, 1}, {u + .1f, .9f}});
		strip.Vertices.push_back({{left + 1, -1, 0}, {0, 0, 1}, {u + .9f, .9f}});
		strip.Vertices.push_back({{left + 1, 1, 0}, {0, 0, 1}, {u + .9f, .1f}});
		strip.Vertices.push_back({{left, 1, 0}, {0, 0, 1}, {u + .1f, .1f}});
		for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
			strip.Indices.push_back(base + index);
	}
	strip.ComputeBounds();
	REQUIRE(body.Fixture.Render.AddMesh(core::Name("fixture.strip"), strip));
	body.Surface.Mesh = core::Name("fixture.strip");
	body.Surface.HalfExtent = {1, 1, 0.01f};
	body.Surface.Frame.Position = {0, 0, -2};
	const auto assembled = render::AssembleShader(
		"tiled",
		"#pragma texture amount\n"
		"void Surface(SurfaceInput i, inout SurfaceOutput o) { o.Custom[0] = texture(amount, i.UV); }\n"
		"vec3 Light(SurfaceOutput s, LightSample l) { return l.IsSun ? s.Custom[0].rrr : vec3(0); }\n",
		{}
	);
	REQUIRE(assembled.Error.empty());
	render::ShaderCompiler compiler;
	compiler.SetOptimise(true);
	const auto forward = compiler.Compile(assembled.Forward, render::ShaderStage::Fragment, "tiled");
	const auto gbuffer = compiler.Compile(assembled.GBuffer, render::ShaderStage::Fragment, "tiled");
	REQUIRE_FALSE(gbuffer.Failed);
	REQUIRE(body.Fixture.Render.AddMaterialShader(core::Name("tiled"), forward.SpirV, {}, gbuffer.SpirV));
	body.Surface.Shader = core::Name("tiled");
	body.Surface.MaterialTexture0 = core::Name("udim:1001=thin|1002=thick");
	const auto image = body.Capture();
	const uint32_t y = body.Target.Height / 2, x = body.Target.Width / 2;
	const auto thin = body.At(image, x - 8, y), thick = body.At(image, x + 8, y);
	INFO("thin " << thin[0] << " thick " << thick[0]);
	CHECK(thin[0] < 80);
	CHECK(thick[0] > thin[0] + 60);
}
