// Complete CPU source -> bilinear transform -> normal blend evaluation.
// Source pixels and compile plans are built before the benchmark harness starts.
// Samples retain normal source copies, intermediate allocations and budget checks.

#include "Pixels.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Bench.hpp>

#include <cmath>
#include <numbers>
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

	void Verify(const Fixture &fixture, const Image &expected) {
		Image actual;
		Diagnostic diagnostic;
		if (!Evaluate(fixture.Graph, fixture.Compiled, "main", fixture.Resolve, actual, diagnostic))
			throw std::runtime_error("imagegraph benchmark preflight refused: " + diagnostic.Message);
		if (actual.Width != expected.Width || actual.Height != expected.Height ||
			actual.Space != expected.Space || actual.Pixels != expected.Pixels)
			throw std::runtime_error("imagegraph benchmark full output changed");
	}

	Image Reference(const Image &source, const Transform &transform, double opacity) {
		Image expected{.Width = source.Width, .Height = source.Height, .Pixels = {}};
		expected.Pixels.resize(source.Pixels.size());
		// The blend reads the transform's stored RGBA8 bytes, including its
		// rounding, rather than unrounded interpolation values.
		Image transformed{.Width = source.Width, .Height = source.Height, .Pixels = {}};
		transformed.Pixels.resize(source.Pixels.size());
		const double radians = std::remainder(transform.Degrees, 360.0) * std::numbers::pi / 180.0;
		const double cosine = std::cos(radians);
		const double sine = std::sin(radians);
		for (uint32_t y = 0; y < expected.Height; ++y)
			for (uint32_t x = 0; x < expected.Width; ++x) {
				const double relativeX = x + 0.5 - transform.PivotX - transform.TranslateX;
				const double relativeY = y + 0.5 - transform.PivotY - transform.TranslateY;
				const double sourceX =
					transform.PivotX + (cosine * relativeX + sine * relativeY) / transform.ScaleX;
				const double sourceY =
					transform.PivotY + (-sine * relativeX + cosine * relativeY) / transform.ScaleY;
				detail::Put(transformed, x, y, detail::Sample(source, sourceX, sourceY, Sampling::Bilinear));
			}
		for (uint32_t y = 0; y < expected.Height; ++y)
			for (uint32_t x = 0; x < expected.Width; ++x)
				detail::Put(
					expected,
					x,
					y,
					detail::Over(detail::At(source, x, y), detail::At(transformed, x, y), opacity)
				);
		return expected;
	}

	Fixture BuildSimpleFixture() {
		Fixture fixture{
			.Graph =
				{.Nodes = {{"solid", Solid{1, 1, {17, 31, 47, 255}}, {}, {}}},
				 .Outputs = {{"main", "solid"}}},
			.Compiled = {},
			.Resolve = {}
		};
		Diagnostic diagnostic;
		if (!Compile(fixture.Graph, fixture.Compiled, diagnostic))
			throw std::runtime_error("imagegraph simple fixture refused: " + diagnostic.Message);
		Verify(fixture, Image{1, 1, {std::byte{17}, std::byte{31}, std::byte{47}, std::byte{255}}});
		return fixture;
	}

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
		const Transform transform{
			side, side, 3.5, -2.25, 0.9, 1.05, 17, side * 0.5, side * 0.5, Sampling::Bilinear
		};
		const Image expected = Reference(source, transform, 0.7);
		Fixture fixture{
			.Graph =
				{
					.Nodes =
						{
							{"source", Source{"fixture.rgba"}, {}, {}},
							{"transform", transform, {"source"}, {}},
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
		Verify(fixture, expected);
		return fixture;
	}

	// Construction uses only local value data; no other translation unit's state is read.
	const Fixture SIMPLE = BuildSimpleFixture();
	const Fixture SMALL = BuildFixture(256);
	const Fixture LARGE = BuildFixture(1024);

	void EvaluateEmpty() {
		const Image expected{1, 1, {std::byte{17}, std::byte{31}, std::byte{47}, std::byte{255}}};
		Image actual = expected;
		Diagnostic diagnostic;
		if (Evaluate(Document{}, Plan{}, "main", {}, actual, diagnostic) || diagnostic.Message.empty() ||
			actual.Width != expected.Width || actual.Height != expected.Height ||
			actual.Pixels != expected.Pixels)
			throw std::runtime_error("imagegraph empty evaluation refusal changed");
		Consume(diagnostic.Message.data());
	}

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

BENCH("Evaluate empty document refusal", 1) {
	imagegraph_bench::EvaluateEmpty();
}

BENCH("Evaluate simple solid output", 1) {
	imagegraph_bench::EvaluateRepeated(imagegraph_bench::SIMPLE, 1);
}

BENCH("Evaluate once · 256x256 source/bilinear transform/blend", 1) {
	imagegraph_bench::EvaluateRepeated(imagegraph_bench::SMALL, 1);
}

BENCH("Evaluate · 256x256 source/bilinear transform/blend", imagegraph_bench::SMALL_ITERATIONS) {
	imagegraph_bench::EvaluateRepeated(imagegraph_bench::SMALL, imagegraph_bench::SMALL_ITERATIONS);
}

BENCH("Evaluate · 1024x1024 source/bilinear transform/blend", imagegraph_bench::LARGE_ITERATIONS) {
	imagegraph_bench::EvaluateRepeated(imagegraph_bench::LARGE, imagegraph_bench::LARGE_ITERATIONS);
}
