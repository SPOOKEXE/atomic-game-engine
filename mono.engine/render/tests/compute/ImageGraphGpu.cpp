#include "../RenderFixture.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Float16.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/compute/ImageGraphMeshes.hpp>
#include <engine/render/resources/EditableImages.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/editable/EditableImage.hpp>
#include <engine/scene/editable/EditableMesh.hpp>
#include <engine/testing/Suite.hpp>

#include <array>
#include <cstring>
#include <initializer_list>
#include <string>

TEST_SUITE_ID("engine.render.imagegraph_gpu")
TEST_DEPENDS("engine.imagegraph.evaluate")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.editableimages")

namespace {
	using namespace engine;
	using Pixel = std::array<uint8_t, 4>;
	constexpr Pixel RED{255, 0, 0, 255}, GREEN{0, 255, 0, 255}, BLUE{0, 0, 255, 255},
		WHITE{255, 255, 255, 255}, CLEAR{};

	assets::TextureData Pixels(uint32_t width, uint32_t height, std::initializer_list<Pixel> pixels) {
		assets::TextureData image;
		image.Width = width;
		image.Height = height;
		image.Format = assets::TextureFormat::RGBA8_LINEAR;
		for (const auto &pixel : pixels)
			for (const auto channel : pixel)
				image.Pixels.push_back(std::byte{channel});
		REQUIRE(image.IsValid());
		return image;
	}

	assets::TextureData Asymmetric() {
		return Pixels(2, 2, {RED, GREEN, BLUE, WHITE});
	}

	imagegraph::Document Unary(const imagegraph::Operation &operation) {
		return {
			.Nodes =
				{{"source", imagegraph::Source{"source.png", imagegraph::SourceInterpretation::Data}, {}, {}},
				 {"result", operation, {"source"}, {}}},
			.Outputs = {{"main", "result", imagegraph::OutputSpace::Linear}},
			.Parameters = {},
			.Bindings = {}
		};
	}

	// Ordinary texture-table output has no graph resource slot. Download it directly,
	// with padded pitch, after its producer submission has entered the same queue.
	render::test::CapturedImage
	CaptureTexture(render::Renderer &renderer, core::Name name, core::Name owner, size_t bytesPerPixel = 4) {
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.TextureSize(name, width, height, owner));
		REQUIRE(width > 0);
		REQUIRE(height > 0);
		REQUIRE(width <= 2048);
		REQUIRE(height <= 2048);
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.TextureHandle(name, owner));
		REQUIRE(device != nullptr);
		REQUIRE(texture != nullptr);
		render::test::CapturedImage image;
		image.Width = width;
		image.Height = height;
		image.RowStrideBytes = (static_cast<size_t>(width) * bytesPerPixel + 255) / 256 * 256;
		image.Bytes.resize(image.RowStrideBytes * height);
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		transferInfo.size = static_cast<uint32_t>(image.Bytes.size());
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
			render::gpu::ReleaseTransferBuffer(device, transfer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			render::gpu::CreateTransferBuffer(device, &transferInfo), releaseTransfer
		);
		REQUIRE(transfer != nullptr);
		const auto cancelCommand = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
			SDL_AcquireGPUCommandBuffer(device), cancelCommand
		);
		REQUIRE(command != nullptr);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy != nullptr);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = static_cast<uint32_t>(image.RowStrideBytes / bytesPerPixel);
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence != nullptr);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		INFO("texture readback timed out for " << name.Text());
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped != nullptr);
		for (uint32_t row = 0; row < height; row++) {
			const size_t offset = row * image.RowStrideBytes;
			std::memcpy(image.Bytes.data() + offset, mapped + offset, width * bytesPerPixel);
		}
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return image;
	}

	void Same(
		render::Renderer &renderer,
		core::Name name,
		core::Name owner,
		const assets::TextureData &expected,
		double tolerance = 0
	) {
		const auto actual = CaptureTexture(renderer, name, owner);
		render::test::ImageTolerance allowed;
		allowed.Absolute = tolerance;
		render::test::CheckImage(
			renderer,
			"imagegraph",
			name.Text(),
			"literal RGBA8 fixture",
			{expected.Width, expected.Height, render::test::ImageFormat::Rgba8Unorm, expected.Pixels, 0},
			actual.View(),
			allowed
		);
	}

	void Unchanged(const render::ImageGraphStatistics &before, const render::ImageGraphStatistics &after) {
		CHECK(after.CommandBuffers == before.CommandBuffers);
		CHECK(after.ComputeDispatches == before.ComputeDispatches);
		CHECK(after.AllocatedBytes == before.AllocatedBytes);
		CHECK(after.CopiedBytes == before.CopiedBytes);
		CHECK(after.ResidentBytes == before.ResidentBytes);
	}
}

TEST_CASE(
	"GPU image transforms preserve literal rows canvas and transparent boundaries",
	"[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-owner"), graph("gpu-imagegraph"), source("source.png"),
		output("result.png");
	REQUIRE(fixture.Render.AddTexture(source, Asymmetric(), owner));
	const std::array bindings{
		render::ImageGraphSourceBinding{"source.png", source, owner, imagegraph::SourceInterpretation::Data}
	};
	imagegraph::Operation operation;
	assets::TextureData expected;
	SECTION("horizontal flip") {
		operation = imagegraph::Flip{true, false};
		expected = Pixels(2, 2, {GREEN, RED, WHITE, BLUE});
	}
	SECTION("vertical flip") {
		operation = imagegraph::Flip{false, true};
		expected = Pixels(2, 2, {BLUE, WHITE, RED, GREEN});
	}
	SECTION("crop with odd-width readback") {
		operation = imagegraph::Crop{1, 0, 1, 2};
		expected = Pixels(1, 2, {GREEN, WHITE});
	}
	SECTION("crop outside canvas") {
		operation = imagegraph::Crop{-1, -1, 2, 2};
		expected = Pixels(2, 2, {CLEAR, CLEAR, CLEAR, RED});
	}
	SECTION("nearest resize") {
		operation = imagegraph::Resize{4, 2};
		expected = Pixels(4, 2, {RED, RED, GREEN, GREEN, BLUE, BLUE, WHITE, WHITE});
	}
	SECTION("translation") {
		operation = imagegraph::Transform{2, 2, 1, 0};
		expected = Pixels(2, 2, {CLEAR, RED, CLEAR, BLUE});
	}
	SECTION("clockwise rotation about pivot") {
		operation = imagegraph::Transform{2, 2, 0, 0, 1, 1, 90, 1, 1};
		expected = Pixels(2, 2, {BLUE, RED, WHITE, GREEN});
	}
	SECTION("negative scale about pivot") {
		operation = imagegraph::Transform{2, 2, 0, 0, -1, 1, 0, 1, 1};
		expected = Pixels(2, 2, {GREEN, RED, WHITE, BLUE});
	}
	imagegraph::Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, Unary(operation), bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, output, owner, expected);
	CHECK(fixture.Render.TextureHandle(output, {}) == nullptr);
}

TEST_CASE(
	"GPU image composition removes transparent hidden colour without fringe", "[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-alpha"), graph("alpha-graph"), source("source.png"),
		front("front.png"), output("alpha.png");
	imagegraph::Document document;
	assets::TextureData expected;
	std::vector<render::ImageGraphSourceBinding> bindings{
		{"source.png", source, owner, imagegraph::SourceInterpretation::Data}
	};
	SECTION("premultiplied bilinear") {
		REQUIRE(fixture.Render.AddTexture(source, Pixels(2, 1, {{255, 0, 0, 0}, BLUE}), owner));
		document = Unary(imagegraph::Resize{1, 1, imagegraph::Sampling::Bilinear});
		expected = Pixels(1, 1, {{0, 0, 255, 128}});
	}
	SECTION("straight-alpha source over") {
		REQUIRE(fixture.Render.AddTexture(source, Pixels(2, 1, {{255, 0, 0, 128}, {255, 0, 0, 0}}), owner));
		REQUIRE(fixture.Render.AddTexture(front, Pixels(2, 1, {{0, 0, 255, 128}, {0, 255, 0, 0}}), owner));
		bindings.push_back({"front.png", front, owner, imagegraph::SourceInterpretation::Data});
		document = {
			.Nodes =
				{{"back", imagegraph::Source{"source.png", imagegraph::SourceInterpretation::Data}, {}, {}},
				 {"front", imagegraph::Source{"front.png", imagegraph::SourceInterpretation::Data}, {}, {}},
				 {"blend", imagegraph::Blend{}, {"back", "front"}, {}}},
			.Outputs = {{"main", "blend", imagegraph::OutputSpace::Linear}},
			.Parameters = {},
			.Bindings = {}
		};
		expected = Pixels(2, 1, {{85, 0, 170, 192}, CLEAR});
	}
	imagegraph::Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, output, owner, expected);
}

TEST_CASE("GPU colour output publishes ordinary sRGB texture bytes", "[render][gpu][imagegraph][.]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-colour"), graph("colour-graph"), source("colour.png"),
		output("colour-output.png");
	fixture.Render.SetProfiling(render::ProfilingTier::Full);
	auto pixels = Pixels(3, 1, {{17, 43, 91, 255}, {128, 191, 230, 128}, {255, 0, 0, 0}});
	pixels.Format = assets::TextureFormat::RGBA8;
	REQUIRE(fixture.Render.AddTexture(source, pixels, owner));
	imagegraph::Document document{
		.Nodes = {{"source", imagegraph::Source{"colour.png"}, {}, {}}},
		.Outputs = {{"main", "source", imagegraph::OutputSpace::SRGB}},
		.Parameters = {},
		.Bindings = {}
	};
	const std::array bindings{render::ImageGraphSourceBinding{"colour.png", source, owner}};
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, output, owner, pixels, 1.0 / 255);
	const auto profile = fixture.Render.ImageGraphProfile();
	CHECK(profile.HasGpuTimings);
	CHECK(profile.GpuTimingSequence > 0);
	CHECK(profile.GpuMicroseconds > 0);
}

TEST_CASE(
	"GPU typed sources distinguish colour and data from the same resident path",
	"[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-interpretation"), graph("typed-graph"), source("grey.png"),
		colourOutput("colour.png"), dataOutput("data.png");
	auto pixels = Pixels(1, 1, {{128, 128, 128, 73}});
	auto expectedData = pixels;
	uint8_t colourByte = 188;
	uint8_t colourAlpha = 73;
	std::string path = "grey.png";
	SECTION("linear storage imports colour through sRGB encoding") {
		pixels.Format = assets::TextureFormat::RGBA8_LINEAR;
	}
	SECTION("sRGB storage imports authored encoded colour and raw data bytes") {
		pixels.Format = assets::TextureFormat::RGBA8;
		colourByte = 128;
	}
	SECTION("R8 data preserves missing channels despite grayscale device upload") {
		pixels.Format = assets::TextureFormat::R8;
		pixels.Pixels = {std::byte{128}};
		expectedData = Pixels(1, 1, {{128, 0, 0, 255}});
		colourAlpha = 255;
	}
	SECTION("R16 float data preserves missing channels") {
		pixels.Format = assets::TextureFormat::R16_FLOAT;
		pixels.Pixels = {std::byte{0}, std::byte{0x38}}; // IEEE half 0.5, little endian.
		expectedData = Pixels(1, 1, {{128, 0, 0, 255}});
		colourAlpha = 255;
	}
	SECTION("R32 float data preserves missing channels") {
		pixels.Format = assets::TextureFormat::R32_FLOAT;
		pixels.Pixels = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x3f}}; // IEEE float 0.5.
		expectedData = Pixels(1, 1, {{128, 0, 0, 255}});
		colourAlpha = 255;
	}
	SECTION("source bindings admit document paths beyond 1024 bytes") {
		path = std::string(1400, 'a') + ".atex";
	}
	REQUIRE(fixture.Render.AddTexture(source, pixels, owner));
	const std::array bindings{
		render::ImageGraphSourceBinding{path, source, owner, imagegraph::SourceInterpretation::Colour},
		render::ImageGraphSourceBinding{path, source, owner, imagegraph::SourceInterpretation::Data}
	};
	imagegraph::Document document{
		.Nodes =
			{{"colour", imagegraph::Source{path, imagegraph::SourceInterpretation::Colour}, {}, {}},
			 {"data", imagegraph::Source{path, imagegraph::SourceInterpretation::Data}, {}, {}}},
		.Outputs =
			{{"colour", "colour", imagegraph::OutputSpace::SRGB},
			 {"data", "data", imagegraph::OutputSpace::Linear}},
		.Parameters = {},
		.Bindings = {}
	};
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "colour", colourOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "data", dataOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(
		fixture.Render,
		colourOutput,
		owner,
		Pixels(1, 1, {{colourByte, colourByte, colourByte, colourAlpha}}),
		1.0 / 255
	);
	Same(fixture.Render, dataOutput, owner, expectedData);
}

TEST_CASE(
	"GPU graph sources follow editable image ownership revisions and storage intent",
	"[render][gpu][imagegraph][editable-source][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	scene::RegisterSceneClasses();
	ecs::Store store("gpu-imagegraph-editable-source");
	const auto entity = store.CreateInstance(scene::EditableImageClass(), "source");
	const core::Name owner("gpu-imagegraph-editable-source"), graph("editable-graph"),
		colourOutput("imagegraph-instance://editable#colour"),
		dataOutput("imagegraph-instance://editable#data");
	const auto source = scene::EditableImageContentName(store, entity);
	const auto grey = Pixels(1, 1, {{128, 128, 128, 73}});
	REQUIRE(
		scene::SetEditableImagePixels(store, entity, 1, 1, grey.Pixels, scene::EditableImageSpace::Linear)
	);
	render::EditableImageUploader uploader;
	REQUIRE(uploader.Refresh(store, fixture.Render, owner) == 1);
	REQUIRE(fixture.Render.AddTexture(source, Pixels(1, 1, {GREEN})));
	REQUIRE(fixture.Render.TextureHandle(source, owner) != fixture.Render.TextureHandle(source));
	CHECK_FALSE(fixture.Render.TextureSamplesSRGB(source, owner));
	const std::string path(source.Text());
	const std::array bindings{
		render::ImageGraphSourceBinding{path, source, owner, imagegraph::SourceInterpretation::Colour},
		render::ImageGraphSourceBinding{path, source, owner, imagegraph::SourceInterpretation::Data}
	};
	imagegraph::Document document{
		.Nodes =
			{{"colour", imagegraph::Source{path, imagegraph::SourceInterpretation::Colour}, {}, {}},
			 {"data", imagegraph::Source{path, imagegraph::SourceInterpretation::Data}, {}, {}}},
		.Outputs =
			{{"colour", "colour", imagegraph::OutputSpace::SRGB},
			 {"data", "data", imagegraph::OutputSpace::Linear}},
		.Parameters = {},
		.Bindings = {}
	};
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "colour", colourOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "data", dataOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, colourOutput, owner, Pixels(1, 1, {{188, 188, 188, 73}}), 1.0 / 255);
	Same(fixture.Render, dataOutput, owner, grey);
	const auto colourHandle = fixture.Render.TextureHandle(colourOutput, owner);
	const auto dataHandle = fixture.Render.TextureHandle(dataOutput, owner);
	const auto cached = fixture.Render.ImageGraphProfile();
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "data", dataOutput, diagnostic) ==
		render::ImageGraphEvaluation::Reused
	);
	Unchanged(cached, fixture.Render.ImageGraphProfile());
	// Identical bytes imported from a codec carry encoded intent, which changes only colour interpretation.
	REQUIRE(scene::SetEditableImagePixels(store, entity, 1, 1, grey.Pixels, scene::EditableImageSpace::SRGB));
	REQUIRE(uploader.Refresh(store, fixture.Render, owner) == 1);
	CHECK(fixture.Render.TextureSamplesSRGB(source, owner));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "colour", colourOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "data", dataOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, colourOutput, owner, grey, 1.0 / 255);
	Same(fixture.Render, dataOutput, owner, grey, 1.0 / 255);
	CHECK(fixture.Render.TextureHandle(colourOutput, owner) == colourHandle);
	CHECK(fixture.Render.TextureHandle(dataOutput, owner) == dataHandle);
	const auto red = Pixels(1, 1, {RED});
	REQUIRE(
		scene::SetEditableImagePixels(store, entity, 1, 1, red.Pixels, scene::EditableImageSpace::Linear)
	);
	REQUIRE(uploader.Refresh(store, fixture.Render, owner) == 1);
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "data", dataOutput, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, dataOutput, owner, red);
	CHECK(fixture.Render.TextureHandle(dataOutput, owner) == dataHandle);
	REQUIRE(fixture.Render.DropTexture(source, owner));
	REQUIRE(fixture.Render.TextureHandle(source) != nullptr);
	CHECK(
		fixture.Render.EvaluateImageGraph(owner, graph, "data", dataOutput, diagnostic) ==
		render::ImageGraphEvaluation::Refused
	);
	CHECK_FALSE(diagnostic.Message.empty());
	Same(fixture.Render, dataOutput, owner, red);
}

TEST_CASE(
	"GPU affine admission refuses finite doubles outside shader precision without changing accepted output",
	"[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-affine-admission"), graph("affine-graph"), source("source.png"),
		output("affine.png");
	REQUIRE(fixture.Render.AddTexture(source, Asymmetric(), owner));
	const std::array bindings{
		render::ImageGraphSourceBinding{"source.png", source, owner, imagegraph::SourceInterpretation::Data}
	};
	const auto accepted = Unary(imagegraph::Transform{2, 2});
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, accepted, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, output, owner, Asymmetric());
	const auto *handle = fixture.Render.TextureHandle(output, owner);
	auto refused = accepted;
	auto &transform = std::get<imagegraph::Transform>(refused.Nodes[1].Value);
	SECTION("large finite translation") {
		transform.TranslateX = 1e300;
	}
	SECTION("tiny finite scale has an unrepresentable inverse") {
		transform.ScaleX = 1e-300;
	}
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, refused, bindings, diagnostic));
	const auto before = fixture.Render.ImageGraphProfile();
	const auto memory = fixture.Render.MemoryStatistics();
	CHECK(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Refused
	);
	CHECK(diagnostic.Node == "result");
	CHECK_FALSE(diagnostic.Message.empty());
	Unchanged(before, fixture.Render.ImageGraphProfile());
	CHECK(fixture.Render.MemoryStatistics().AllocatedBytes == memory.AllocatedBytes);
	CHECK(fixture.Render.TextureHandle(output, owner) == handle);
	Same(fixture.Render, output, owner, Asymmetric());
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, accepted, bindings, diagnostic));
	const auto restored = fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic);
	REQUIRE((
		restored == render::ImageGraphEvaluation::Reused || restored == render::ImageGraphEvaluation::Updated
	));
	CHECK(fixture.Render.TextureHandle(output, owner) == handle);
	Same(fixture.Render, output, owner, Asymmetric());
}

TEST_CASE(
	"GPU solid canvas resizes and owner retirement releases derived images", "[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-solid"), graph("solid-graph"), output("solid.png");
	imagegraph::Document document{
		.Nodes = {{"solid", imagegraph::Solid{3, 1, {17, 91, 43, 128}}, {}, {}}},
		.Outputs = {{"main", "solid", imagegraph::OutputSpace::Linear}},
		.Parameters = {},
		.Bindings = {}
	};
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, {}, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(
		fixture.Render, output, owner, Pixels(3, 1, {{17, 91, 43, 128}, {17, 91, 43, 128}, {17, 91, 43, 128}})
	);
	std::get<imagegraph::Solid>(document.Nodes[0].Value).Width = 1;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, {}, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, output, owner, Pixels(1, 1, {{17, 91, 43, 128}}));
	const auto resident = fixture.Render.MemoryStatistics().LiveBytes;
	fixture.Render.DropContentOwner(owner);
	CHECK(fixture.Render.TextureHandle(output, owner) == nullptr);
	CHECK(fixture.Render.MemoryStatistics().LiveBytes < resident);
	CHECK(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Refused
	);
}

TEST_CASE(
	"GPU image graph dirty cones reuse resident shared nodes and allocate nothing on hits",
	"[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-cache"), graph("cache-graph"), source("source.png"),
		left("left.png"), right("right.png");
	REQUIRE(fixture.Render.AddTexture(source, Asymmetric(), owner));
	const std::array bindings{
		render::ImageGraphSourceBinding{"source.png", source, owner, imagegraph::SourceInterpretation::Data}
	};
	imagegraph::Document document{
		.Nodes =
			{{"source", imagegraph::Source{"source.png", imagegraph::SourceInterpretation::Data}, {}, {}},
			 {"left", imagegraph::Flip{true, false}, {"source"}, {}},
			 {"right", imagegraph::Flip{false, true}, {"source"}, {}}},
		.Outputs =
			{{"left", "left", imagegraph::OutputSpace::Linear},
			 {"right", "right", imagegraph::OutputSpace::Linear}},
		.Parameters = {},
		.Bindings = {}
	};
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "left", left, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	const auto first = fixture.Render.ImageGraphProfile();
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "right", right, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	const auto second = fixture.Render.ImageGraphProfile();
	CHECK(second.ComputeDispatches - first.ComputeDispatches == 1);
	Same(fixture.Render, left, owner, Pixels(2, 2, {GREEN, RED, WHITE, BLUE}));
	Same(fixture.Render, right, owner, Pixels(2, 2, {BLUE, WHITE, RED, GREEN}));
	const auto before = fixture.Render.ImageGraphProfile();
	const auto memory = fixture.Render.MemoryStatistics();
	for (size_t iteration = 0; iteration < 32; iteration++)
		REQUIRE(
			fixture.Render.EvaluateImageGraph(owner, graph, "left", left, diagnostic) ==
			render::ImageGraphEvaluation::Reused
		);
	const auto after = fixture.Render.ImageGraphProfile();
	Unchanged(before, after);
	CHECK(after.CacheHits - before.CacheHits == 32);
	CHECK(fixture.Render.MemoryStatistics().AllocatedBytes == memory.AllocatedBytes);
	CHECK(fixture.Render.MemoryStatistics().LiveBytes == memory.LiveBytes);
	const auto *leftHandle = fixture.Render.TextureHandle(left, owner);
	document.Nodes[1].Value = imagegraph::Flip{true, true};
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	const auto edit = fixture.Render.ImageGraphProfile();
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "right", right, diagnostic) ==
		render::ImageGraphEvaluation::Reused
	);
	Unchanged(edit, fixture.Render.ImageGraphProfile());
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "left", left, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	CHECK(fixture.Render.ImageGraphProfile().ComputeDispatches - edit.ComputeDispatches == 1);
	CHECK(fixture.Render.TextureHandle(left, owner) == leftHandle);
	Same(fixture.Render, left, owner, Pixels(2, 2, {WHITE, BLUE, GREEN, RED}));
	REQUIRE(fixture.Render.DropImageGraph(owner, graph));
	CHECK(fixture.Render.TextureHandle(left, owner) == nullptr);
	CHECK(fixture.Render.TextureHandle(right, owner) == nullptr);
	CHECK(fixture.Render.TextureHandle(source, owner) != nullptr);
}

TEST_CASE(
	"GPU image sources are owner exact and refusals preserve accepted outputs", "[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-owner-a"), other("gpu-imagegraph-owner-b"), graph("owner-graph"),
		source("source.png"), output("published.png");
	REQUIRE(fixture.Render.AddTexture(source, Asymmetric(), owner));
	REQUIRE(fixture.Render.AddTexture(source, Pixels(2, 2, {WHITE, WHITE, WHITE, WHITE}), other));
	REQUIRE(fixture.Render.AddTexture(source, Pixels(2, 2, {BLUE, BLUE, BLUE, BLUE})));
	const std::array bindings{
		render::ImageGraphSourceBinding{"source.png", source, owner, imagegraph::SourceInterpretation::Data}
	};
	const auto document = Unary(imagegraph::Flip{true, false});
	imagegraph::Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, bindings, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	Same(fixture.Render, output, owner, Pixels(2, 2, {GREEN, RED, WHITE, BLUE}));
	const auto *handle = fixture.Render.TextureHandle(output, owner);
	REQUIRE(fixture.Render.AddTexture(source, Pixels(2, 2, {GREEN, BLUE, WHITE, RED}), owner));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	CHECK(fixture.Render.TextureHandle(output, owner) == handle);
	const auto replaced = Pixels(2, 2, {BLUE, GREEN, RED, WHITE});
	Same(fixture.Render, output, owner, replaced);
	auto invalid = document;
	invalid.Nodes[1].Inputs = {"result"};
	CHECK_FALSE(fixture.Render.SetImageGraph(owner, graph, invalid, bindings, diagnostic));
	CHECK_FALSE(diagnostic.Message.empty());
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Reused
	);
	REQUIRE(fixture.Render.DropTexture(source, owner));
	CHECK(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Refused
	);
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(fixture.Render.TextureHandle(output, owner) == handle);
	Same(fixture.Render, output, owner, replaced);
}

namespace {
	// CPU reference for a document with no sources, published as linear RGBA8.
	assets::TextureData Reference(const imagegraph::Document &document) {
		imagegraph::Plan plan;
		imagegraph::Diagnostic diagnostic;
		REQUIRE(imagegraph::Compile(document, plan, diagnostic));
		imagegraph::Image image;
		INFO(diagnostic.Message);
		REQUIRE(imagegraph::EvaluateWith(document, plan, {}, {}, image, diagnostic));
		assets::TextureData texture;
		texture.Width = image.Width;
		texture.Height = image.Height;
		texture.Format = assets::TextureFormat::RGBA8_LINEAR;
		texture.Pixels = image.Pixels;
		return texture;
	}
	imagegraph::Document Procedural(std::vector<imagegraph::Node> nodes, std::string target) {
		return {
			.Nodes = std::move(nodes),
			.Outputs = {{"main", std::move(target), imagegraph::OutputSpace::Linear}},
			.Parameters = {},
			.Bindings = {}
		};
	}
}

TEST_CASE("GPU procedural colour and filter nodes match the CPU evaluator", "[render][gpu][imagegraph][.]") {
	using namespace engine;
	using namespace engine::imagegraph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-nodes"), graph("nodes-graph"), output("nodes.png");
	const Node gradient{"gradient", Gradient{32, 32, GradientType::Linear, 0, 0, 1, 1}, {}, {}};
	const Node solid{"solid", Solid{32, 32, {200, 90, 40, 255}}, {}, {}};
	Document document;
	// Float lattice and distance maths can round a few boundary pixels differently.
	size_t mismatched = 0;
	SECTION("value noise") {
		document = Procedural({{"noise", Noise{32, 32, NoiseType::Value, 4, 3, 2, 0.5, 9}, {}, {}}}, "noise");
		mismatched = 16;
	}
	SECTION("gradient noise") {
		document =
			Procedural({{"noise", Noise{32, 32, NoiseType::Gradient, 4, 2, 2, 0.5, 9}, {}, {}}}, "noise");
		mismatched = 16;
	}
	SECTION("voronoi driven by a vector input") {
		document = Procedural(
			{gradient, {"cells", Voronoi{32, 32, 4, 1, VoronoiFeature::F1, 3}, {"gradient"}, {}}}, "cells"
		);
		mismatched = 16;
	}
	SECTION("voronoi edges") {
		document = Procedural({{"cells", Voronoi{32, 32, 4, 1, VoronoiFeature::Edge, 3}, {}, {}}}, "cells");
		mismatched = 16;
	}
	SECTION("distorted rings") {
		document = Procedural(
			{{"wave", Wave{32, 32, WaveType::Rings, WaveProfile::Triangle, 3, 0, 0, 0.3, 2, 2, 5}, {}, {}}},
			"wave"
		);
		mismatched = 16;
	}
	SECTION("ramp") {
		document = Procedural(
			{gradient,
			 {"ramp",
			  Ramp{
				  Channel::Red,
				  RampInterpolation::Smooth,
				  {{0, {0, 0, 255, 255}}, {0.5, {255, 0, 0, 255}}, {1, {0, 255, 0, 128}}}
			  },
			  {"gradient"},
			  {}}},
			"ramp"
		);
	}
	SECTION("levels and math") {
		document = Procedural(
			{gradient,
			 solid,
			 {"levels", Levels{0.2, 0.8, 0.1, 0.9, 2.2, true, 7}, {"gradient"}, {}},
			 {"math", Math{MathOperation::Multiply, {1, 1, 1, 1}, 15}, {"levels", "solid"}, {}}},
			"math"
		);
	}
	SECTION("channels and invert") {
		document = Procedural(
			{gradient,
			 solid,
			 {"merge", Channels{{{{1, 0}, {0, 0}, {-1, 0}, {-2, 0}}}}, {"gradient", "solid"}, {}},
			 {"invert", Invert{3}, {"merge"}, {}}},
			"invert"
		);
	}
	SECTION("masked blend modes") {
		document = Procedural(
			{gradient,
			 solid,
			 {"blend", Blend{0.8, BlendMode::Overlay}, {"solid", "gradient", "gradient"}, {}}},
			"blend"
		);
	}
	SECTION("blur") {
		document = Procedural({gradient, {"blur", Blur{4}, {"gradient"}, {}}}, "blur");
	}
	SECTION("normal from height") {
		document = Procedural(
			{{"noise", Noise{32, 32, NoiseType::Value, 3, 1, 2, 0.5, 1}, {}, {}},
			 {"normal", NormalMap{4, Channel::Red, false}, {"noise"}, {}}},
			"normal"
		);
		mismatched = 16;
	}
	SECTION("warp") {
		document = Procedural(
			{gradient, solid, {"warp", Warp{3, Sampling::Bilinear}, {"gradient", "solid"}, {}}}, "warp"
		);
	}
	SECTION("lookup") {
		document = Procedural(
			{{"noise", Noise{16, 16, NoiseType::Value, 3, 1, 2, 0.5, 1}, {}, {}},
			 gradient,
			 {"lookup",
			  Lookup{Sampling::Bilinear, LookupWrap::Repeat, 3, 2, 0.1, 0},
			  {"noise", "gradient"},
			  {}}},
			"lookup"
		);
		mismatched = 16;
	}
	SECTION("wide intermediates") {
		document = Procedural(
			{gradient,
			 {"squeeze", Levels{0, 1, 0, 0.0625, 1, true, 7}, {"gradient"}, {}},
			 {"stretch", Levels{0, 0.0625, 0, 1, 1, true, 7}, {"squeeze"}, {}}},
			"stretch"
		);
		document.Storage = Precision::Wide;
	}
	const auto expected = Reference(document);
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, {}, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	const auto actual = CaptureTexture(fixture.Render, output, owner);
	render::test::ImageTolerance allowed;
	allowed.Absolute = 2.0 / 255;
	allowed.AllowedMismatchedPixels = mismatched;
	render::test::CheckImage(
		fixture.Render,
		"imagegraph",
		output.Text(),
		"CPU imagegraph reference",
		{expected.Width, expected.Height, render::test::ImageFormat::Rgba8Unorm, expected.Pixels, 0},
		actual.View(),
		allowed
	);
}

TEST_CASE(
	"GPU float outputs publish R16F and refuse unlowered mesh attributes", "[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	using namespace engine::imagegraph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-float"), graph("float-graph"), output("float.png");
	Document document{
		.Nodes = {{"gradient", Gradient{64, 2, GradientType::Linear, 0, 0, 1, 0}, {}, {}}},
		.Outputs = {{"main", "gradient", OutputSpace::Linear, OutputFormat::R16F}},
		.Parameters = {},
		.Bindings = {},
		.Storage = Precision::Wide
	};
	Diagnostic diagnostic;
	REQUIRE(fixture.Render.SetImageGraph(owner, graph, document, {}, diagnostic));
	REQUIRE(
		fixture.Render.EvaluateImageGraph(owner, graph, "main", output, diagnostic) ==
		render::ImageGraphEvaluation::Updated
	);
	const auto actual = CaptureTexture(fixture.Render, output, owner, 2);
	const auto half = [&](uint32_t x) {
		return core::DecodeFloat16(
			uint16_t(std::to_integer<uint8_t>(actual.Bytes[x * 2])) |
			uint16_t(std::to_integer<uint8_t>(actual.Bytes[x * 2 + 1])) << 8
		);
	};
	for (uint32_t x = 0; x < 64; ++x)
		CHECK(std::abs(half(x) - (x + 0.5f) / 64) < 1e-3f);

	document.Nodes = {{"mesh", MeshAttribute{8, 8, "editable-mesh://3"}, {}, {}}};
	document.Outputs[0].Node = "mesh";
	CHECK_FALSE(fixture.Render.SetImageGraph(owner, graph, document, {}, diagnostic));
	CHECK(diagnostic.Message.find("lowered") != std::string::npos);
}

TEST_CASE(
	"regionUV voronoi ramp multiply cooks from a mesh deterministically on CPU and GPU",
	"[render][gpu][imagegraph][.]"
) {
	using namespace engine;
	using namespace engine::imagegraph;
	scene::RegisterSceneClasses();
	ecs::Store store("imagegraph.anatomy");
	const auto entity = store.CreateInstance(scene::EditableMeshClass(), "Body");
	// One quad over UDIM tile 1001 whose attribute channel 1 carries region UV.
	scene::EditableMeshGeometry quad;
	quad.Positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
	quad.Normals.assign(4, {0, 0, 1});
	quad.UVs = {{0.05f, 0.05f}, {0.95f, 0.05f}, {0.95f, 0.95f}, {0.05f, 0.95f}};
	quad.Colours.assign(4, {1, 1, 1});
	quad.Alphas.assign(4, 1);
	quad.Indices = {0, 1, 2, 0, 2, 3};
	quad.Attributes[1] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}};
	REQUIRE(scene::ReplaceEditableMesh(store, entity, quad) == scene::EditableMeshCommit::Applied);
	const std::string mesh = std::string(scene::EditableMeshContentName(store, entity).Text());
	Document document{
		.Nodes =
			{{"uv", MeshAttribute{64, 64, mesh, 1, "xy", 0, 0, 0, 1, 2}, {}, {}},
			 {"pores", Voronoi{64, 64, 10, 1, VoronoiFeature::F1, 4}, {"uv"}, {}},
			 {"tint",
			  Ramp{
				  Channel::Red, RampInterpolation::Linear, {{0, {90, 40, 40, 255}}, {1, {255, 255, 255, 255}}}
			  },
			  {"pores"},
			  {}},
			 {"base", Solid{64, 64, {220, 170, 150, 255}}, {}, {}},
			 {"skin", Blend{1, BlendMode::Multiply}, {"base", "tint"}, {}}},
		.Outputs = {{"albedo", "skin", OutputSpace::Linear}},
		.Parameters = {},
		.Bindings = {}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic));
	Image first, second;
	INFO(diagnostic.Message);
	REQUIRE(EvaluateWith(document, plan, {}, {{}, render::EditableMeshResolver(store)}, first, diagnostic));
	REQUIRE(EvaluateWith(document, plan, {}, {{}, render::EditableMeshResolver(store)}, second, diagnostic));
	CHECK(first.Pixels == second.Pixels);

	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("gpu-imagegraph-anatomy"), graph("anatomy"), output("albedo.png");
	render::ImageGraphMeshUploader meshes;
	const auto cook = [&] {
		Document lowered = document;
		INFO(diagnostic.Message);
		REQUIRE(meshes.Lower(store, fixture.Render, owner, 1, lowered, diagnostic));
		REQUIRE(std::holds_alternative<Source>(lowered.Nodes[0].Value));
		const auto &path = std::get<Source>(lowered.Nodes[0].Value).Path;
		const std::array bindings{
			render::ImageGraphSourceBinding{path, core::Name(path), owner, SourceInterpretation::Data}
		};
		REQUIRE(fixture.Render.SetImageGraph(owner, graph, lowered, bindings, diagnostic));
		REQUIRE(
			fixture.Render.EvaluateImageGraph(owner, graph, "albedo", output, diagnostic) ==
			render::ImageGraphEvaluation::Updated
		);
		return CaptureTexture(fixture.Render, output, owner);
	};
	const auto gpu = cook();
	assets::TextureData expected;
	expected.Width = first.Width;
	expected.Height = first.Height;
	expected.Format = assets::TextureFormat::RGBA8_LINEAR;
	expected.Pixels = first.Pixels;
	render::test::ImageTolerance allowed;
	allowed.Absolute = 2.0 / 255;
	allowed.AllowedMismatchedPixels = 24;
	render::test::CheckImage(
		fixture.Render,
		"imagegraph",
		output.Text(),
		"CPU anatomy reference",
		{expected.Width, expected.Height, render::test::ImageFormat::Rgba8Unorm, expected.Pixels, 0},
		gpu.View(),
		allowed
	);
	// A fresh cook of the same seed reproduces the GPU bytes exactly.
	REQUIRE(fixture.Render.DropImageGraph(owner, graph));
	CHECK(cook().Bytes == gpu.Bytes);
	meshes.Forget(fixture.Render, 1);
}
