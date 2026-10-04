#include "FontTextBatch.hpp"
#include "FontTextCase.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.source_font_text")
using namespace engine::imagegraph;
namespace {
	const SourceFontContext &TextNativeNamespace() {
		static const SourceFontContext value = [] {
			SourceFontContext result;
			result.AliasMapKnown = true;
			result.DefaultFontPath = std::string{};
			result.Playing = false;
			return result;
		}();
		return value;
	}
	Document TextGraph(std::string text = "A", std::string map = "A") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string{"glyphs"}}}},
			{"bitmap", "pc.font_bitmap", "", {}, {{"string_map", std::move(map)}, {"separation", 0.0}}},
			{"text",
			 "pc.text",
			 "",
			 {},
			 {{"text", std::move(text)}, {"interpolate", EnumValue{1}}, {"oversample", EnumValue{3}}}}
		};
		document.Links = {{"source", "image", "bitmap", "font_surfaces"}, {"bitmap", "font", "text", "font"}};
		document.Outputs = {{"image", "text", "surface_out"}, {"atlas", "text", "draw_data"}};
		return document;
	}
	void TextSet(Document &document, std::string id, Value value) {
		auto &values = document.Nodes.back().Values;
		for (auto &input : values)
			if (input.Port == id) {
				input.Data = std::move(value);
				return;
			}
		values.push_back({std::move(id), std::move(value)});
	}
	Plan TextPlan(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Document TextRestore(const Document &document) {
		Document result;
		Diagnostic diagnostic;
		const auto status = Read(Write(document), result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(result == document);
		return result;
	}
	Image TextImage(const Document &document, const EvaluationRequest &request) {
		const auto plan = TextPlan(document);
		Image output;
		Diagnostic diagnostic;
		const auto status = Evaluate(document, plan, "image", request, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output;
	}
	std::array<RequestImageSource, 1> TextImages(Image image = {1, 1, {255, 255, 255, 255}}) {
		image.Hash = SurfaceHash(image);
		return {RequestImageSource{"glyphs", std::move(image)}};
	}
}
TEST_CASE(
	"persisted bitmap Text graph draws literal coverage with source alpha multiply", "[source_font_text]"
) {
	const auto document = TextRestore(TextGraph());
	const auto images = TextImages({1, 1, {255, 0, 0, 128}});
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(document, request);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 0, 0, 128});
	CHECK(output.Hash == SurfaceHash(output));
}
TEST_CASE("Text dynamic lines and tracking preserve literal line geometry", "[source_font_text]") {
	auto document = TextGraph("AA\nA");
	TextSet(document, "letter_spacing", 1.0);
	TextSet(document, "line_height", 1.0);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 3);
	CHECK(output.Height == 3);
	CHECK(detail::ReadPixel(output, 0, 0) == detail::Rgba{1, 1, 1, 1});
	CHECK(detail::ReadPixel(output, 1, 0) == detail::Rgba{});
	CHECK(detail::ReadPixel(output, 2, 0) == detail::Rgba{1, 1, 1, 1});
	CHECK(detail::ReadPixel(output, 0, 2) == detail::Rgba{1, 1, 1, 1});
}
TEST_CASE("Text fixed project dimensions and authored padding remain distinct", "[source_font_text]") {
	auto document = TextGraph();
	document.Project.emplace();
	document.Project->SurfaceWidth = 4;
	document.Project->SurfaceHeight = 3;
	TextSet(document, "dimension", EnumValue{0});
	TextSet(document, "padding", Vector4{1, 2, 3, 4});
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 8);
	CHECK(output.Height == 9);
	CHECK(detail::ReadPixel(output, 3, 2) == detail::Rgba{1, 1, 1, 1});
	CHECK(detail::ReadPixel(output, 0, 0) == detail::Rgba{});
}
TEST_CASE("Text color and palette multiply use packed channel truncation", "[source_font_text]") {
	auto document = TextGraph("AA");
	TextSet(document, "color", Colour{128, 255, 255, 255});
	TextSet(
		document,
		"color_by_letter",
		ArrayValue{ValueType::Colour, {Colour{128, 255, 255, 255}, Colour{255, 0, 0, 255}}}
	);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 255, 255, 255, 128, 0, 0, 255});
}
TEST_CASE("Text ASCII casing and character trimming transform actual layout", "[source_font_text]") {
	auto document = TextGraph("aa", "A");
	TextSet(document, "change_case", EnumValue{2});
	TextSet(document, "trim", true);
	TextSet(document, "range", Vector2{0, .5});
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}
TEST_CASE(
	"Text Unicode casing uses exact owned transform and refuses absent evidence atomically",
	"[source_font_text]"
) {
	auto document = TextGraph("\xC3\x9F", "S");
	TextSet(document, "change_case", EnumValue{2});
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto plan = TextPlan(document);
	Image output{1, 1, {11, 12, 13, 14}};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "change_case");
	CHECK(output == before);
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.TextTransforms = {{"\xC3\x9F", "SS", 2}};
	request.SourceFonts = &context;
	output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 2);
	CHECK(output.Pixels == std::vector<uint8_t>(8, 255));
	context.TextTransforms.push_back(context.TextTransforms.front());
	const auto recorded = output;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(output == recorded);
}
TEST_CASE("Text Atlas output owns original pixels and draw placement", "[source_font_text]") {
	auto document = TextGraph("AA");
	TextSet(document, "atlas", true);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto plan = TextPlan(TextRestore(document));
	EvaluatedValue output;
	Diagnostic diagnostic;
	const auto status = EvaluateValue(document, plan, "atlas", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *array = std::get_if<ArrayValue>(&output.Data);
	REQUIRE(array);
	CHECK(array->ElementType == ValueType::Atlas);
	REQUIRE(array->Elements.size() == 2);
	const auto &first = std::get<AtlasValue>(array->Elements[0]);
	const auto &second = std::get<AtlasValue>(array->Elements[1]);
	REQUIRE(first.Data);
	REQUIRE(second.Data);
	CHECK(first.Data->Dimension == Vector2{1, 1});
	CHECK(second.Data->Position == Vector2{1, 0});
	REQUIRE(first.Data->OriginalSurface);
	CHECK(first.Data->OriginalSurface->Data.Pixels == std::vector<uint8_t>(8, 255));
	CHECK(first.Data->Surface.Data.Pixels == std::vector<uint8_t>(4, 255));
}
TEST_CASE(
	"Text empty default produces one transparent pixel without ambient font discovery", "[source_font_text]"
) {
	auto document = TextGraph("");
	document.Links.clear();
	document.Nodes.erase(document.Nodes.begin(), document.Nodes.begin() + 2);
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.DefaultFontPath = std::string{};
	request.SourceFonts = &context;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>(4, 0));
}
TEST_CASE("Text linked text arrays render all selected rows and preserve persistence", "[source_font_text]") {
	auto document = TextGraph();
	Node strings{"strings", "pc.array", "", {}, {{"type", EnumValue{4}}}};
	strings.DynamicInputs = {
		{"input_0", ValueType::Text, std::string{"A"}}, {"input_1", ValueType::Text, std::string{"AA"}}
	};
	document.Nodes.insert(document.Nodes.begin(), std::move(strings));
	document.Links.push_back({"strings", "array", "text", "text"});
	const auto restored = TextRestore(document);
	const auto plan = TextPlan(restored);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	ImageArray output;
	Diagnostic diagnostic;
	const auto status = EvaluateArray(restored, plan, "image", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Width == 1);
	CHECK(output.Images[1].Width == 2);
	CHECK(output.Images[0].Pixels == std::vector<uint8_t>(4, 255));
	CHECK(output.Images[1].Pixels == std::vector<uint8_t>(8, 255));
	SECTION("Each processor row retains its whole glyph Atlas array") {
		auto atlasDocument = restored;
		TextSet(atlasDocument, "atlas", true);
		EvaluatedValue atlasOutput;
		const auto atlasStatus =
			EvaluateValue(atlasDocument, TextPlan(atlasDocument), "atlas", request, atlasOutput, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(atlasStatus == Status::Ok);
		const auto *rows = std::get_if<ArrayValue>(&atlasOutput.Data);
		REQUIRE(rows);
		REQUIRE(rows->Nested.size() == 2);
		REQUIRE(rows->Nested[0].size() == 1);
		REQUIRE(rows->Nested[1].size() == 2);
		const auto &first = std::get<AtlasValue>(rows->Nested[0][0]);
		const auto &last = std::get<AtlasValue>(rows->Nested[1][1]);
		REQUIRE(first.Data);
		REQUIRE(last.Data);
		REQUIRE(first.Data->OriginalSurface);
		REQUIRE(last.Data->OriginalSurface);
		CHECK(first.Data->OriginalSurface->Data.Pixels == std::vector<uint8_t>(4, 255));
		CHECK(last.Data->OriginalSurface->Data.Pixels == std::vector<uint8_t>(8, 255));
		CHECK(last.Data->Position == Vector2{1, 0});
	}
}
TEST_CASE(
	"Text exact coverage refuses output and invalid authored format refuses compilation", "[source_font_text]"
) {
	auto document = TextGraph();
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	request.RequireSourceGpuRasterCoverage = true;
	Image output{1, 1, {9, 8, 7, 6}};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(
		Evaluate(document, TextPlan(document), "image", request, output, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(output == before);
	request.RequireSourceGpuRasterCoverage = false;
	TextSet(document, "attribute_color_depth", EnumValue{9});
	Plan rejected;
	CHECK(Compile(document, rejected, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "text");
	CHECK(diagnostic.Port == "attribute_color_depth");
	CHECK(output == before);
}
TEST_CASE(
	"Text recorded transform ownership and request context are part of font replay identity",
	"[source_font_text]"
) {
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.TextTransforms = {{"\xC3\x9F", "SS", 2}};
	REQUIRE(SourceFontContextRetainedBytes(context));
	std::optional<std::string_view> transformed;
	std::string failure;
	REQUIRE(detail::FindFontTextCase("\xC3\x9F", 2, &context, transformed, failure) == Status::Ok);
	REQUIRE(transformed);
	CHECK(*transformed == "SS");
	auto changed = context;
	changed.TextTransforms[0].Transformed = "ss";
	SourceFontRequest first, second;
	first.Context = context;
	second.Context = changed;
	CHECK(first != second);
	context.TextTransforms[0].Transformed = "\xFF";
	CHECK_FALSE(SourceFontContextRetainedBytes(context));
}

TEST_CASE(
	"Text later fixed dimension row rejects whole batch work before candidate output pixels",
	"[source_font_text]"
) {
	auto document = TextGraph();
	TextSet(document, "dimension", EnumValue{0});
	TextSet(document, "render_background", true);
	Node dimensions{"dimensions", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	dimensions.DynamicInputs = {
		{"input_0", ValueType::Any, Vector2{1000, 1000}}, {"input_1", ValueType::Any, Vector2{1100, 1000}}
	};
	document.Nodes.insert(document.Nodes.begin(), std::move(dimensions));
	document.Links.push_back({"dimensions", "array", "text", "fixed_dimension"});
	// An actual background image uses the admitted eight-unit composition path.
	document.Links.push_back({"source", "image", "text", "background"});
	const auto restored = TextRestore(document);
	const auto plan = TextPlan(restored);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	ImageArray output;
	output.Images = {{1, 1, {1, 2, 3, 4}}};
	const auto prior = output;
	Diagnostic diagnostic;
	CHECK(EvaluateArray(restored, plan, "image", request, output, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "text");
	INFO(diagnostic.Message);
	CHECK(diagnostic.Message.find("whole processor batch") != std::string::npos);
	CHECK(output.Images == prior.Images);
	CHECK(output.Items.empty());
}
TEST_CASE(
	"Text admitted plans reject aggregate candidate bytes without creating row pixels", "[source_font_text]"
) {
	const auto document = TextGraph();
	const auto *entry = FindCatalogueEntry("pc.text");
	REQUIRE(entry);
	FontValue font;
	auto &face = font.Data.emplace();
	face.LineHeight = 1;
	face.Frames = {{1, 1, {255, 255, 255, 255}}};
	FontGlyph glyph;
	glyph.Character = 'A';
	glyph.Frame = 0;
	glyph.Width = 1;
	glyph.Height = 1;
	glyph.Advance = 1;
	face.Glyphs = {glyph};
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	detail::EvaluationBudget budget(1024 * 1024);
	detail::NodeContext context(document.Nodes.back(), *entry, request, budget);
	context.ByteBudget = 1024 * 1024;
	context.ProcessorCount = 2;
	context.Values = {
		{"text", std::string{"A"}},
		{"font", font},
		{"dimension", EnumValue{0}},
		{"fixed_dimension", Vector2{512, 512}},
		{"color_by_letter", ArrayValue{ValueType::Colour, {Colour{255, 255, 255, 255}}}},
		{"interpolate", EnumValue{1}},
		{"oversample", EnumValue{3}}
	};
	detail::FontTextBatch batch;
	REQUIRE(detail::BeginFontTextBatch(context, 2, batch));
	context.ProcessorRow = 0;
	REQUIRE(detail::PrepareFontTextRow(context, batch));
	context.ProcessorRow = 1;
	REQUIRE(detail::PrepareFontTextRow(context, batch));
	CHECK_FALSE(detail::AdmitFontTextBatch(context, batch));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputValues.empty());
	CHECK(context.DataUpdates.empty());
}

TEST_CASE(
	"Text empty frame retains prior format and Atlas through owned replay and bounded retry",
	"[source_font_text]"
) {
	auto document = TextGraph();
	TextSet(document, "atlas", true);
	TextSet(document, "attribute_color_depth", EnumValue{4});
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	StatefulEvaluationResult first;
	Diagnostic diagnostic;
	const auto firstStatus =
		EvaluateStateful(document, TextPlan(document), "atlas", request, first, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(firstStatus == Status::Ok);
	REQUIRE(first.Data.Entries.size() == 1);
	const auto firstArray = std::get<ArrayValue>(std::get<EvaluatedValue>(first.Output).Data);
	REQUIRE(firstArray.Elements.size() == 1);
	TextSet(document, "text", std::string{});
	TextSet(document, "atlas", false);
	TextSet(document, "attribute_color_depth", EnumValue{3});
	request.Tick = 1;
	request.DataReplay = &first.Data;
	StatefulEvaluationResult next;
	const auto status = EvaluateStateful(document, TextPlan(document), "image", request, next, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &image = std::get<Image>(next.Output);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK(image.Format == SurfaceFormat::RGBA16Float);
	const auto &state = std::get<StructValue>(next.Data.Entries[0].Values[0].Data);
	const auto &rows = std::get<ArrayValue>(state.Data->Fields[2].second);
	const auto &row = std::get<StructValue>(rows.Elements[0]);
	CHECK(std::get<ArrayValue>(row.Data->Fields[1].second) == firstArray);
	const auto prior = next.Data;
	CHECK(
		EvaluateStateful(document, TextPlan(document), "image", request, next, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(next.Data == prior);
	REQUIRE(EvaluateStateful(document, TextPlan(document), "image", request, next, diagnostic) == Status::Ok);
	CHECK(next.Data == prior);
}

TEST_CASE(
	"Text wave callback starts at source index one and consumes literal sine controls", "[source_font_text]"
) {
	auto document = TextGraph("AA");
	TextSet(document, "dimension", EnumValue{0});
	TextSet(document, "fixed_dimension", Vector2{2, 5});
	TextSet(document, "offset", Vector2{0, 1});
	TextSet(document, "wave", true);
	TextSet(document, "wave_amplitude", 1.0);
	TextSet(document, "wave_scale", 90.0);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(detail::ReadPixel(output, 0, 3) == detail::Rgba{1, 1, 1, 1});
	CHECK(detail::ReadPixel(output, 1, 2) == detail::Rgba{1, 1, 1, 1});
	CHECK(detail::ReadPixel(output, 0, 2) == detail::Rgba{});
	CHECK(detail::ReadPixel(output, 1, 3) == detail::Rgba{});
}
TEST_CASE(
	"Text palette Ping-pong duplicates the source upper endpoint in source order", "[source_font_text]"
) {
	auto document = TextGraph("AAAAA");
	TextSet(document, "color_by_letter_select", EnumValue{1});
	TextSet(
		document,
		"color_by_letter",
		ArrayValue{
			ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}, Colour{0, 0, 255, 255}}
		}
	);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 0,	  0, 255, 0,   255, 0, 255, 0, 0,
												255, 255, 0, 0,	  255, 255, 0, 255, 0, 255});
}
TEST_CASE(
	"Text random palette consumes actual resolved-control recordings without drawing ambient RNG",
	"[source_font_text]"
) {
	auto document = TextGraph("AA");
	TextSet(document, "color_by_letter_select", EnumValue{2});
	TextSet(
		document,
		"color_by_letter",
		ArrayValue{ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}}}
	);
	const auto plan = TextPlan(TextRestore(document));
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	Image output{1, 1, {4, 3, 2, 1}};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == before);
	SourceBuiltinRandomCapture capture;
	const auto status =
		PrepareSourceBuiltinRandomCapture(document, plan, "text", request, capture, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	capture.Draws = {
		{SourceBuiltinRandomOperation::IRandom, 0, 1, 1}, {SourceBuiltinRandomOperation::IRandom, 0, 1, 0}
	};
	std::array records{capture};
	request.BuiltinRandomCaptures = records;
	REQUIRE(Evaluate(document, plan, "image", request, output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 255, 0, 255, 255, 0, 0, 255});
	const auto good = output;
	records[0].Draws.pop_back();
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == good);
}

TEST_CASE("Text wave and palette random observations preserve the source call order", "[source_font_text]") {
	auto document = TextGraph("AA");
	TextSet(document, "wave", true);
	TextSet(document, "wave_shape", 3.0);
	TextSet(document, "wave_amplitude", 1.0);
	TextSet(document, "color_by_letter_select", EnumValue{2});
	TextSet(
		document,
		"color_by_letter",
		ArrayValue{ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}}}
	);
	const auto plan = TextPlan(document);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	SourceBuiltinRandomCapture capture;
	Diagnostic diagnostic;
	REQUIRE(
		PrepareSourceBuiltinRandomCapture(document, plan, "text", request, capture, diagnostic) == Status::Ok
	);
	capture.Draws = {
		{SourceBuiltinRandomOperation::RandomRange, -1, 1, 1},
		{SourceBuiltinRandomOperation::IRandom, 0, 1, 0},
		{SourceBuiltinRandomOperation::RandomRange, -1, 1, -1},
		{SourceBuiltinRandomOperation::IRandom, 0, 1, 1}
	};
	std::array records{capture};
	request.BuiltinRandomCaptures = records;
	Image output;
	REQUIRE(Evaluate(document, plan, "image", request, output, diagnostic) == Status::Ok);
	CHECK(output.Height == 3);
	CHECK(detail::ReadPixel(output, 0, 2) == detail::Rgba{1, 0, 0, 1});
	CHECK(detail::ReadPixel(output, 1, 0) == detail::Rgba{0, 1, 0, 1});
	const auto before = output;
	std::swap(records[0].Draws[0], records[0].Draws[1]);
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(output == before);
}
TEST_CASE(
	"Text native path placement uses distance and captured fixed canvas without scaling glyphs",
	"[source_font_text]"
) {
	auto document = TextGraph();
	Path2D path;
	// Raw path handles are offsets from each anchor; zero handles form a straight segment.
	path.Anchors = {{{0, 1, 0, 0, 0, 0}, 0}, {{4, 1, 0, 0, 0, 0}, 1}};
	TextSet(document, "path", path);
	TextSet(document, "fixed_dimension", Vector2{4, 3});
	TextSet(document, "path_shift", 1.0);
	TextSet(document, "rotate_along_path", false);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	const auto output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 4);
	CHECK(output.Height == 3);
	CHECK(detail::ReadPixel(output, 1, 1) == detail::Rgba{1, 1, 1, 1});
	CHECK(detail::ReadPixel(output, 0, 0) == detail::Rgba{});
}

TEST_CASE(
	"Text owned initial bitmap atlas evidence enables texture and debug output with literal pixels",
	"[source_font_text]"
) {
	auto document = TextGraph();
	document.Nodes.erase(document.Nodes.begin(), document.Nodes.begin() + 2);
	document.Links.clear();
	document.Nodes.insert(
		document.Nodes.begin(), {"texture", "image.captured", "", {}, {{"source_id", std::string{"glyphs"}}}}
	);
	document.Links = {{"texture", "image", "text", "texture"}};
	FontValue font;
	auto &face = font.Data.emplace();
	face.LineHeight = 1;
	face.Frames = {{1, 1, {255, 0, 0, 255}}};
	FontGlyph glyph;
	glyph.Character = 'A';
	glyph.Frame = 0;
	glyph.Advance = 1;
	glyph.Width = 1;
	glyph.Height = 1;
	face.Glyphs = {glyph};
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.DefaultFontPath = std::string{};
	context.InitialFont = font;
	const auto images = TextImages({1, 1, {0, 0, 255, 255}});
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.SourceFonts = &context;
	request.ImageSources = images;
	const auto plan = TextPlan(document);
	Image output{1, 1, {3, 4, 5, 6}};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == before);
	context.InitialFont->Data->SourceTexture = face.Frames[0];
	context.InitialFont->Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
	output = TextImage(TextRestore(document), request);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	TextSet(document, "attribute_debug_texture", true);
	output = TextImage(TextRestore(document), request);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 0, 255});
}

TEST_CASE(
	"Text request admits retained host ownership before raster growth and preserves prior output",
	"[source_font_text][font_host_residency]"
) {
	auto document = TextGraph();
	const auto plan = TextPlan(document);
	const auto images = TextImages();
	EvaluationRequest request;
	request.SourceFonts = &TextNativeNamespace();
	request.ImageSources = images;
	StatefulEvaluationResult result;
	Diagnostic diagnostic;
	REQUIRE(EvaluateStateful(document, plan, "image", request, result, diagnostic) == Status::Ok);
	const auto before = result;
	request.SourceFontHostResidentBytes = UINT64_MAX;
	CHECK(EvaluateStateful(document, plan, "image", request, result, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Port == "font_inputs");
	CHECK(std::get<Image>(result.Output) == std::get<Image>(before.Output));
	CHECK(result.Data == before.Data);
	CHECK(result.Rigid == before.Rigid);
	CHECK(result.Simulation == before.Simulation);
	CHECK(result.Random == before.Random);
	CHECK(result.Surfaces == before.Surfaces);
	request.SourceFontHostResidentBytes = 4096;
	CHECK(
		EvaluateStateful(document, plan, "image", request, result, diagnostic, 4095) == Status::LimitExceeded
	);
	CHECK(std::get<Image>(result.Output) == std::get<Image>(before.Output));
	CHECK(result.Data == before.Data);
	CHECK(result.Rigid == before.Rigid);
	CHECK(result.Simulation == before.Simulation);
	CHECK(result.Random == before.Random);
	CHECK(result.Surfaces == before.Surfaces);
	REQUIRE(EvaluateStateful(document, plan, "image", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(result.Output) == std::get<Image>(before.Output));
}

TEST_CASE(
	"fresh held-playing Text uses exact per-instance primary and fallback seed without provider",
	"[source_font_text][font_initial_text]"
) {
	auto document = TextGraph("B");
	document.Nodes = {document.Nodes.back()};
	document.Links.clear();
	TextSet(document, "font", std::string("/held/font.bdf"));
	FontValue primary, fallback;
	for (auto *font : {&primary, &fallback}) {
		auto &data = font->Data.emplace();
		data.LineHeight = 1;
		data.Frames = {{1, 1, {0, 0, 255, 255}}};
		FontGlyph glyph;
		glyph.Character = font == &primary ? 'A' : 'B';
		glyph.Frame = 0;
		glyph.Advance = glyph.Width = glyph.Height = 1;
		data.Glyphs = {glyph};
	}
	SourceFontContext context = TextNativeNamespace();
	context.Playing = true;
	context.InitialTextFonts = {{"text", primary, fallback}};
	EvaluationRequest request;
	request.SourceFonts = &context;
	Image output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	const auto before = output;
	Diagnostic diagnostic;
	const auto plan = TextPlan(document);
	context.InitialTextFonts[0].NodeId = "unknown";
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "font_inputs");
	CHECK(output == before);
	context.InitialTextFonts[0].NodeId = "text";
	context.InitialTextFonts.push_back(context.InitialTextFonts[0]);
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(output == before);
	context.InitialTextFonts.clear();
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == before);
	context.InitialTextFonts = {{"text", primary, fallback}};
	StatefulEvaluationResult retained;
	REQUIRE(EvaluateStateful(document, plan, "image", request, retained, diagnostic) == Status::Ok);
	context.InitialTextFonts[0].Fallback->Data->Frames[0].Pixels = {255, 0, 0, 255};
	request.DataReplay = &retained.Data;
	request.Tick = 1;
	CHECK(TextImage(document, request).Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	StatefulEvaluationResult refused = retained;
	CHECK(
		EvaluateStateful(document, plan, "image", request, refused, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(refused.Data == retained.Data);
	CHECK(std::get<Image>(refused.Output) == std::get<Image>(retained.Output));
}

TEST_CASE(
	"Text smoothed filtered sampling refuses aggregate work before publishing local or modulated pixels",
	"[source_font_text][font_sampling_work]"
) {
	for (size_t samplingPath = 0; samplingPath < 3; ++samplingPath) {
		const bool modulated = samplingPath != 0;
		auto document = TextGraph();
		TextSet(document, "dimension", EnumValue{0});
		TextSet(document, "scale_to_fit", true);
		TextSet(document, "size", int64_t{16});
		TextSet(document, "anti_aliasing", false);
		TextSet(document, "interpolate", EnumValue{3});
		Node dimensions{"dimensions", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		dimensions.DynamicInputs = {
			{"input_0", ValueType::Any, Vector2{1200, 1200}}, {"input_1", ValueType::Any, Vector2{1200, 1200}}
		};
		document.Nodes.insert(document.Nodes.begin(), std::move(dimensions));
		document.Links.push_back({"dimensions", "array", "text", "fixed_dimension"});
		if (modulated) {
			document.Nodes.insert(
				document.Nodes.begin(),
				{"texture", "image.captured", "", {}, {{"source_id", std::string{"modulation"}}}}
			);
			document.Links.push_back({"texture", "image", "text", "texture"});
		}
		document.Outputs.push_back({"font", "bitmap", "font"});
		const auto restored = TextRestore(document);
		const auto plan = TextPlan(restored);
		std::array<RequestImageSource, 2> images{
			{{"glyphs", {1, 1, {255, 255, 255, 255}}}, {"modulation", {1, 1, {255, 255, 255, 255}}}}
		};
		for (auto &image : images)
			image.Data.Hash = SurfaceHash(image.Data);
		auto context = TextNativeNamespace();
		if (samplingPath == 1) context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
		EvaluationRequest request;
		request.SourceFonts = &context;
		request.ImageSources = images;
		std::array<SourceFontObservation, 2> observations;
		if (samplingPath == 2) {
			EvaluatedValue sourceFont;
			Diagnostic fontDiagnostic;
			REQUIRE(EvaluateValue(restored, plan, "font", request, sourceFont, fontDiagnostic) == Status::Ok);
			const auto *owned = std::get_if<FontValue>(&sourceFont.Data);
			REQUIRE(owned);
			for (size_t row = 0; row < observations.size(); ++row) {
				auto &observation = observations[row];
				observation.Request.Authored = restored.Nodes.back();
				observation.Request.Context = context;
				observation.Request.ProcessorRow = row;
				observation.Request.Role = "bitmap_texture";
				observation.Request.PixelSize = 16;
				observation.Request.FontInput = *owned;
				observation.Request.Characters = {'A'};
				observation.Font = *owned;
				observation.Font->Data->SourceTexture = observation.Font->Data->Frames[0];
				observation.Font->Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
				REQUIRE(SourceFontObservationRetainedBytes(observation));
			}
			request.FontObservations = observations;
		}
		ImageArray output;
		output.Images = {{1, 1, {1, 2, 3, 4}}};
		const auto prior = output;
		Diagnostic diagnostic;
		// Each row is below 16 Mi work. Two rows exceed it with one or two four-read sampling streams.
		const auto status = EvaluateArray(restored, plan, "image", request, output, diagnostic);
		INFO(samplingPath << ':' << diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		CHECK(status == Status::LimitExceeded);
		CHECK(diagnostic.NodeId == "text");
		CHECK(diagnostic.Message.find("whole processor batch") != std::string::npos);
		CHECK(output.Images == prior.Images);
		CHECK(output.Items.empty());
	}
}

TEST_CASE(
	"fresh held-playing Text uses its per-instance seed without a font path",
	"[source_font_text][font_initial_text]"
) {
	auto document = TextGraph("A");
	document.Nodes = {document.Nodes.back()};
	document.Links.clear();
	FontValue font;
	auto &data = font.Data.emplace();
	data.LineHeight = 1;
	data.Frames = {{1, 1, {0, 0, 255, 255}}};
	FontGlyph glyph;
	glyph.Character = 'A';
	glyph.Frame = 0;
	glyph.Advance = glyph.Width = glyph.Height = 1;
	data.Glyphs = {glyph};
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.Playing = true;
	context.InitialTextFonts = {{"text", font, std::nullopt}};
	EvaluationRequest request;
	request.SourceFonts = &context;
	auto output = TextImage(TextRestore(document), request);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	const auto previousOutput = output;
	context.Playing = false;
	TextSet(document, "font", std::string{"/held/font.bdf"});
	const auto plan = TextPlan(document);
	Diagnostic diagnostic;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == previousOutput);
}
