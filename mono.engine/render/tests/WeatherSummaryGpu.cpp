#include "RenderFixture.hpp"
#include "ShaderBinary.hpp"
#include "WeatherAssembly.hpp"
#include "WeatherGpu.hpp"

#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.render.weathersummarygpu")
TEST_DEPENDS("engine.weather.weather")
TEST_DEPENDS("engine.render.weatherassembly")

namespace {
	using namespace engine;
	using namespace engine::render;
	using Summary = std::array<uint32_t, 4>;
	constexpr Summary EMPTY_REDUCTION{0, 255, 0, 0};

	void Accumulate(Summary &summary, uint32_t density) {
		summary[0] = std::max(summary[0], density);
		summary[1] = std::min(summary[1], density);
		summary[2] += density;
		++summary[3];
	}

	SDL_GPUComputePipeline *SummaryPipeline(SDL_GPUDevice *device) {
		const auto binary = ShaderBinaryFor(device);
		std::ifstream file(resources::Shader("weather-summary.comp", binary.Form), std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		REQUIRE_FALSE(bytes.empty());
		SDL_GPUComputePipelineCreateInfo info{};
		info.code = reinterpret_cast<const Uint8 *>(bytes.data());
		info.code_size = bytes.size();
		info.entrypoint = binary.EntryPoint;
		info.format = binary.Format;
		info.num_readonly_storage_buffers = 3;
		info.num_readwrite_storage_buffers = 1;
		info.num_uniform_buffers = 1;
		info.threadcount_x = 64;
		info.threadcount_y = 1;
		info.threadcount_z = 1;
		return SDL_CreateGPUComputePipeline(device, &info);
	}

	std::vector<Summary> ReadSummaries(
		SDL_GPUDevice *device,
		SDL_GPUComputePipeline *pipeline,
		const WeatherGpuUniforms &uniforms,
		std::span<const uint32_t> words,
		std::span<const glm::ivec4> descriptors
	) {
		REQUIRE_FALSE(words.empty());
		REQUIRE_FALSE(descriptors.empty());
		REQUIRE(uniforms.Bricks.w > 0);
		const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
		SDL_GPUBufferCreateInfo info{};
		info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
		info.size = static_cast<uint32_t>(words.size_bytes());
		std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> cells(
			gpu::CreateBuffer(device, &info), releaseBuffer
		);
		info.size = static_cast<uint32_t>(descriptors.size_bytes());
		std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> pages(
			gpu::CreateBuffer(device, &info), releaseBuffer
		);
		info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
		info.size = uniforms.Bricks.w * sizeof(Summary);
		std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> summaries(
			gpu::CreateBuffer(device, &info), releaseBuffer
		);
		REQUIRE(cells);
		REQUIRE(pages);
		REQUIRE(summaries);

		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *buffer) {
			gpu::ReleaseTransferBuffer(device, buffer);
		};
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(words.size_bytes() + descriptors.size_bytes());
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> upload(
			gpu::CreateTransferBuffer(device, &transferInfo), releaseTransfer
		);
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		transferInfo.size = info.size;
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> download(
			gpu::CreateTransferBuffer(device, &transferInfo), releaseTransfer
		);
		REQUIRE(upload);
		REQUIRE(download);
		auto *mapped = static_cast<std::byte *>(SDL_MapGPUTransferBuffer(device, upload.get(), false));
		REQUIRE(mapped);
		std::memcpy(mapped, words.data(), words.size_bytes());
		std::memcpy(mapped + words.size_bytes(), descriptors.data(), descriptors.size_bytes());
		SDL_UnmapGPUTransferBuffer(device, upload.get());
		const auto cancel = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> command(
			SDL_AcquireGPUCommandBuffer(device), cancel
		);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy);
		const SDL_GPUTransferBufferLocation cellSource{upload.get(), 0};
		const SDL_GPUBufferRegion cellTarget{cells.get(), 0, static_cast<uint32_t>(words.size_bytes())};
		SDL_UploadToGPUBuffer(copy, &cellSource, &cellTarget, false);
		const SDL_GPUTransferBufferLocation pageSource{
			upload.get(), static_cast<uint32_t>(words.size_bytes())
		};
		const SDL_GPUBufferRegion pageTarget{pages.get(), 0, static_cast<uint32_t>(descriptors.size_bytes())};
		SDL_UploadToGPUBuffer(copy, &pageSource, &pageTarget, false);
		SDL_EndGPUCopyPass(copy);
		REQUIRE(
			RecordWeatherSummary(command.get(), pipeline, cells.get(), pages.get(), summaries.get(), uniforms)
		);
		copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy);
		const SDL_GPUBufferRegion source{summaries.get(), 0, transferInfo.size};
		const SDL_GPUTransferBufferLocation destination{download.get(), 0};
		SDL_DownloadFromGPUBuffer(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const void *result = SDL_MapGPUTransferBuffer(device, download.get(), false);
		REQUIRE(result);
		std::vector<Summary> values(uniforms.Bricks.w);
		std::memcpy(values.data(), result, transferInfo.size);
		SDL_UnmapGPUTransferBuffer(device, download.get());
		return values;
	}

	void AppendDensity(std::vector<uint32_t> &words, uint32_t density) {
		weather::CellValues values;
		values.Condensate = static_cast<int16_t>(density);
		const auto encoded = weather::Encode(values);
		REQUIRE(encoded);
		for (size_t word = 0; word < 2; ++word) {
			uint32_t packed = 0;
			for (size_t byte = 0; byte < 4; ++byte)
				packed |= uint32_t{encoded->Codes[word * 4 + byte]} << (byte * 8);
			words.push_back(packed);
		}
	}

	Summary AppendCompactPage(
		std::vector<uint32_t> &words,
		const glm::ivec4 &page,
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		bool unsafe,
		bool alternating
	) {
		Summary expected = EMPTY_REDUCTION;
		const int32_t stride = int32_t{1} << (page.w & 31);
		for (uint32_t group = 0; group < WEATHER_ASSEMBLIES_PER_PAGE; ++group) {
			const bool fallback = unsafe && (!alternating || (group & 1u));
			std::array<weather::EncodedCell, 8> children{};
			for (uint32_t child = 0; child < children.size(); ++child) {
				weather::CellValues values;
				values.Condensate = static_cast<int16_t>(fallback ? 0 : (group * 17) % 253 + child % 3);
				const auto encoded = weather::Encode(values);
				REQUIRE(encoded);
				children[child] = *encoded;
			}
			auto packed = PackWeatherAssembly(children);
			REQUIRE(packed);
			if (fallback) {
				packed->Words[1] |= uint32_t{1} << 23;
				for (uint32_t child = 0; child < children.size(); ++child) {
					const weather::CellCoordinate coordinate{
						page.x + static_cast<int32_t>((group % 4) * 2 + (child & 1u)) * stride,
						page.y + static_cast<int32_t>(((group / 4) % 4) * 2 + ((child >> 1) & 1u)) * stride,
						page.z + static_cast<int32_t>((group / 16) * 2 + ((child >> 2) & 1u)) * stride
					};
					Accumulate(expected, weather::Evaluate(compiled.Definition, time, coordinate).Codes[0]);
				}
			} else {
				const auto reconstructed = UnpackWeatherAssembly(*packed);
				REQUIRE(reconstructed);
				for (const auto &cell : *reconstructed)
					Accumulate(expected, cell.Codes[0]);
			}
			words.insert(words.end(), packed->Words.begin(), packed->Words.end());
		}
		return expected;
	}
}

TEST_CASE(
	"parallel weather summaries reduce complete and partial dense pages exactly", "[render][gpu][weather][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		SummaryPipeline(device), releasePipeline
	);
	REQUIRE(pipeline);
	scene::WeatherCompute controls;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 0);
	REQUIRE(time);
	for (const glm::uvec3 dimensions : {glm::uvec3(8), glm::uvec3(11, 9, 10), glm::uvec3(1, 9, 2)}) {
		CAPTURE(dimensions.x, dimensions.y, dimensions.z);
		WeatherGpuGrid grid;
		grid.Origin = {-19, -5, -21};
		grid.Dimensions = dimensions;
		grid.BrickDimensions = (dimensions + glm::uvec3(7)) / glm::uvec3(8);
		grid.BrickCount = grid.BrickDimensions.x * grid.BrickDimensions.y * grid.BrickDimensions.z;
		grid.CellCount = dimensions.x * dimensions.y * dimensions.z;
		const auto uniforms = WeatherUniformsOf(compiled->Definition, *time, grid);
		REQUIRE(uniforms.Origin.w != 0);
		std::vector<uint32_t> words;
		std::vector<Summary> expected(grid.BrickCount, EMPTY_REDUCTION);
		for (uint32_t z = 0; z < dimensions.z; ++z)
			for (uint32_t y = 0; y < dimensions.y; ++y)
				for (uint32_t x = 0; x < dimensions.x; ++x) {
					const uint32_t density =
						dimensions == glm::uvec3(8) ? 255 : 17 + (x * 19 + y * 37 + z * 61) % 239;
					AppendDensity(words, density);
					const uint32_t page =
						x / 8 + grid.BrickDimensions.x * (y / 8 + grid.BrickDimensions.y * (z / 8));
					Accumulate(expected[page], density);
				}
		const std::array<glm::ivec4, 1> unusedPage{};
		const auto actual = ReadSummaries(device, pipeline.get(), uniforms, words, unusedPage);
		CHECK(actual == expected);
		uint32_t count = 0;
		for (const auto &summary : actual)
			count += summary[3];
		CHECK(count == grid.CellCount);
	}
}

TEST_CASE(
	"parallel sparse summaries preserve dense compact empty and unsafe page semantics",
	"[render][gpu][weather][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releasePipeline = [device](SDL_GPUComputePipeline *pipeline) {
		SDL_ReleaseGPUComputePipeline(device, pipeline);
	};
	std::unique_ptr<SDL_GPUComputePipeline, decltype(releasePipeline)> pipeline(
		SummaryPipeline(device), releasePipeline
	);
	REQUIRE(pipeline);
	scene::WeatherCompute controls;
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = 0;
	controls.ThicknessMetres = 12;
	controls.ExtentMetres = {64, 64, 64};
	controls.Coverage = 1;
	controls.VapourMixingRatio = controls.VapourScale;
	controls.BaseTemperatureCelsius = -50;
	controls.TemperatureVariationCelsius = 0;
	controls.TemperatureLapseCelsiusPerKilometre = 0;
	controls.GrowthRate = 0;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	for (uint64_t tick : {uint64_t{0}, uint64_t{251}, (uint64_t{1} << 60) + 29}) {
		CAPTURE(tick);
		const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), tick);
		REQUIRE(time);
		std::vector<uint32_t> words;
		std::array<glm::ivec4, 5> descriptors{
			{{-16, 0, -16, 1}, {-64, 0, -64, 2}, {-8, 64, -8, 0}, {-32, 0, -32, 1}, {-48, 0, -48, 1}}
		};
		std::vector<Summary> expected(descriptors.size(), EMPTY_REDUCTION);
		for (uint32_t cell = 0; cell < weather::BRICK_CELLS; ++cell) {
			const uint32_t density = (cell * 73) % 256;
			AppendDensity(words, density);
			Accumulate(expected[0], density);
		}
		for (uint32_t page : {1u, 3u, 4u}) {
			const auto offset = static_cast<uint32_t>(words.size());
			expected[page] =
				AppendCompactPage(words, descriptors[page], *compiled, *time, page != 1, page == 4);
			descriptors[page].w |= static_cast<int32_t>(32u | (offset << 7));
		}
		descriptors[2].w |= 64;
		expected[2] = {0, 0, 0, weather::BRICK_CELLS};
		REQUIRE(expected[3][2] > 0);
		REQUIRE(expected[3][1] == 0);
		WeatherGpuGrid grid;
		grid.Dimensions = {8, 8, 8};
		grid.CellCount = static_cast<uint32_t>(descriptors.size()) * weather::BRICK_CELLS;
		grid.BrickCount = static_cast<uint32_t>(descriptors.size());
		auto uniforms = WeatherUniformsOf(compiled->Definition, *time, grid);
		uniforms.Origin.w = 0;
		const auto actual = ReadSummaries(device, pipeline.get(), uniforms, words, descriptors);
		CHECK(actual == expected);
	}
}
