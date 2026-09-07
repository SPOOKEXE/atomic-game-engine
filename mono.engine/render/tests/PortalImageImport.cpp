#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalImageImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <iostream>

TEST_SUITE_ID("engine.render.portalimageimport")
TEST_DEPENDS("engine.render.imagecomparison")
TEST_DEPENDS("engine.render.portalexchange")

namespace {
	uint8_t DisplayCode(double radiance);
}

TEST_CASE("whole-eye images fill the viewport and reject a different owner", "[render][gpu][eye-image][.]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const auto document = graph::DefaultEyeDocument();
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(core::Name("eye-test"), pipeline));
	render::PortalImageBinding binding;
	binding.World = 7;
	binding.WorldName = core::Name("source");
	binding.Portal = core::Name("player-eye");
	binding.Expected = {1, "player-eye", 2, 3};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = reply.Height = 1;
	reply.RowStride = 8;
	reply.Pixels = {
		std::byte{0},
		std::byte{0x40},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0x3c}
	};
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	render::View view;
	view.World = binding.World;
	view.WorldName = binding.WorldName;
	view.EyeImageKey = binding.Portal;
	view.EyeImage = fixture.Render.QueuePortalImage(binding, std::move(reply));
	REQUIRE(view.EyeImage != 0);
	view.Pipeline = core::Name("eye-test");
	render::SceneTarget target{17, 11};
	view.Target = &target;
	render::OverlayImage overlay;
	const auto format = static_cast<SDL_GPUTextureFormat>(fixture.Render.Backend().ColourFormat);
	const bool bgra =
		format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
	const bool srgb = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB ||
					  format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
	REQUIRE(
		(bgra || format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
		 format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB)
	);
	const int expected =
		srgb ? DisplayCode(2)
			 : int(std::lround(
				   std::pow((2.0 * (2.51 * 2 + .03)) / (2.0 * (2.43 * 2 + .59) + .14), 1 / 2.2) * 255
			   ));
	const auto owned = view;
	for (int mismatch : {0, 1, 2, 3, 4, 5, 0}) {
		view = owned;
		if (mismatch == 1) view.WorldName = core::Name("another-world");
		if (mismatch == 2) view.World++;
		if (mismatch == 3) view.Slot++;
		if (mismatch == 4) view.EyeImageKey = core::Name("another-eye");
		if (mismatch == 5) view.EyeImage++;
		CAPTURE(mismatch);
		(void)fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto image = render::test::CaptureResource(
			fixture.Render,
			core::Name("composed-image"),
			view.Slot,
			17,
			11,
			render::test::ImageFormat::Rgba8Unorm
		);
		for (const auto [x, y] : {std::pair{0, 0}, std::pair{8, 5}, std::pair{16, 10}}) {
			const auto *pixel = image.Bytes.data() + y * image.RowStrideBytes + x * 4;
			CHECK(std::abs(std::to_integer<int>(pixel[bgra ? 2 : 0]) - (mismatch ? 0 : expected)) <= 2);
			CHECK(pixel[1] == std::byte{0});
			CHECK(pixel[bgra ? 0 : 2] == std::byte{0});
		}
	}
}

namespace {
	using namespace engine;
	using namespace engine::render::test;
	constexpr uint32_t WIDTH = 97;
	constexpr uint32_t HEIGHT = 65;
	constexpr uint32_t IMAGE_SIZE = 32;

	graph::PipelineDocument Install(render::Renderer &renderer) {
		auto document = graph::DefaultPbrDocument();
		const core::Name boundary("import-fixture-boundary");
		graph::NodeKindSpec spec;
		spec.Kind = boundary;
		spec.Scope = graph::NodeScope::Frame;
		spec.Queue = graph::ExecutionQueue::Cpu;
		spec.Category = graph::NodeCategory::Output;
		for (const char *resource : {"tonemapped", "portaled", "composed-image"}) {
			spec.Inputs.push_back({.Name = core::Name(resource), .Kind = graph::ResourceKind::Texture});
		}
		REQUIRE(graph::RegisterNodeKind(std::move(spec)));
		REQUIRE(renderer.InstallNodeHandler(boundary, [](const graph::RunContext &) { return true; }));
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = boundary,
			 .NodeKind = boundary,
			 .Scope = graph::NodeScope::Frame}
		);
		for (const char *resource : {"tonemapped", "portaled", "composed-image"}) {
			document.Record(
				{.Kind = graph::EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(resource)}
			);
		}
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("import.fixture"), graph));
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-.5f, -.5f, 0}, {0, 0, 1}, {0, 1}},
			{{.5f, -.5f, 0}, {0, 0, 1}, {1, 1}},
			{{.5f, .5f, 0}, {0, 0, 1}, {1, 0}},
			{{-.5f, .5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		REQUIRE(renderer.AddMesh(core::Name("import.plane"), mesh));
		assets::TextureData white;
		white.Width = white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(core::Name("import.white"), white));
		renderer.SetPortalDepth(1);
		return document;
	}

	// All source channels are exactly representable binary16 radiance.
	constexpr std::array<std::array<uint16_t, 4>, 4> HALF_COLOURS{{
		{0x3800, 0x2c00, 0x2800, 0x3c00},
		{0x2800, 0x3800, 0x2c00, 0x3c00},
		{0x2800, 0x2c00, 0x3800, 0x3c00},
		{0x3800, 0x3800, 0x2800, 0x3c00},
	}};
	constexpr std::array<std::array<double, 3>, 4> RADIANCE{{
		{.5, .0625, .03125},
		{.03125, .5, .0625},
		{.03125, .0625, .5},
		{.5, .5, .03125},
	}};

	render::PortalImageReply Image(const render::PortalExchangeKey &key) {
		render::PortalImageReply reply;
		reply.Key = key;
		reply.Status = render::PortalImageStatus::Ok;
		reply.Width = reply.Height = IMAGE_SIZE;
		reply.RowStride = IMAGE_SIZE * 8;
		reply.Pixels.resize(IMAGE_SIZE * IMAGE_SIZE * 8);
		for (uint32_t y = 0; y < IMAGE_SIZE; y++) {
			for (uint32_t x = 0; x < IMAGE_SIZE; x++) {
				const auto &colour =
					HALF_COLOURS[(y >= IMAGE_SIZE / 2 ? 2 : 0) + (x >= IMAGE_SIZE / 2 ? 1 : 0)];
				for (size_t channel = 0; channel < 4; channel++) {
					const size_t at = (size_t(y) * IMAGE_SIZE + x) * 8 + channel * 2;
					reply.Pixels[at] = std::byte(colour[channel] & 255);
					reply.Pixels[at + 1] = std::byte(colour[channel] >> 8);
				}
			}
		}
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		return reply;
	}

	uint8_t DisplayCode(double radiance) {
		const double aces = (radiance * (2.51 * radiance + .03)) / (radiance * (2.43 * radiance + .59) + .14);
		const double display = std::pow(aces, 1 / 2.2);
		// The graph's portaled attachment stores sRGB; the display shader writes
		// its encoded result into that attachment, just as the direct pipeline does.
		const double stored =
			display <= .0031308 ? 12.92 * display : 1.055 * std::pow(display, 1 / 2.4) - .055;
		return static_cast<uint8_t>(std::lround(stored * 255));
	}

	void CheckImported(
		render::Renderer &renderer,
		const graph::PipelineDocument &document,
		const CapturedImage &actual,
		const CapturedImage &before
	) {
		std::vector<uint32_t> expected(WIDTH * HEIGHT);
		std::vector<uint32_t> observed(expected.size());
		size_t inside = 0;
		for (uint32_t y = 0; y < HEIGHT; y++) {
			for (uint32_t x = 0; x < WIDTH; x++) {
				const double worldX =
					(2 * (x + .5) / WIDTH - 1) * std::tan(1.0471975512 / 2) * WIDTH / HEIGHT * 4;
				const double worldY = (1 - 2 * (y + .5) / HEIGHT) * std::tan(1.0471975512 / 2) * 4;
				// Aperture raster edges and the fixed texel-filter footprint around
				// quadrant boundaries are excluded by geometry, never by observed colour.
				if (std::abs(std::abs(worldX) - 1.5) < .08 || std::abs(std::abs(worldY) - 1.2) < .08 ||
					std::abs(worldX) < .16 || std::abs(worldY) < .16) {
					continue;
				}
				const size_t pixel = size_t(y) * WIDTH + x;
				if (std::abs(worldX) < 1.5 && std::abs(worldY) < 1.2) {
					const auto &colour = RADIANCE[(worldY < 0 ? 2 : 0) + (worldX > 0 ? 1 : 0)];
					expected[pixel] = uint32_t(DisplayCode(colour[0])) |
									  (uint32_t(DisplayCode(colour[1])) << 8) |
									  (uint32_t(DisplayCode(colour[2])) << 16) | 0xff000000;
					inside++;
				} else {
					std::memcpy(&expected[pixel], before.Bytes.data() + y * before.RowStrideBytes + x * 4, 4);
				}
				std::memcpy(&observed[pixel], actual.Bytes.data() + y * actual.RowStrideBytes + x * 4, 4);
			}
		}
		REQUIRE(inside > 500);
		ImageTolerance tolerance;
		tolerance.Absolute = 2.0 / 255;
		const auto comparison = CompareImages(
			{WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(expected))},
			{WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(observed))},
			tolerance
		);
		std::cout << "portal import inside=" << inside << " max=" << comparison.MaximumAbsoluteError
				  << " rmse=" << comparison.RootMeanSquareError
				  << " mismatched=" << comparison.MismatchedPixels << '\n';
		CheckImage(
			renderer,
			"imported-quadrants",
			"portaled",
			"image=32x32 linear half quadrants; fitted image bounds=-2,2,-2,2; aperture=1.5,1.2; "
			"eye=0,0,4\n" +
				graph::Write(document),
			{WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(expected))},
			{WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(observed))},
			tolerance
		);
	}
}

TEST_CASE(
	"owned portal images upload once and use fitted untinted sampling",
	"[render][gpu][fixture][portal-import][.]"
) {
	const bool pairedDepth = GENERATE(false, true);
	CAPTURE(pairedDepth);
	const auto makeImage = [&](const render::PortalExchangeKey &key) {
		auto image = Image(key);
		if (pairedDepth) {
			core::ByteWriter samples;
			for (size_t i = 0; i < size_t(image.Width) * image.Height; ++i)
				samples.WriteFloat(4);
			image.Depth.assign(samples.Bytes().begin(), samples.Bytes().end());
			image.DepthHash = assets::Hasher::Of(image.Depth);
		}
		return image;
	};
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = Install(fixture.Render);
	fixture.Render.SetPortalDepth(0);
	render::SceneTarget target{WIDTH, HEIGHT};
	scene::DrawInstance pane;
	pane.Source = 1;
	pane.Mesh = core::Name("import.plane");
	pane.Texture = core::Name("import.white");
	pane.HalfExtent = {1.5f, 1.2f, .01f};
	pane.Tint = {.1f, .1f, .1f};
	pane.Surface = 0;
	pane.CastShadow = false;
	render::PortalView portal;
	portal.ExternalImage = true;
	portal.ImagePortal = core::Name("import.entrance");
	portal.Normal = {0, 0, 1};
	portal.First = {1.5f, 0, 0};
	portal.Second = {0, 1.2f, 0};
	portal.Warp.Frame.Position = {100, 0, 0};
	render::View view;
	view.World = 930;
	view.WorldName = core::Name("import.owner");
	view.Slot = 0;
	view.Pipeline = core::Name("import.fixture");
	view.Target = &target;
	view.Instances = std::span(&pane, 1);
	view.Portals = std::span(&portal, 1);
	view.CameraFrame.Position = {0, 0, 4};
	view.Camera.FieldOfViewRadians = 1.0471975512f;
	view.Camera.NearPlane = .1f;
	view.Camera.FarPlane = 64;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {.25f, .25f, .25f};
	view.Lighting.Direct = {};
	render::PortalImageBinding binding;
	binding.World = view.World;
	binding.WorldName = view.WorldName;
	binding.Portal = portal.ImagePortal;
	binding.Expected = {1, "import.entrance", 1, 1};
	binding.Sampling = glm::ortho(-2.f, 2.f, -2.f, 2.f, -1.f, 1.f);
	auto reply = makeImage(binding.Expected);
	portal.ImportedImage = fixture.Render.QueuePortalImage(binding, std::move(reply));
	REQUIRE(portal.ImportedImage != 0);
	CHECK(fixture.Render.PortalImageUsage().Uploads == 0);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == 0);
	CHECK(
		fixture.Render.PortalImageUsage().PendingCpuBytes == IMAGE_SIZE * IMAGE_SIZE * (pairedDepth ? 12 : 8)
	);
	render::OverlayImage overlay;
	SECTION("successful graph") {}
	SECTION("valid upload followed by graph failure") {
		REQUIRE(fixture.Render.InstallNodeHandler(
			core::Name("import-fixture-boundary"), [](const graph::RunContext &) { return false; }
		));
	}
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(fixture.Render.InstallNodeHandler(
		core::Name("import-fixture-boundary"), [](const graph::RunContext &) { return true; }
	));
	CHECK(frame.PortalPasses == 0);
	REQUIRE(frame.SurfaceInstances > 0);
	CHECK(fixture.Render.PortalImageUsage().Uploads == (pairedDepth ? 2 : 1));
	CHECK(
		fixture.Render.PortalImageUsage().UploadedBytes == IMAGE_SIZE * IMAGE_SIZE * (pairedDepth ? 12 : 8)
	);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == IMAGE_SIZE * IMAGE_SIZE * (pairedDepth ? 12 : 8));
	const auto before =
		CaptureResource(fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm);
	const auto actual =
		CaptureResource(fixture.Render, core::Name("portaled"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm);
	CheckImported(fixture.Render, document, actual, before);
	reply = makeImage(binding.Expected);
	CHECK(fixture.Render.QueuePortalImage(binding, std::move(reply)) == portal.ImportedImage);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(fixture.Render.PortalImageUsage().Uploads == (pairedDepth ? 2 : 1));
	CHECK(fixture.Render.PortalImageUsage().Reuses == 1);
	if (pairedDepth) {
		auto invalidDepth = makeImage(binding.Expected);
		invalidDepth.Depth.pop_back();
		CHECK(fixture.Render.QueuePortalImage(binding, std::move(invalidDepth)) == 0);
		CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
		auto changed = makeImage(binding.Expected);
		changed.Depth[3] = std::byte{0x41};
		changed.DepthHash = assets::Hasher::Of(changed.Depth);
		const auto previous = portal.ImportedImage;
		portal.ImportedImage = fixture.Render.QueuePortalImage(binding, std::move(changed));
		REQUIRE(portal.ImportedImage != 0);
		CHECK(portal.ImportedImage != previous);
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(fixture.Render.PortalImageUsage().Uploads == 4);
		CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	}
	for (const int mismatch : {0, 1, 2, 3}) {
		auto other = view;
		auto otherPortal = portal;
		if (mismatch == 0) {
			other.World++;
		}
		if (mismatch == 1) {
			other.WorldName = core::Name("import.other-world");
		}
		if (mismatch == 2) {
			other.Slot = 1;
		}
		if (mismatch == 3) {
			otherPortal.ImagePortal = core::Name("import.other-entrance");
		}
		other.Portals = std::span(&otherPortal, 1);
		const auto refused = fixture.Render.Render(std::span(&other, 1), overlay, nullptr, false);
		CHECK(refused.SurfaceInstances == 0);
		CHECK(refused.PortalPasses == 0);
	}
	reply = makeImage(binding.Expected);
	reply.Key.CameraRevision++;
	CHECK(fixture.Render.QueuePortalImage(binding, std::move(reply)) == 0);
	const auto oldHandle = portal.ImportedImage;
	binding.Expected.CameraRevision++;
	reply = makeImage(binding.Expected);
	portal.ImportedImage = fixture.Render.QueuePortalImage(binding, std::move(reply));
	REQUIRE(portal.ImportedImage != oldHandle);
	CHECK_FALSE(fixture.Render.DropPortalImage(oldHandle));
	fixture.Render.ForgetWorld(view.World, view.WorldName);
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == 0);
	CHECK(fixture.Render.PortalImageUsage().StagingBytes == 0);
	const auto unloaded = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(unloaded.SurfaceInstances == 0);
	CHECK(unloaded.PortalPasses == 0);
}

TEST_CASE(
	"portal image admission bounds owned pixels and validates the complete binding",
	"[render][gpu][portal-import][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	render::PortalImageBinding binding;
	binding.World = 940;
	binding.WorldName = core::Name("import.admission");
	binding.Portal = core::Name("import.admission.entrance");
	binding.Expected = {1, "import.admission.entrance", 1, 1};
	for (const int malformed : {0, 1, 2, 3, 4, 5}) {
		auto reply = Image(binding.Expected);
		if (malformed == 0) {
			reply.Pixels[1] = std::byte{0x7c};
			reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		}
		if (malformed == 1) {
			reply.Pixels[0] ^= std::byte{1};
		}
		if (malformed == 2) {
			reply.RowStride += 8;
		}
		if (malformed == 3) {
			reply.Width = render::MAX_PORTAL_IMAGE_EXTENT + 1;
		}
		if (malformed == 4) {
			reply.Scope = render::PortalImageScope::OpaqueLighting;
		}
		if (malformed == 5) {
			reply.Pixels.reserve(render::MAX_IMPORTED_PORTAL_STAGING_BYTES + 1);
		}
		CHECK(fixture.Render.QueuePortalImage(binding, std::move(reply)) == 0);
	}
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	for (size_t index = 0; index < render::MAX_IMPORTED_PORTAL_IMAGES; index++) {
		binding.Portal = core::Name("import.admission." + std::to_string(index));
		binding.Expected.PortalKey = binding.Portal.Text();
		auto reply = Image(binding.Expected);
		REQUIRE(fixture.Render.QueuePortalImage(binding, std::move(reply)) != 0);
	}
	const auto full = fixture.Render.PortalImageUsage();
	CHECK(full.Images == render::MAX_IMPORTED_PORTAL_IMAGES);
	CHECK(full.PendingCpuBytes == render::MAX_IMPORTED_PORTAL_IMAGES * IMAGE_SIZE * IMAGE_SIZE * 8);
	CHECK(full.TextureBytes == 0);
	CHECK(full.StagingBytes == 0);
	CHECK(full.Uploads == 0);
	binding.Portal = core::Name("import.admission.over-limit");
	binding.Expected.PortalKey = binding.Portal.Text();
	auto reply = Image(binding.Expected);
	CHECK(fixture.Render.QueuePortalImage(binding, std::move(reply)) == 0);
	fixture.Render.ForgetWorld(binding.World, binding.WorldName);
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
}
