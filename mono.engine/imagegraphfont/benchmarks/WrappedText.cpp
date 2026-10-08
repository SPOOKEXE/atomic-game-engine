#include "../tests/fixtures/WrappedTextBoundary.hpp"
#include "FontBoundaryProfile.hpp"

TEST_SUITE_ID("engine.imagegraphfont.bench.wrapped-text")
using namespace engine::imagegraphfont::testing;
BENCH("Real BDF advances wrap 128 A A lines with native full-text measurement", 1) {
	static FontBoundaryProfile<WrappedTextBoundary> fixture(
		"wrapped_paragraph", WrappedTextBoundary::Operation::Paragraph, {"imagegraph.font.measure_native"}
	);
	fixture.Measure();
}
BENCH("Source space trimming scans 256 consecutive spaces with bounded work", 1) {
	static FontBoundaryProfile<WrappedTextBoundary> fixture(
		"wrapped_spaces", WrappedTextBoundary::Operation::Spaces, {"imagegraph.font.measure_native"}
	);
	fixture.Measure();
}
BENCH("Native font batch measures 64 distinct fractional-width requests", 1) {
	static FontBoundaryProfile<WrappedTextBoundary> fixture(
		"wrapped_batch", WrappedTextBoundary::Operation::Batch, {"imagegraph.font.measure_native"}
	);
	fixture.Measure();
}
BENCH("Signed character trim keeps native full paragraph extent through granted font host", 1) {
	static FontBoundaryProfile<WrappedTextBoundary> fixture(
		"wrapped_trimmed_text",
		WrappedTextBoundary::Operation::TrimmedText,
		{"imagegraphfont.font_inputs_bind",
		 "imagegraphfont.font_glyphs",
		 "imagegraph.font.measure_native",
		 "imagegraph.font.trim",
		 "imagegraph.text"}
	);
	fixture.Measure();
}
