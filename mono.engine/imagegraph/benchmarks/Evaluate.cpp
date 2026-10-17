// Complete CPU source -> bilinear transform -> normal blend evaluation.
// Source pixels and compile plans are built before the benchmark harness starts.
// Samples retain normal source copies, intermediate allocations and budget checks.

#include "Pixels.hpp"

#include <engine/core/HeapProfile.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Bench.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("runtime heap probe requires MONO_HEAP_PROFILE");
		// grug single owner and no device threads: reset makes nested tags share one call's process peak.
		HeapProfile::ResetTotals();
		const auto before = HeapProfile::Totals();
		{
			ENGINE_HEAP_SCOPE(name);
			work();
		}
		const auto after = HeapProfile::Totals();
		int64_t peak = 0;
		bool found = false;
		for (uint32_t index = 1; index < HeapProfile::NodeCount(); ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Parent == HeapProfile::ROOT && node.Name == name) {
				found = true;
				peak = node.PeakBytes;
			}
		}
		if (!found) throw std::runtime_error("runtime benchmark heap scope was not recorded");
		std::cout << "# b0 heap\t" << name << '\t' << (after.TotalBytes - before.TotalBytes) << '\t'
				  << (after.TotalBlocks - before.TotalBlocks) << '\t' << (after.LiveBytes - before.LiveBytes)
				  << '\t' << after.LiveBytes << '\t' << peak << '\t' << after.LiveBlocks << '\t'
				  << after.PeakBytes << '\t' << before.LiveBytes << '\t'
				  << (after.PeakBytes - before.LiveBytes) << '\t' << after.OverheadBytes << '\t'
				  << HeapProfile::BlockOverhead() << '\t' << (after.DroppedScopes - before.DroppedScopes)
				  << '\t' << (after.ForeignFrees - before.ForeignFrees) << '\n';
	}

	void PrintHeapBasis() {
		std::cout << "# b0 heap basis\ttracked_cpp_requested_payload=1\t"
				  << "malloc_gpu_excluded=1\twork_invocations=1\tfixtures_prepared=1\t"
				  << "reference_setup_excluded=1\toutput_retained_at_snapshot=1\t"
				  << "scope_peak=exclusive\tprocess_peak=single_call_since_reset\t"
				  << "fixtures_references_resident=1\tpeak_above_entry=additional_tracked_payload_not_rss\t"
				  << "external_contention_invalidates_timing=1\ttiming_valid=0\t"
				  << "counters=process_local\n";
		std::cout << "# b0 heap v3\tname\tprocess_allocated_bytes\tprocess_allocated_blocks\t"
				  << "process_retained_bytes\tprocess_live_bytes\tscope_self_peak_bytes\t"
				  << "process_live_blocks\tprocess_peak_since_reset_bytes\tbaseline_live_bytes\t"
				  << "process_peak_above_entry_live_bytes\t"
				  << "process_header_overhead_model_bytes\tblock_overhead_bytes\t"
				  << "dropped_scopes_delta\tforeign_frees_delta\n";
	}

	uint64_t PixelHash(const Image &image) {
		uint64_t hash = 14695981039346656037ULL;
		for (std::byte byte : image.Pixels) {
			hash ^= std::to_integer<uint8_t>(byte);
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	void ProbeEvaluation(const char *name, const Fixture &fixture) {
		Image expected{1, 1, {std::byte{17}, std::byte{31}, std::byte{47}, std::byte{255}}};
		if (fixture.Graph.Nodes.size() > 1) {
			Image source;
			std::string failure;
			if (!fixture.Resolve("fixture.rgba", source, failure))
				throw std::runtime_error("imagegraph memory reference source refused: " + failure);
			expected = Reference(source, std::get<Transform>(fixture.Graph.Nodes[1].Value), 0.7);
		}
		Image actual;
		Diagnostic diagnostic;
		bool accepted = false;
		ProbeHeap(name, [&] {
			accepted = Evaluate(fixture.Graph, fixture.Compiled, "main", fixture.Resolve, actual, diagnostic);
		});
		if (!accepted || !diagnostic.Message.empty() || actual.Width != expected.Width ||
			actual.Height != expected.Height || actual.Space != expected.Space ||
			actual.Pixels != expected.Pixels)
			throw std::runtime_error("imagegraph memory probe full output changed");
		std::cout << "# b0 runtime oracle\t" << name << "\tfull_pixels=1\twidth=" << actual.Width
				  << "\theight=" << actual.Height << "\tbytes=" << actual.Pixels.size()
				  << "\tpixel_fnv1a64=" << PixelHash(actual) << '\n';
	}

	void ProbeMemoryOnce() {
		static const bool probed = [] {
			const char *setting = std::getenv("MONO_BENCH_MEMORY");
			if (setting == nullptr || setting[0] != '1' || setting[1] != '\0') return true;
			PrintHeapBasis();
			const Document empty;
			const Plan plan;
			const SourceResolver resolve;
			const Image expected{1, 1, {std::byte{17}, std::byte{31}, std::byte{47}, std::byte{255}}};
			Image actual = expected;
			Diagnostic diagnostic;
			bool accepted = false;
			ProbeHeap("b0 image runtime empty prepared", [&] {
				accepted = Evaluate(empty, plan, "main", resolve, actual, diagnostic);
			});
			if (accepted || diagnostic.Message.empty() || actual.Width != expected.Width ||
				actual.Height != expected.Height || actual.Space != expected.Space ||
				actual.Pixels != expected.Pixels)
				throw std::runtime_error("imagegraph memory probe empty refusal changed");
			std::cout << "# b0 runtime oracle\tb0 image runtime empty prepared"
					  << "\trefused=1\tdiagnostic=1\tfull_pixels_preserved=1\tpixel_fnv1a64="
					  << PixelHash(actual) << '\n';
			ProbeEvaluation("b0 image runtime simple prepared", SIMPLE);
			ProbeEvaluation("b0 image runtime busy prepared", SMALL);
			ProbeEvaluation("b0 image runtime complex prepared", LARGE);
			return true;
		}();
		(void)probed;
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
	imagegraph_bench::ProbeMemoryOnce();
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
