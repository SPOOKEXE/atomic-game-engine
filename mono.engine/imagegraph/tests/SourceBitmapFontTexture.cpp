#include "FontPayload.hpp"

#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.source_bitmap_font_texture")
using namespace engine::imagegraph;
namespace {
	SourceFontContext BitmapNamespace() {
		SourceFontContext context;
		context.AliasMapKnown = true;
		context.DefaultFontPath = std::string{};
		context.Playing = false;
		return context;
	}
	Document BitmapTextureGraph(std::string text = "A") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"glyphs", "image.captured", "", {}, {{"source_id", std::string{"glyphs"}}}},
			{"texture", "image.captured", "", {}, {{"source_id", std::string{"texture"}}}},
			{"bitmap", "pc.font_bitmap", "", {}, {{"string_map", std::string{"A"}}, {"separation", 0.0}}},
			{"text",
			 "pc.text",
			 "",
			 {},
			 {{"text", std::move(text)},
			  {"size", int64_t{16}},
			  {"anti_aliasing", false},
			  {"interpolate", EnumValue{1}},
			  {"oversample", EnumValue{3}}}}
		};
		document.Links = {
			{"glyphs", "image", "bitmap", "font_surfaces"},
			{"bitmap", "font", "text", "font"},
			{"texture", "image", "text", "texture"}
		};
		document.Outputs = {{"font", "bitmap", "font"}, {"image", "text", "surface_out"}};
		return document;
	}
	Plan TexturePlan(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Document TextureRestore(const Document &document) {
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		return restored;
	}
	std::array<RequestImageSource, 2> BitmapImages() {
		Image glyph{1, 1, {255, 0, 0, 128}};
		glyph.Hash = SurfaceHash(glyph);
		Image texture{1, 1, {255, 255, 255, 255}};
		texture.Hash = SurfaceHash(texture);
		return {RequestImageSource{"glyphs", glyph}, RequestImageSource{"texture", texture}};
	}
	SourceFontObservation
	BitmapRecord(const Document &document, const Plan &plan, const EvaluationRequest &request) {
		EvaluatedValue font;
		Diagnostic diagnostic;
		const auto status = EvaluateValue(document, plan, "font", request, font, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto *owned = std::get_if<FontValue>(&font.Data);
		REQUIRE(owned);
		SourceFontObservation record;
		record.Request.Authored = document.Nodes.back();
		record.Request.Context = *request.SourceFonts;
		record.Request.Role = "bitmap_texture";
		record.Request.PixelSize = 16;
		record.Request.FontInput = *owned;
		record.Request.Characters = {'A'};
		record.Font = *owned;
		record.Font->Data->SourceTexture = record.Font->Data->Frames[0];
		record.Font->Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
		REQUIRE(SourceFontObservationRetainedBytes(record));
		return record;
	}
	struct ForbiddenProvider final : SourceFontProvider {
		size_t Calls = 0;
		bool
		Observe(const SourceFontRequest &, uint64_t, SourceFontObservation &, std::string &failure) override {
			++Calls;
			failure = "provider must not handle bitmap atlas observations";
			return false;
		}
	};
}
TEST_CASE(
	"persisted Bitmap Font to textured Text consumes only an identity-bound owned atlas record",
	"[source_bitmap_font_texture]"
) {
	const auto document = TextureRestore(BitmapTextureGraph());
	const auto plan = TexturePlan(document);
	const auto images = BitmapImages();
	const auto context = BitmapNamespace();
	EvaluationRequest request;
	request.ImageSources = images;
	request.SourceFonts = &context;
	auto record = BitmapRecord(document, plan, request);
	request.FontObservations = {&record, 1};
	ForbiddenProvider provider;
	request.FontProvider = &provider;
	Image output;
	Diagnostic diagnostic;
	const auto status = Evaluate(document, plan, "image", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 0, 0, 128});
	CHECK(provider.Calls == 0);
	const auto original = *record.Request.FontInput;
	auto stale = record;
	stale.Request.FontInput->Data->Frames[0].Pixels[0] = 17;
	request.FontObservations = {&stale, 1};
	const auto before = output;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(output == before);
	CHECK(*record.Request.FontInput == original);
}
TEST_CASE(
	"bitmap texture records preserve metrics frames and preexisting atlas facts atomically",
	"[source_bitmap_font_texture]"
) {
	const auto document = BitmapTextureGraph();
	const auto plan = TexturePlan(document);
	const auto images = BitmapImages();
	const auto context = BitmapNamespace();
	EvaluationRequest request;
	request.ImageSources = images;
	request.SourceFonts = &context;
	const auto record = BitmapRecord(document, plan, request);
	for (size_t mutation = 0; mutation < 7; ++mutation) {
		auto bad = record;
		switch (mutation) {
		case 0:
			bad.Font->Data->Glyphs[0].Advance += 1;
			break;
		case 1:
			bad.Font->Data->Frames[0].Pixels[0] = 19;
			break;
		case 2:
			bad.Font->Data->LineHeight += 1;
			break;
		case 3:
			bad.Font->Data->Identity = "invented";
			break;
		case 4:
			bad.Font->Data->Glyphs[0].TextureRectangle.reset();
			break;
		case 5:
			bad.Request.ResolvedPath = "/file.ttf";
			break;
		case 6:
			bad.Presence = SourceFontPresence::AbsentFile;
			bad.Font.reset();
			break;
		}
		CHECK_FALSE(SourceFontObservationRetainedBytes(bad));
		request.FontObservations = {&bad, 1};
		Image output{1, 1, {7, 8, 9, 10}};
		const auto prior = output;
		Diagnostic diagnostic;
		CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
		CHECK(output == prior);
	}
	auto present = record;
	present.Request.FontInput->Data->SourceTexture = present.Font->Data->SourceTexture;
	present.Request.FontInput->Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
	REQUIRE(SourceFontObservationRetainedBytes(present));
	present.Font->Data->SourceTexture->Pixels[0] = 31;
	CHECK_FALSE(SourceFontObservationRetainedBytes(present));
}
TEST_CASE(
	"missing or duplicated bitmap atlas observations never fall back to a file provider",
	"[source_bitmap_font_texture]"
) {
	const auto document = BitmapTextureGraph();
	const auto plan = TexturePlan(document);
	const auto images = BitmapImages();
	const auto context = BitmapNamespace();
	EvaluationRequest request;
	request.ImageSources = images;
	request.SourceFonts = &context;
	ForbiddenProvider provider;
	request.FontProvider = &provider;
	Image output{1, 1, {1, 2, 3, 4}};
	const auto prior = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "text");
	CHECK(diagnostic.Port == "bitmap_texture");
	CHECK(output == prior);
	CHECK(provider.Calls == 0);
	const auto record = BitmapRecord(document, plan, request);
	const std::array duplicate{record, record};
	request.FontObservations = duplicate;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::DuplicateId);
	CHECK(output == prior);
	CHECK(provider.Calls == 0);
	request.FontObservations = {&record, 1};
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic, 1) == Status::LimitExceeded);
	CHECK(output == prior);
	CHECK(provider.Calls == 0);
}
TEST_CASE(
	"text replay owns the augmented bitmap Atlas and empty frames retain its original pixels",
	"[source_bitmap_font_texture]"
) {
	auto document = BitmapTextureGraph();
	document.Nodes.back().Values.push_back({"atlas", true});
	document.Outputs.push_back({"atlas", "text", "draw_data"});
	const auto plan = TexturePlan(document);
	auto images = BitmapImages();
	const auto context = BitmapNamespace();
	EvaluationRequest request;
	request.ImageSources = images;
	request.SourceFonts = &context;
	auto record = BitmapRecord(document, plan, request);
	request.FontObservations = {&record, 1};
	StatefulEvaluationResult result;
	Diagnostic diagnostic;
	const auto status = EvaluateStateful(document, plan, "atlas", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto original = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Output).Data);
	REQUIRE(original.Elements.size() == 1);
	record.Font->Data->SourceTexture->Pixels.assign(4, 0);
	images[0].Data.Pixels.assign(4, 0);
	for (auto &value : document.Nodes.back().Values)
		if (value.Port == "text") value.Data = std::string{};
	request.Tick = 1;
	request.FontObservations = {};
	request.DataReplay = &result.Data;
	StatefulEvaluationResult next;
	REQUIRE(
		EvaluateStateful(document, TexturePlan(document), "atlas", request, next, diagnostic) == Status::Ok
	);
	CHECK(std::get<ArrayValue>(std::get<EvaluatedValue>(next.Output).Data) == original);
	const auto &atlas = std::get<AtlasValue>(original.Elements[0]);
	REQUIRE(atlas.Data);
	REQUIRE(atlas.Data->OriginalSurface);
	CHECK(atlas.Data->OriginalSurface->Data.Pixels == std::vector<uint8_t>{128, 0, 0, 128});
}
TEST_CASE(
	"bitmap Text explicitly distinguishes missing font namespace from known native empty aliases",
	"[source_bitmap_font_texture]"
) {
	auto document = BitmapTextureGraph("");
	const auto plan = TexturePlan(document);
	const auto images = BitmapImages();
	EvaluationRequest request;
	request.ImageSources = images;
	Image output{1, 1, {9, 8, 7, 6}};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == before);
	const auto context = BitmapNamespace();
	request.SourceFonts = &context;
	REQUIRE(Evaluate(document, plan, "image", request, output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>(4, 0));
}

TEST_CASE(
	"native bitmap texture modulation uses owned frame UV without atlas discovery",
	"[source_bitmap_font_texture][font_native_profile]"
) {
	const auto document = TextureRestore(BitmapTextureGraph());
	const auto plan = TexturePlan(document);
	auto images = BitmapImages();
	images[1].Data.Pixels = {64, 128, 255, 255};
	images[1].Data.Hash = SurfaceHash(images[1].Data);
	auto context = BitmapNamespace();
	context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
	EvaluationRequest request;
	request.ImageSources = images;
	request.SourceFonts = &context;
	ForbiddenProvider provider;
	request.FontProvider = &provider;
	Image output;
	Diagnostic diagnostic;
	REQUIRE(Evaluate(document, plan, "image", request, output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{32, 0, 0, 128});
	CHECK(provider.Calls == 0);
	auto debug = document;
	debug.Nodes.back().Values.push_back({"attribute_debug_texture", true});
	const auto before = output;
	CHECK(
		Evaluate(debug, TexturePlan(debug), "image", request, output, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.Port == "bitmap_texture");
	CHECK(output == before);
	CHECK(provider.Calls == 0);
}
TEST_CASE(
	"native Unicode text expansion selects real bitmap glyphs after graph persistence",
	"[source_bitmap_font_texture][font_native_case]"
) {
	auto document = BitmapTextureGraph("ß");
	document.Nodes[2].Values[0].Data = std::string{"ßS"};
	document.Nodes.back().Values.push_back({"change_case", EnumValue{2}});
	Node frames{"frames", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	frames.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	document.Nodes.push_back(std::move(frames));
	document.Nodes.push_back({"second", "image.captured", "", {}, {{"source_id", std::string{"second"}}}});
	document.Links[0] = {"frames", "array", "bitmap", "font_surfaces"};
	document.Links.push_back({"glyphs", "image", "frames", "input_0"});
	document.Links.push_back({"second", "image", "frames", "input_1"});
	document = TextureRestore(document);
	const auto plan = TexturePlan(document);
	const auto first = BitmapImages();
	Image second{1, 1, {0, 0, 255, 255}};
	second.Hash = SurfaceHash(second);
	std::array images{first[0], first[1], RequestImageSource{"second", second}};
	auto context = BitmapNamespace();
	context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
	context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
	EvaluationRequest request;
	request.SourceFonts = &context;
	request.ImageSources = images;
	Image output;
	Diagnostic diagnostic;
	const auto status = Evaluate(document, plan, "image", request, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 2);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 255, 255, 0, 0, 255, 255});
}
