#include "FontPayload.hpp"
#include "FontTextLayout.hpp"
#include "NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.source_bitmap_font")
using namespace engine::imagegraph;
namespace {
	Document BitmapGraph(std::string map = "AB") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string{"glyphs"}}}},
			{"bitmap", "pc.font_bitmap", "", {}, {{"string_map", std::move(map)}}},
			{"font", "pc.font_data", "", {}, {}},
			{"pin", "pc.pin", "", {}, {}}
		};
		document.Links = {
			{"source", "image", "bitmap", "font_surfaces"},
			{"bitmap", "font", "font", "font"},
			{"font", "font", "pin", "in"}
		};
		document.Outputs = {{"out", "pin", "out"}};
		return document;
	}
	Plan BitmapPlan(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Document BitmapRestore(const Document &document) {
		Document restored;
		Diagnostic diagnostic;
		const auto status = Read(Write(document), restored, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(restored == document);
		return restored;
	}
	Image GlyphImage() {
		Image image{2, 1, {255, 0, 0, 255, 0, 255, 0, 128}};
		image.Hash = SurfaceHash(image);
		return image;
	}
}
TEST_CASE(
	"persisted bitmap font graph forwards owned glyphs through Font Data and Pin", "[source_bitmap_font]"
) {
	const auto document = BitmapRestore(BitmapGraph());
	const auto plan = BitmapPlan(document);
	std::array images{RequestImageSource{"glyphs", GlyphImage()}};
	EvaluationRequest request;
	request.ImageSources = images;
	EvaluatedValue output;
	Diagnostic diagnostic;
	const auto status = EvaluateValue(document, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *face = std::get_if<FontValue>(&output.Data);
	REQUIRE(face);
	REQUIRE(face->Data);
	REQUIRE(detail::ValidFontPayload(*face));
	REQUIRE(face->Data->Frames.size() == 1);
	REQUIRE(face->Data->Glyphs.size() == 2);
	CHECK(face->Data->Frames[0].Pixels == images[0].Data.Pixels);
	CHECK(face->Data->Glyphs[0].Character == 'A');
	CHECK(face->Data->Glyphs[0].Present);
	CHECK(face->Data->Glyphs[0].Frame == 0);
	CHECK(face->Data->Glyphs[0].Advance == 4);
	CHECK_FALSE(face->Data->Glyphs[1].Present);
	CHECK(face->Data->Glyphs[1].Advance == 2);
	images[0].Data.Pixels[0] = 17;
	CHECK(face->Data->Frames[0].Pixels[0] == 255);
	CHECK_FALSE(Write(Document{document}).empty());
}
TEST_CASE(
	"bitmap duplicate map keeps the last mapping even beyond the sprite frame list", "[source_bitmap_font]"
) {
	const auto document = BitmapGraph("AA");
	const auto plan = BitmapPlan(document);
	std::array images{RequestImageSource{"glyphs", GlyphImage()}};
	EvaluationRequest request;
	request.ImageSources = images;
	EvaluatedValue output;
	Diagnostic diagnostic;
	REQUIRE(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::Ok);
	const auto *face = std::get_if<FontValue>(&output.Data);
	REQUIRE(face);
	REQUIRE(face->Data->Glyphs.size() == 1);
	CHECK(face->Data->Glyphs[0].Character == 'A');
	CHECK_FALSE(face->Data->Glyphs[0].Present);
	CHECK_FALSE(face->Data->Glyphs[0].Frame);
	CHECK(face->Data->Glyphs[0].Advance == 2);
}
TEST_CASE(
	"bitmap single channel safe draw expands red and gives opaque glyph alpha", "[source_bitmap_font]"
) {
	const auto document = BitmapGraph("A");
	const auto plan = BitmapPlan(document);
	Image gray{2, 1, {17, 251}};
	gray.Format = SurfaceFormat::R8Unorm;
	gray.Hash = SurfaceHash(gray);
	std::array images{RequestImageSource{"glyphs", gray}};
	EvaluationRequest request;
	request.ImageSources = images;
	EvaluatedValue output;
	Diagnostic diagnostic;
	REQUIRE(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::Ok);
	const auto *face = std::get_if<FontValue>(&output.Data);
	REQUIRE(face);
	CHECK(face->Data->Frames[0].Format == SurfaceFormat::RGBA8Unorm);
	CHECK(face->Data->Frames[0].Pixels == std::vector<uint8_t>{17, 17, 17, 255, 251, 251, 251, 255});
}
TEST_CASE(
	"bitmap Font cannot masquerade as unrelated Text through direct or Font Data links",
	"[source_bitmap_font]"
) {
	auto document = BitmapGraph("A");
	document.Nodes.push_back({"text", "pc.string_length", "", {}, {}});
	document.Links.push_back({"bitmap", "font", "text", "text"});
	document.Outputs = {{"out", "text", "length"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "text");
	document.Links.back().FromNode = "font";
	plan = BitmapPlan(document);
	std::array images{RequestImageSource{"glyphs", GlyphImage()}};
	EvaluationRequest request;
	request.ImageSources = images;
	EvaluatedValue output;
	output.Data = int64_t{71};
	const auto before = output;
	CHECK(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "text");
	CHECK(output == before);
}
TEST_CASE(
	"bitmap font malformed Unicode and tight allocation refusal preserve prior outputs",
	"[source_bitmap_font]"
) {
	auto document = BitmapGraph(std::string{"\xC0\xAF", 2});
	auto plan = BitmapPlan(document);
	std::array images{RequestImageSource{"glyphs", GlyphImage()}};
	EvaluationRequest request;
	request.ImageSources = images;
	EvaluatedValue output;
	output.Data = std::string{"retained"};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "bitmap");
	CHECK(output == before);
	document = BitmapGraph("A");
	plan = BitmapPlan(document);
	const auto *entry = FindCatalogueEntry("pc.font_bitmap");
	REQUIRE(entry);
	detail::NodeContext context(document.Nodes[1], *entry, request);
	context.ByteBudget = 1;
	context.Images = {{"font_surfaces", &images[0].Data}};
	for (const auto &value : document.Nodes[1].Values)
		context.ValueViews.emplace_back(value.Port, &value.Data);
	const auto executor = detail::FindExecutor("pc.font_bitmap");
	REQUIRE(executor);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputValues.empty());
	CHECK(output == before);
}

TEST_CASE(
	"bitmap empty input refuses the freed source handle rather than retaining a live font",
	"[source_bitmap_font]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"bitmap", "pc.font_bitmap", "", {}, {{"string_map", std::string{"A"}}}}};
	document.Outputs = {{"font", "bitmap", "font"}};
	const auto plan = BitmapPlan(document);
	EvaluationRequest request;
	EvaluatedValue output;
	output.Data = int64_t{37};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(document, plan, "font", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "bitmap");
	CHECK(diagnostic.Port == "font_surfaces");
	CHECK(output == before);
}
TEST_CASE(
	"bitmap UTF16 map and heterogeneous frames use an owned pixel-center CPU profile", "[source_bitmap_font]"
) {
	auto document = BitmapGraph("\xF0\x9F\x98\x80");
	document.Nodes[1].Values.push_back({"separation", 0.0});
	Node surfaces{"surfaces", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	surfaces.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	document.Nodes.insert(document.Nodes.begin() + 1, surfaces);
	document.Nodes.insert(
		document.Nodes.begin() + 1,
		{"second", "image.captured", "", {}, {{"source_id", std::string{"second"}}}}
	);
	document.Links[0] = {"surfaces", "array", "bitmap", "font_surfaces"};
	document.Links.push_back({"source", "image", "surfaces", "input_0"});
	document.Links.push_back({"second", "image", "surfaces", "input_1"});
	const auto restored = BitmapRestore(document);
	const auto plan = BitmapPlan(restored);
	Image first{1, 1, {255, 0, 0, 255}};
	first.Hash = SurfaceHash(first);
	Image second{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}};
	second.Hash = SurfaceHash(second);
	std::array images{RequestImageSource{"glyphs", first}, RequestImageSource{"second", second}};
	EvaluationRequest request;
	request.ImageSources = images;
	EvaluatedValue output;
	Diagnostic diagnostic;
	const auto status = EvaluateValue(restored, plan, "out", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *font = std::get_if<FontValue>(&output.Data);
	REQUIRE(font);
	REQUIRE(font->Data);
	REQUIRE(font->Data->Frames.size() == 2);
	REQUIRE(font->Data->Glyphs.size() == 2);
	CHECK(font->Data->Characters == FontCharacterProfile::Utf16);
	CHECK(font->Data->Glyphs[0].Character == 0xd83d);
	CHECK(font->Data->Glyphs[1].Character == 0xde00);
	CHECK(font->Data->Frames[0].Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	CHECK(font->Data->Frames[1].Pixels == std::vector<uint8_t>{0, 255, 0, 255});
	images[1].Data.Pixels.assign(8, 0);
	CHECK(font->Data->Frames[1].Pixels[1] == 255);
}
