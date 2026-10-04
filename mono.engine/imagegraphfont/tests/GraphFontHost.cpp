#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphfont/GraphFontHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>

TEST_SUITE_ID("engine.imagegraphfont.graphfont_host")
using namespace engine::imagegraph;
namespace {
	constexpr std::string_view BITMAP_FONT = R"(STARTFONT 2.1
FONT -engine-test-medium-r-normal--10-100-72-72-c-80-iso10646-1
SIZE 10 72 72
FONTBOUNDINGBOX 5 7 -1 -2
STARTPROPERTIES 4
FONT_ASCENT 8
FONT_DESCENT 2
CHARSET_REGISTRY "ISO10646"
CHARSET_ENCODING "1"
ENDPROPERTIES
CHARS 2
STARTCHAR A
ENCODING 65
SWIDTH 800 0
DWIDTH 8 0
BBX 5 7 -1 -2
BITMAP
70
88
88
F8
88
88
88
ENDCHAR
STARTCHAR space
ENCODING 32
SWIDTH 400 0
DWIDTH 4 0
BBX 0 0 0 0
BITMAP
ENDCHAR
ENDFONT
)";
	struct FontFile {
		std::filesystem::path Directory, Path;
		FontFile() {
			Directory = std::filesystem::temp_directory_path() /
						("engine-font-host-" +
						 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			REQUIRE(std::filesystem::create_directory(Directory));
			Path = Directory / "font.bdf";
			std::ofstream file(Path, std::ios::binary);
			file.write(BITMAP_FONT.data(), BITMAP_FONT.size());
			file.close();
			REQUIRE(file.good());
		}
		~FontFile() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	};
	SourceFontRequest FontRequest(const FontFile &file) {
		SourceFontRequest request;
		request.Authored = {"text", "pc.text", "", {}, {{"font", file.Path.string()}, {"size", int64_t{10}}}};
		request.Context.AliasMapKnown = true;
		request.Context.Playing = false;
		request.ResolvedPath = file.Path.string();
		request.Role = "font";
		request.PixelSize = 10;
		request.Characters = {32, 65, 66};
		return request;
	}
	struct FontRecorder final : SourceFontProvider {
		SourceFontProvider &Provider;
		std::vector<SourceFontObservation> Records;
		explicit FontRecorder(SourceFontProvider &provider) : Provider(provider) {}
		bool Observe(
			const SourceFontRequest &request,
			uint64_t cap,
			SourceFontObservation &output,
			std::string &failure
		) override {
			if (!Provider.Observe(request, cap, output, failure)) return false;
			Records.push_back(output);
			return true;
		}
	};

}
TEST_CASE(
	"exact granted BDF Font host returns real bearings advances whitespace and missing glyph",
	"[graph_font_host]"
) {
	FontFile file;
	auto request = FontRequest(file);
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontObservation output;
	std::string failure;
	REQUIRE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output.Request == request);
	CHECK(output.Presence == SourceFontPresence::Present);
	REQUIRE(output.Font);
	REQUIRE(output.Font->Data);
	const auto &font = *output.Font->Data;
	CHECK(font.Raster == FontRasterProfile::NativeGlyphCoverage);
	CHECK_FALSE(font.GlyphMapComplete);
	REQUIRE(font.Glyphs.size() == 3);
	CHECK(font.Glyphs[0].Advance == 4);
	CHECK_FALSE(font.Glyphs[0].Frame);
	CHECK(font.Glyphs[1].Advance == 8);
	CHECK(font.Glyphs[1].Offset == Vector2{-1, 3});
	CHECK_FALSE(font.Glyphs[2].Present);
	CHECK_FALSE(font.Glyphs[2].Frame);
	CHECK(font.Glyphs[2].Advance == 0);
	REQUIRE(font.Frames.size() == 1);
	CHECK(font.Frames[0].Width == 5);
	CHECK(font.Frames[0].Height == 7);
	CHECK(font.Frames[0].Pixels[3] == 0);
	CHECK(font.Frames[0].Pixels[7] == 255);
	CHECK(font.Frames[0].Pixels[11] == 255);
	CHECK(SourceFontObservationRetainedBytes(output));
	CHECK_FALSE(font.SourceTexture);
}
TEST_CASE(
	"Font host refuses stale paths roles write grants and byte caps without replacing prior observation",
	"[graph_font_host]"
) {
	FontFile file;
	auto request = FontRequest(file);
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontObservation output;
	std::string failure;
	REQUIRE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	const auto prior = output;
	request.Role = "fallback_font";
	CHECK_FALSE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output == prior);
	request = FontRequest(file);
	request.ResolvedPath += ".other";
	CHECK_FALSE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output == prior);
	request = FontRequest(file);
	request.SignedDistanceField = true;
	CHECK_FALSE(host.Observe(request, 1, output, failure));
	CHECK(output == prior);
	request = FontRequest(file);
	CHECK_FALSE(host.Observe(request, 1, output, failure));
	CHECK(output == prior);
	grants[0].Write = true;
	engine::imagegraphfont::GraphFontHost writer(grants, {});
	CHECK_FALSE(writer.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output == prior);
}
TEST_CASE(
	"Font host observed missing file differs from malformed file and duplicate grant", "[graph_font_host]"
) {
	FontFile file;
	auto request = FontRequest(file);
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontObservation output;
	std::string failure;
	REQUIRE(std::filesystem::remove(file.Path));
	REQUIRE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output.Presence == SourceFontPresence::AbsentFile);
	CHECK_FALSE(output.Font);
	const auto absent = output;
	{
		std::ofstream bad(file.Path, std::ios::binary);
		bad << "not a font";
	}
	CHECK_FALSE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output == absent);
	std::array duplicates{grants[0], grants[0]};
	engine::imagegraphfont::GraphFontHost duplicate(duplicates, {});
	CHECK_FALSE(duplicate.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output == absent);
}
TEST_CASE(
	"real granted file font Text graph renders native coverage and file texture modulation",
	"[graph_font_host]"
) {
	FontFile file;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"text",
		 "pc.text",
		 "",
		 {},
		 {{"text", std::string{"A"}},
		  {"font", file.Path.string()},
		  {"size", int64_t{10}},
		  {"interpolate", EnumValue{1}}}}
	};
	document.Outputs = {{"out", "text", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.Playing = false;
	EvaluationRequest request;
	request.SourceFonts = &context;
	request.FontProvider = &host;
	Image output;
	const auto status = Evaluate(document, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 8);
	CHECK(output.Height == 10);
	CHECK(output.Pixels[(3 * 8 + 0) * 4 + 3] == 255);
	CHECK(output.Pixels[(3 * 8 + 3) * 4 + 3] == 0);
	document.Nodes.insert(
		document.Nodes.begin(), {"texture", "image.captured", "", {}, {{"source_id", std::string{"tint"}}}}
	);
	document.Links = {{"texture", "image", "text", "texture"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image blue{1, 1, {0, 0, 255, 255}};
	blue.Hash = SurfaceHash(blue);
	std::array images{RequestImageSource{"tint", blue}};
	request.ImageSources = images;
	REQUIRE(Evaluate(document, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output.Pixels[(3 * 8 + 0) * 4 + 0] == 0);
	CHECK(output.Pixels[(3 * 8 + 0) * 4 + 1] == 0);
	CHECK(output.Pixels[(3 * 8 + 0) * 4 + 2] == 255);
	CHECK(output.Pixels[(3 * 8 + 0) * 4 + 3] == 255);
}

TEST_CASE(
	"Text retains granted font during playback and observed absent file without reopening ambient fonts",
	"[graph_font_host]"
) {
	FontFile file;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"text",
		 "pc.text",
		 "",
		 {},
		 {{"text", std::string{"A"}},
		  {"font", file.Path.string()},
		  {"size", int64_t{10}},
		  {"interpolate", EnumValue{1}}}}
	};
	document.Outputs = {{"out", "text", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.Playing = false;
	EvaluationRequest request;
	request.SourceFonts = &context;
	request.FontProvider = &host;
	StatefulEvaluationResult first;
	const auto status = EvaluateStateful(document, plan, "out", request, first, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(first.Data.Entries.size() == 1);
	CHECK(std::get<Image>(first.Output).Width == 8);
	context.Playing = true;
	request.FontProvider = nullptr;
	request.Tick = 1;
	request.DataReplay = &first.Data;
	document.Nodes[0].Values[0].Data = std::string{"AA"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult playing;
	REQUIRE(EvaluateStateful(document, plan, "out", request, playing, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(playing.Output).Width == 16);
	REQUIRE(std::filesystem::remove(file.Path));
	context.Playing = false;
	request.FontProvider = &host;
	request.Tick = 2;
	request.DataReplay = &playing.Data;
	StatefulEvaluationResult absent;
	REQUIRE(EvaluateStateful(document, plan, "out", request, absent, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(absent.Output) == std::get<Image>(playing.Output));
}

TEST_CASE(
	"real Font host receipts replay without provider and reject changed source context atomically",
	"[graph_font_host]"
) {
	FontFile file;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"text",
		 "pc.text",
		 "",
		 {},
		 {{"text", std::string{"A"}},
		  {"font", file.Path.string()},
		  {"size", int64_t{10}},
		  {"interpolate", EnumValue{1}}}}
	};
	document.Outputs = {{"out", "text", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	FontRecorder recorder(host);
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.Playing = false;
	EvaluationRequest request;
	request.SourceFonts = &context;
	request.FontProvider = &recorder;
	Image first;
	REQUIRE(Evaluate(document, plan, "out", request, first, diagnostic) == Status::Ok);
	REQUIRE(recorder.Records.size() == 1);
	request.FontProvider = nullptr;
	request.FontObservations = recorder.Records;
	Image replay;
	REQUIRE(Evaluate(document, plan, "out", request, replay, diagnostic) == Status::Ok);
	CHECK(replay == first);
	context.Directory = "changed-unused-directory";
	CHECK(Evaluate(document, plan, "out", request, replay, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "font");
	CHECK(replay == first);
}

TEST_CASE(
	"exact granted font host produces real padded distance glyphs and renders SDF text",
	"[graph_font_host][font_native_sdf]"
) {
	FontFile file;
	auto request = FontRequest(file);
	request.SignedDistanceField = true;
	std::array grants{engine::imagegraphfont::GraphFontFileGrant{"text", file.Path, false, "font"}};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontObservation result;
	std::string failure;
	REQUIRE(host.Observe(request, Limits::MaximumEvaluationBytes, result, failure));
	REQUIRE(result.Font);
	REQUIRE(result.Font->Data);
	const auto &font = *result.Font->Data;
	CHECK(font.Raster == FontRasterProfile::NativeSignedDistance);
	CHECK(font.DistanceSpread == 8);
	CHECK(font.LineHeight == 10);
	REQUIRE(font.Glyphs.size() == 3);
	CHECK(font.Glyphs[0].Advance == 4);
	CHECK_FALSE(font.Glyphs[0].Frame);
	CHECK(font.Glyphs[0].DistancePaddingPixels == 0);
	CHECK(font.Glyphs[1].Advance == 8);
	CHECK(font.Glyphs[1].Offset == Vector2{-9, -5});
	CHECK(font.Glyphs[1].DistancePaddingPixels == 8);
	REQUIRE(font.Glyphs[1].Frame);
	const auto &frame = font.Frames[*font.Glyphs[1].Frame];
	CHECK(frame.Width == 21);
	CHECK(frame.Height == 23);
	CHECK(frame.Pixels[3] < 128);
	CHECK(std::any_of(frame.Pixels.begin(), frame.Pixels.end(), [](uint8_t byte) {
		return byte > 0 && byte < 128;
	}));
	REQUIRE(SourceFontObservationRetainedBytes(result));
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"text",
		 "pc.text",
		 "",
		 {},
		 {{"text", std::string{"A"}},
		  {"font", file.Path.string()},
		  {"size", int64_t{10}},
		  {"use_sdf", true},
		  {"anti_aliasing", true},
		  {"interpolate", EnumValue{1}}}}
	};
	document.Outputs = {{"out", "text", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.DefaultFontPath = std::string{};
	context.Playing = false;
	EvaluationRequest evaluation;
	evaluation.SourceFonts = &context;
	evaluation.FontProvider = &host;
	Image output;
	const auto status = Evaluate(document, plan, "out", evaluation, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 8);
	CHECK(output.Height == 10);
	CHECK(std::any_of(output.Pixels.begin(), output.Pixels.end(), [](uint8_t byte) { return byte != 0; }));
}

TEST_CASE("font path changes select only the matching exact read grant", "[graphfont_host]") {
	FontFile first;
	FontFile second;
	const std::array<engine::imagegraphfont::GraphFontFileGrant, 2> grants{
		{{"text", first.Path, false, "font"}, {"text", second.Path, false, "font"}}
	};
	engine::imagegraphfont::GraphFontHost host(grants, {});
	SourceFontObservation output;
	std::string failure;
	REQUIRE(host.Observe(FontRequest(first), Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output.Request.ResolvedPath == first.Path.string());
	REQUIRE(host.Observe(FontRequest(second), Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output.Request.ResolvedPath == second.Path.string());
	CHECK(output.Font->Data->SpaceAdvance == 4);
	const auto before = output;
	auto absent = FontRequest(second);
	absent.ResolvedPath = (second.Directory / "ungranted.bdf").string();
	CHECK_FALSE(host.Observe(absent, Limits::MaximumEvaluationBytes, output, failure));
	CHECK(output == before);
}
