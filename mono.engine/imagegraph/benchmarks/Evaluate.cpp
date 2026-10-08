// Complete CPU source -> bilinear transform -> normal blend evaluation.
// Source pixels and compile plans are built before the benchmark harness starts.
// Samples retain normal source copies, intermediate allocations and budget checks.

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.bench.evaluate")

namespace imagegraph_bench {
	using namespace engine::imagegraph;
	using engine::testing::Consume;

	constexpr size_t SMALL_ITERATIONS = 4;
	constexpr size_t LARGE_ITERATIONS = 1;

	struct Fixture {
		Document Graph;
		Plan Compiled;
		SourceResolver Resolve;
	};

	// A repeating colour/alpha gradient exercises interpolation without random setup cost.
	Fixture BuildFixture(uint32_t side) {
		Image source{.Width = side, .Height = side, .Pixels = {}};
		source.Pixels.resize(static_cast<size_t>(side) * side * 4);
		for (uint32_t y = 0; y < side; y++) {
			for (uint32_t x = 0; x < side; x++) {
				const size_t offset = (static_cast<size_t>(y) * side + x) * 4;
				source.Pixels[offset] = std::byte{static_cast<uint8_t>(x % 256)};
				source.Pixels[offset + 1] = std::byte{static_cast<uint8_t>(y % 256)};
				source.Pixels[offset + 2] = std::byte{static_cast<uint8_t>((x + y) % 256)};
				source.Pixels[offset + 3] = std::byte{static_cast<uint8_t>(128 + (x + y) % 128)};
			}
		}
		Fixture fixture{
			.Graph =
				{
					.Nodes =
						{
							{"source", Source{"fixture.rgba"}, {}, {}},
							{"transform",
							 Transform{
								 side,
								 side,
								 3.5,
								 -2.25,
								 0.9,
								 1.05,
								 17,
								 side * 0.5,
								 side * 0.5,
								 Sampling::Bilinear
							 },
							 {"source"},
							 {}},
							{"blend", Blend{0.7}, {"source", "transform"}, {}},
						},
					.Outputs = {{"main", "blend"}},
				},
			.Compiled = {},
			.Resolve = [source =
							std::move(source)](std::string_view path, Image &image, std::string &failure) {
				if (path != "fixture.rgba") {
					failure = "unexpected benchmark source";
					return false;
				}
				image = source;
				return true;
			},
		};
		Diagnostic diagnostic;
		if (!Compile(fixture.Graph, fixture.Compiled, diagnostic))
			throw std::runtime_error("imagegraph benchmark fixture: " + diagnostic.Message);
		return fixture;
	}

	// Construction uses only local value data; no other translation unit's state is read.
	const Fixture SMALL = BuildFixture(256);
	const Fixture LARGE = BuildFixture(1024);

	void EvaluateRepeated(const Fixture &fixture, size_t iterations) {
		for (size_t iteration = 0; iteration < iterations; iteration++) {
			Image result;
			Diagnostic diagnostic;
			if (!Evaluate(fixture.Graph, fixture.Compiled, "main", fixture.Resolve, result, diagnostic))
				throw std::runtime_error("imagegraph benchmark refused: " + diagnostic.Message);
			Consume(result.Pixels.data());
			Consume(result.Pixels.size());
			Consume(result.Pixels[result.Pixels.size() / 2]);
		}
	}
}

BENCH("Evaluate · 256x256 source/bilinear transform/blend", imagegraph_bench::SMALL_ITERATIONS) {
	imagegraph_bench::EvaluateRepeated(imagegraph_bench::SMALL, imagegraph_bench::SMALL_ITERATIONS);
}

BENCH("Evaluate · 1024x1024 source/bilinear transform/blend", imagegraph_bench::LARGE_ITERATIONS) {
	imagegraph_bench::EvaluateRepeated(imagegraph_bench::LARGE, imagegraph_bench::LARGE_ITERATIONS);
}
