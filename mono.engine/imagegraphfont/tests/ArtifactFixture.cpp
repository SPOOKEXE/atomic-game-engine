#include "../benchmarks/ArtifactFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphfont.artifact_fixture")

using namespace engine::imagegraph;
using namespace engine::imagegraphfont;
using namespace font_boundary_fixture;

TEST_CASE(
	"artifact benchmark fixture round trips literal owned semantics with nonempty prior results",
	"[font_boundary_workloads]"
) {
	const auto fixture = MakeArtifactFixture();
	std::string failure;
	INFO(failure);
	REQUIRE(VerifyArtifactConfiguration(fixture, failure));
	REQUIRE(GraphFontConfigurationRetainedBytes(fixture));
	std::string encoded = "nonempty prior encoded result";
	encoded.reserve(4096);
	Diagnostic diagnostic;
	const bool written = WriteGraphFontConfiguration(fixture, encoded, ArtifactOperationBytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	REQUIRE(VerifyEncodedArtifact(encoded, failure));
	auto decoded = MakeArtifactFixture();
	decoded.Context.InitialFont->Data->Frames[0].Hash = 19;
	REQUIRE(GraphFontConfigurationRetainedBytes(decoded));
	REQUIRE(ReadGraphFontConfiguration(encoded, decoded, ArtifactOperationBytes, diagnostic));
	REQUIRE(VerifyArtifactConfiguration(decoded, failure));
	decoded.Context.InitialTextFonts[0].Fallback->Data->Frames[0].Pixels[3] = 99;
	CHECK(fixture.Context.InitialTextFonts[0].Fallback->Data->Frames[0].Pixels[3] == 44);
}

TEST_CASE(
	"artifact benchmark oracle independently detects profile hash metric and capability corruption",
	"[font_boundary_workloads]"
) {
	std::string failure;
	INFO(failure);
	for (size_t mutation = 0; mutation < 6; ++mutation) {
		auto fixture = MakeArtifactFixture();
		switch (mutation) {
		case 0:
			fixture.Context.BitmapTextureProfile = FontBitmapTextureProfile::SourceObserved;
			break;
		case 1:
			fixture.Observations[0].Font->Data->SourceTexture->Hash ^= 1;
			break;
		case 2:
			fixture.Observations[1].Font->Data->Frames[0].Pixels[3] = 99;
			break;
		case 3:
			fixture.Context.InitialTextFonts[0].Primary->Data->Glyphs[1].Advance = 9;
			break;
		case 4:
			fixture.Observations[0].Request.Authored.Values[2].Data = 10.0;
			break;
		case 5:
			fixture.ReadGrants[0].Write = true;
			break;
		}
		CHECK_FALSE(VerifyArtifactConfiguration(fixture, failure));
		CHECK_FALSE(failure.empty());
	}
}

TEST_CASE(
	"artifact benchmark byte cap refusal preserves complete prior encoded and decoded results",
	"[font_boundary_workloads]"
) {
	const auto fixture = MakeArtifactFixture();
	std::string encoded = "nonempty prior encoded result";
	const auto priorEncoded = encoded;
	Diagnostic diagnostic;
	REQUIRE_FALSE(WriteGraphFontConfiguration(fixture, encoded, encoded.capacity(), diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(encoded == priorEncoded);
	REQUIRE(WriteGraphFontConfiguration(fixture, encoded, ArtifactOperationBytes, diagnostic));
	auto destination = MakeArtifactFixture();
	const auto retained = GraphFontConfigurationRetainedBytes(destination);
	REQUIRE(retained);
	REQUIRE_FALSE(
		ReadGraphFontConfiguration(encoded, destination, *retained + encoded.size() - 1, diagnostic)
	);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	std::string failure;
	INFO(failure);
	REQUIRE(VerifyArtifactConfiguration(destination, failure));
}
