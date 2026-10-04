#include "ArtifactBoundaryWorkload.hpp"
#include "FontBoundaryProfile.hpp"

TEST_SUITE_ID("engine.imagegraphfont.bench.boundaries")
using namespace engine::imagegraphfont::testing;
BENCH("Exact granted 421-byte BDF file to three owned coverage glyph observations", 1) {
	static FontBoundaryProfile<FontHostBoundary> fixture(
		"bdf_coverage",
		FontHostBoundary::Operation::Coverage,
		{"imagegraphfont.font_inputs_bind", "imagegraphfont.font_glyphs", "gui.font_glyphs.decode"}
	);
	fixture.Measure();
}
BENCH("Exact granted BDF file to real spread-8 bsdf and owned distance glyphs", 1) {
	static FontBoundaryProfile<FontHostBoundary> fixture(
		"bdf_distance",
		FontHostBoundary::Operation::Distance,
		{"imagegraphfont.font_inputs_bind", "imagegraphfont.font_glyphs", "gui.font_glyphs.decode"}
	);
	fixture.Measure();
}
BENCH("Pinned 876576-byte Inter to native coverage observations at 64 pixels", 1) {
	static FontBoundaryProfile<FontHostBoundary> fixture(
		"inter_coverage",
		FontHostBoundary::Operation::OutlineCoverage,
		{"imagegraphfont.font_inputs_bind", "imagegraphfont.font_glyphs", "gui.font_glyphs.decode"}
	);
	fixture.Measure();
}
BENCH("Pinned Inter to real spread-8 outline sdf at 64 pixels", 1) {
	static FontBoundaryProfile<FontHostBoundary> fixture(
		"inter_distance",
		FontHostBoundary::Operation::OutlineDistance,
		{"imagegraphfont.font_inputs_bind", "imagegraphfont.font_glyphs", "gui.font_glyphs.decode"}
	);
	fixture.Measure();
}
BENCH("Unicode upper aß to ASS 128 times through Text and exact BDF provider", 1) {
	static FontBoundaryProfile<FontHostBoundary> fixture(
		"unicode_text",
		FontHostBoundary::Operation::UnicodeText,
		{"imagegraphfont.font_inputs_bind",
		 "imagegraph.evaluate",
		 "imagegraph.font.unicode_case",
		 "imagegraphfont.font_glyphs",
		 "gui.font_glyphs.decode",
		 "imagegraph.text"}
	);
	fixture.Measure();
}
BENCH("Owned complete font input artifact Encode replacing nonempty bytes", 1) {
	static FontBoundaryProfile<ArtifactBoundaryWorkload> fixture(
		"artifact_encode", ArtifactBoundaryWorkload::Operation::Encode, {"imagegraphfont.artifact.encode"}
	);
	fixture.Measure();
}
BENCH("Owned complete font input artifact Decode replacing nonempty configuration", 1) {
	static FontBoundaryProfile<ArtifactBoundaryWorkload> fixture(
		"artifact_decode", ArtifactBoundaryWorkload::Operation::Decode, {"imagegraphfont.artifact.decode"}
	);
	fixture.Measure();
}
