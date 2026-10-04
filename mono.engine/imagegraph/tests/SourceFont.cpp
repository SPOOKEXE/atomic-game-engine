#include "FontPayload.hpp"
#include "FontTextLayout.hpp"
#include "SourceFontObservationContext.hpp"
#include "SourceSeparatedVec2.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/SourceFont.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <tuple>

TEST_SUITE_ID("engine.imagegraph.sourcefont")
using namespace engine::imagegraph;
namespace {
	FontValue BitmapFace() {
		FontValue value;
		auto &font = value.Data.emplace();
		font.Frames.push_back({2, 3, std::vector<uint8_t>(24, 255)});
		font.LineHeight = 3;
		font.MissingAdvance = 1;
		font.SpaceAdvance = 3;
		font.HasCharacterRange = true;
		font.FirstCharacter = 'A';
		font.LastCharacter = 'W';
		for (uint32_t character : {'A', 'B', 'W'}) {
			FontGlyph glyph;
			glyph.Character = character;
			glyph.Frame = 0;
			glyph.Advance = character == 'W' ? 5 : 3;
			glyph.Width = 2;
			glyph.Height = 3;
			font.Glyphs.push_back(glyph);
		}
		return value;
	}
	SourceFontObservation Observation() {
		SourceFontObservation record;
		record.Request.Authored = {"text", "pc.text", "", {}, {}};
		record.Request.Role = "font";
		record.Request.ResolvedPath = "/granted/font.ttf";
		record.Request.Characters = {'A'};
		record.Font = BitmapFace();
		record.Font->Data->Characters = FontCharacterProfile::UnicodeScalar;
		record.Font->Data->Raster = FontRasterProfile::NativeGlyphCoverage;
		record.Font->Data->GlyphMapComplete = false;
		return record;
	}
}
TEST_CASE("owned font cloning preserves complete pixels and isolates forwarded storage", "[source_font]") {
	const auto font = BitmapFace();
	REQUIRE(detail::ValidFontPayload(font));
	CHECK(detail::PayloadType(Value{font}) == ValueType::Font);
	CHECK(detail::ValidRuntimeValue(Value{font}));
	CHECK_FALSE(detail::ValidValuePayload(Value{font}, false));
	auto clone = font;
	clone.Data->Frames[0].Pixels[0] = 17;
	clone.Data->Glyphs[0].Advance = 9;
	CHECK(font.Data->Frames[0].Pixels[0] == 255);
	CHECK(font.Data->Glyphs[0].Advance == 3);
	CHECK(clone != font);
	ArrayValue array;
	array.ElementType = ValueType::Font;
	array.Elements = {font, clone};
	REQUIRE(detail::ValidRuntimeValue(Value{array}));
	const auto bytes = ValueClonePayloadBytes(Value{array});
	REQUIRE(bytes);
	CHECK(*bytes >= 2 * detail::FontStorageBytes(font, false));
	array.Elements[1] = std::string("wrong");
	CHECK_FALSE(detail::ValidRuntimeValue(Value{array}));
}
TEST_CASE("font payloads reject malformed mappings frames and scalar character domains", "[source_font]") {
	auto font = BitmapFace();
	font.Data->Glyphs[1].Character = 'A';
	CHECK_FALSE(detail::ValidFontPayload(font));
	font = BitmapFace();
	font.Data->Glyphs[0].Frame = 1;
	CHECK_FALSE(detail::ValidFontPayload(font));
	font = BitmapFace();
	font.Data->Frames[0].Pixels.pop_back();
	CHECK_FALSE(detail::ValidFontPayload(font));
	font = BitmapFace();
	font.Data->Glyphs[0].Advance = std::nan("");
	CHECK_FALSE(detail::ValidFontPayload(font));
	font = BitmapFace();
	font.Data->Glyphs[0].Character = 0xd800;
	font.Data->Glyphs.resize(1);
	CHECK(detail::ValidFontPayload(font));
	font.Data->Characters = FontCharacterProfile::UnicodeScalar;
	CHECK_FALSE(detail::ValidFontPayload(font));
	font = BitmapFace();
	font.Data->SourceTexture = font.Data->Frames[0];
	font.Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
	CHECK(detail::ValidFontPayload(font));
	font.Data->Glyphs[0].TextureRectangle->X = std::numeric_limits<double>::max();
	CHECK_FALSE(detail::ValidFontPayload(font));
	font.Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 3, 1};
	CHECK_FALSE(detail::ValidFontPayload(font));
}
TEST_CASE("runtime font literals cannot accidentally serialize as a different resource", "[source_font]") {
	Document document;
	Node node{"value", "pc.pin", "", {}, {}};
	node.Values.push_back({"in", BitmapFace()});
	document.Nodes.push_back(node);
	CHECK(Write(document).empty());
	StructValue structure;
	structure.Data.emplace().Fields.push_back({"face", BitmapFace()});
	document.Nodes[0].Values[0].Data = structure;
	CHECK(Write(document).empty());
	CHECK(detail::ContainsFontLiteral(document.Nodes[0].Values[0].Data));
	ArrayValue emptyFonts;
	emptyFonts.ElementType = ValueType::Font;
	document.Nodes[0].Values[0].Data = emptyFonts;
	CHECK(Write(document).empty());
}
TEST_CASE(
	"font observations bind full row time identity and distinguish known absent files", "[source_font]"
) {
	auto record = Observation();
	Diagnostic diagnostic;
	REQUIRE(SourceFontObservationRetainedBytes(record));
	CHECK(
		ValidateSourceFontObservations(nullptr, {&record, 1}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::Ok
	);
	record.Request.SignedDistanceField = true;
	CHECK_FALSE(SourceFontObservationRetainedBytes(record));
	record.Font->Data->Raster = FontRasterProfile::SourceObserved;
	CHECK(SourceFontObservationRetainedBytes(record));
	record.Presence = SourceFontPresence::AbsentFile;
	CHECK_FALSE(SourceFontObservationRetainedBytes(record));
	record.Font.reset();
	CHECK(SourceFontObservationRetainedBytes(record));
	record.Request.Characters.push_back('A');
	CHECK_FALSE(SourceFontObservationRetainedBytes(record));
	const std::array records{Observation(), Observation()};
	CHECK(
		ValidateSourceFontObservations(nullptr, records, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::DuplicateId
	);
	CHECK(diagnostic.NodeId == "text");
	CHECK(
		ValidateSourceFontObservations(nullptr, {records.data(), 1}, 1, diagnostic) == Status::LimitExceeded
	);
}
TEST_CASE("text character wrap preserves source equality split and empty line geometry", "[source_font]") {
	const auto font = BitmapFace();
	detail::FontTextLayoutOptions options;
	options.MaximumLineWidth = 3;
	detail::FontTextLayout layout;
	std::string failure;
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "AB", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	REQUIRE(layout.Lines.size() == 3);
	CHECK(layout.Lines[0].Text.empty());
	CHECK(layout.Lines[0].Height == 0);
	CHECK(layout.Lines[1].Text == "A");
	CHECK(layout.Lines[2].Text == "B");
	CHECK(layout.Width == 3);
	CHECK(layout.Height == 6);
	CHECK(layout.CharacterCount == 2);
	options.LineGap = 2;
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "AB", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	CHECK(layout.Height == 10);
}
TEST_CASE("text word wrap and full text monospacing preserve pinned arithmetic", "[source_font]") {
	const auto font = BitmapFace();
	detail::FontTextLayoutOptions options;
	options.MaximumLineWidth = 6;
	options.SplitWord = false;
	detail::FontTextLayout layout;
	std::string failure;
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "A B", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	REQUIRE(layout.Lines.size() == 2);
	CHECK(layout.Lines[0].Text == "A ");
	CHECK(layout.Lines[1].Text == "B ");
	CHECK(layout.Lines[0].Width == 5);
	CHECK(layout.Width == 5);
	options = {};
	options.FullTextSize = true;
	options.Monospaced = true;
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "A\nB", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	CHECK(layout.Width == 15);
	CHECK(layout.Height == 6);
}
TEST_CASE("text titlecase trim Unicode identity and staging refusal publish atomically", "[source_font]") {
	const auto font = BitmapFace();
	detail::FontTextLayoutOptions options;
	options.ChangeCase = 3;
	detail::FontTextLayout layout;
	std::string failure;
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "ab AB", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	CHECK(layout.Text == "Ab AB");
	options = {};
	options.Trim = true;
	options.TrimType = 1;
	options.Range = {0, .5};
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "A B", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	CHECK(layout.Text == "A ");
	// Source string_splice keeps each delimiter attached to its preceding token.
	for (const auto &[text, type, range, expected] :
		 std::array<std::tuple<std::string_view, int64_t, Vector2, std::string_view>, 5>{
			 {{"AB B", 1, {0, .5}, "AB "},
			  {"A AB B", 1, {1. / 3, 2. / 3}, "AB "},
			  {"AB\nB", 2, {0, .5}, "AB\n"},
			  {"A  B", 1, {1. / 3, 2. / 3}, " "},
			  {"A B ", 1, {1. / 3, 2. / 3}, "B "}}
		 }) {
		options.TrimType = type;
		options.Range = range;
		INFO(text);
		REQUIRE(
			detail::BuildFontTextLayout(
				*font.Data, text, options, Limits::MaximumEvaluationBytes, layout, failure
			) == Status::Ok
		);
		CHECK(layout.Text == expected);
	}
	options = {};
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "\xF0\x9F\x98\x80", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	CHECK(layout.CharacterCount == 1);
	CHECK(layout.Width == 2);
	const auto before = layout.Text;
	options.ChangeCase = 2;
	CHECK(
		detail::BuildFontTextLayout(
			*font.Data, before, options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::UnsupportedExecution
	);
	CHECK(layout.Text == before);
	options = {};
	CHECK(
		detail::BuildFontTextLayout(*font.Data, "AB", options, 1, layout, failure) == Status::LimitExceeded
	);
	CHECK(layout.Text == before);
}

TEST_CASE(
	"partial retained glyph maps check the cased glyph rather than guessing coverage", "[source_font]"
) {
	auto font = BitmapFace();
	font.Data->Raster = FontRasterProfile::NativeGlyphCoverage;
	font.Data->Characters = FontCharacterProfile::UnicodeScalar;
	font.Data->GlyphMapComplete = false;
	font.Data->Glyphs = {font.Data->Glyphs.front()};
	detail::FontTextLayoutOptions options;
	options.ChangeCase = 1;
	detail::FontTextLayout layout;
	layout.Text = "prior";
	std::string failure;
	CHECK(
		detail::BuildFontTextLayout(
			*font.Data, "A", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::UnsupportedExecution
	);
	CHECK(layout.Text == "prior");
	font.Data->Glyphs[0].Character = 'a';
	REQUIRE(
		detail::BuildFontTextLayout(
			*font.Data, "A", options, Limits::MaximumEvaluationBytes, layout, failure
		) == Status::Ok
	);
	CHECK(layout.Text == "a");
	CHECK(layout.Width == 3);
}

TEST_CASE("font authored request admission includes retained split-axis backing capacity", "[source_font]") {
	Node authored{"mirror", "pc.mirror_polar", "", {}, {}};
	auto &inputs = authored.SourceSeparatedVec2Animators.emplace().Inputs;
	inputs.reserve(8);
	inputs.push_back({"position", {}});
	auto &keys = inputs[0].Axes[0].Keys;
	keys.reserve(32);
	Keyframe key;
	key.NodeId = "mirror";
	key.Port = "position";
	key.Data = .5;
	keys.push_back(std::move(key));
	const auto clone = detail::SeparatedVec2Bytes(authored, false);
	const auto retained = detail::SeparatedVec2Bytes(authored, true);
	const auto total = detail::SourceFontAuthoredRetainedBytes(authored);
	REQUIRE(clone);
	REQUIRE(retained);
	REQUIRE(total);
	REQUIRE(*retained > *clone);
	CHECK(*total >= *retained);
	auto compact = authored;
	compact.SourceSeparatedVec2Animators->Inputs.shrink_to_fit();
	compact.SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys.shrink_to_fit();
	const auto compactBytes = detail::SourceFontAuthoredRetainedBytes(compact);
	REQUIRE(compactBytes);
	CHECK(*total > *compactBytes);
	CHECK(authored == compact);
}

TEST_CASE("font authored admission counts an empty reserved sampler table", "[source_font]") {
	Node authored{"font", "pc.font_data", "", {}, {}};
	const auto baseline = detail::SourceFontAuthoredRetainedBytes(authored);
	REQUIRE(baseline);
	authored.NativeSamplerBindings.reserve(16);
	const auto retained = detail::SourceFontAuthoredRetainedBytes(authored);
	REQUIRE(retained);
	CHECK(*retained - *baseline == authored.NativeSamplerBindings.capacity() * sizeof(NativeSamplerBinding));
	CHECK(authored.NativeSamplerBindings.empty());
}

TEST_CASE("borrowed font retention validates complete backing before a caller clones it", "[source_font]") {
	auto font = BitmapFace();
	const auto retained = SourceFontValueRetainedBytes(font);
	REQUIRE(retained);
	CHECK(*retained == detail::FontStorageBytes(font, true));
	const auto preserved = font;
	CHECK(font == preserved);
	font.Data->Frames[0].Pixels.pop_back();
	CHECK_FALSE(SourceFontValueRetainedBytes(font));
	CHECK_FALSE(SourceFontValueRetainedBytes(FontValue{}));
}
