#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphfont.graphfont_configuration")
using namespace engine::imagegraph;
using namespace engine::imagegraphfont;
namespace {
	using Json = nlohmann::json;
	FontValue ArtifactFont() {
		FontValue value;
		auto &font = value.Data.emplace();
		font.Characters = FontCharacterProfile::UnicodeScalar;
		font.Raster = FontRasterProfile::NativeGlyphCoverage;
		font.GlyphMapComplete = false;
		font.Frames.push_back({1, 1, {11, 22, 33, 44}, 0xdeadbeef01234567});
		font.Glyphs.push_back({'A', true, 0, 2.25, 1, 1, {-0.5, 0.25}, Vector4{0, 0, 1, 1}});
		font.Measurements.push_back({"A", 20, 1, 2.25, 3});
		font.LineHeight = 3;
		font.MissingAdvance = 1;
		font.SpaceAdvance = 2;
		font.Identity = "observed-font";
		font.SourceTexture = font.Frames[0];
		font.HasCharacterRange = true;
		font.FirstCharacter = font.LastCharacter = 'A';
		return value;
	}
	GraphFontConfiguration Artifact() {
		GraphFontConfiguration config;
		auto &context = config.Context;
		context.AliasMapKnown = true;
		context.Aliases = {{"explicit", "/granted/font.ttf"}};
		context.Directory = "/working/";
		context.ApplicationLocation = "/app/";
		context.ProjectPath = "/project/";
		context.DefaultFontPath = "/granted/default.ttf";
		context.Playing = false;
		context.InitialFont = ArtifactFont();
		context.TextTransforms = {{"A", "a", 1}};
		context.InitialTextFonts = {{"text", ArtifactFont(), ArtifactFont()}};
		SourceFontObservation receipt;
		receipt.Request.Authored = {
			"text", "pc.text", "", {}, {{"text", std::string{"A"}}, {"offset", Vector2{1, 2}}}
		};
		receipt.Request.Authored.SourceDisplayName = "Text glyph";
		receipt.Request.Context = context;
		receipt.Request.ProcessorRow = 2;
		receipt.Request.Tick = 17;
		receipt.Request.Subframe = 0.5;
		receipt.Request.NegativeFrame = true;
		receipt.Request.Role = "font";
		receipt.Request.ResolvedPath = "/granted/font.ttf";
		receipt.Request.Characters = {'A'};
		receipt.Request.Measurements = {{"A", 20, 1, 0, 0}};
		receipt.Font = ArtifactFont();
		config.Observations.push_back(receipt);
		config.ReadGrants.push_back({"text", "/granted/font.ttf", false, "font"});
		return config;
	}
	std::string Encode(const GraphFontConfiguration &config) {
		std::string bytes;
		Diagnostic diagnostic;
		const bool written =
			WriteGraphFontConfiguration(config, bytes, Limits::MaximumEvaluationBytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		return bytes;
	}
	void Equal(const GraphFontConfiguration &left, const GraphFontConfiguration &right) {
		CHECK(left.Context == right.Context);
		CHECK(left.Observations == right.Observations);
		REQUIRE(left.ReadGrants.size() == right.ReadGrants.size());
		for (size_t i = 0; i < left.ReadGrants.size(); ++i) {
			CHECK(left.ReadGrants[i].NodeId == right.ReadGrants[i].NodeId);
			CHECK(left.ReadGrants[i].File == right.ReadGrants[i].File);
			CHECK(left.ReadGrants[i].Resource == right.ReadGrants[i].Resource);
			CHECK(left.ReadGrants[i].Write == right.ReadGrants[i].Write);
		}
	}
	void Refuse(const std::string &bytes) {
		auto destination = Artifact();
		const auto old = destination;
		Diagnostic diagnostic;
		REQUIRE_FALSE(
			ReadGraphFontConfiguration(bytes, destination, Limits::MaximumEvaluationBytes, diagnostic)
		);
		CHECK(diagnostic.Code != Status::Ok);
		Equal(destination, old);
	}
}
TEST_CASE(
	"font artifacts round trip complete owned requests and native authored singleton nodes",
	"[graph_font_configuration]"
) {
	auto config = Artifact();
	auto bytes = Encode(config);
	GraphFontConfiguration restored;
	Diagnostic diagnostic;
	REQUIRE(ReadGraphFontConfiguration(bytes, restored, Limits::MaximumEvaluationBytes, diagnostic));
	Equal(restored, config);
	const auto parsed = Json::parse(bytes);
	CHECK(parsed["schema"] == "atomic.font_inputs.v1");
	CHECK(parsed["observations"][0]["request"]["authored"].get<std::string>().starts_with("imagegraph 9\n"));
	CHECK(parsed["context"]["initialFont"]["frames"][0]["storedHash"] == 0xdeadbeef01234567);
	CHECK(parsed["context"]["initialFont"]["frames"][0]["pixelsHex"] == "0b16212c");
	restored.Context.InitialFont->Data->Frames[0].Pixels[0] = 99;
	restored.Observations[0].Request.Authored.Values[0].Port = "other";
	CHECK(config.Context.InitialFont->Data->Frames[0].Pixels[0] == 11);
	CHECK(config.Observations[0].Request.Authored.Values[0].Port == "text");
}
TEST_CASE(
	"font artifacts retain bitmap receipts native SDF and named profile choices", "[graph_font_configuration]"
) {
	auto config = Artifact();
	config.Context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
	config.Context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
	auto bitmap = ArtifactFont();
	bitmap.Data->Raster = FontRasterProfile::BitmapSurface;
	bitmap.Data->Characters = FontCharacterProfile::Utf16;
	bitmap.Data->GlyphMapComplete = true;
	bitmap.Data->SourceTexture.reset();
	bitmap.Data->Glyphs[0].TextureRectangle.reset();
	SourceFontObservation receipt;
	receipt.Request.Authored = {"bitmaptext", "pc.text", "", {}, {{"text", std::string{"A"}}}};
	receipt.Request.Context = config.Context;
	receipt.Request.Role = "bitmap_texture";
	receipt.Request.FontInput = bitmap;
	receipt.Request.Characters = {'A'};
	receipt.Font = bitmap;
	receipt.Font->Data->SourceTexture = bitmap.Data->Frames[0];
	receipt.Font->Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
	config.Observations.push_back(receipt);
	auto sdf = config.Observations[0];
	sdf.Request.Authored.Id = "sdftext";
	sdf.Request.SignedDistanceField = true;
	sdf.Font->Data->Raster = FontRasterProfile::NativeSignedDistance;
	sdf.Font->Data->DistanceSpread = 4;
	sdf.Font->Data->Glyphs[0].DistancePaddingPixels = 4;
	sdf.Font->Data->Frames[0].Pixels = {255, 255, 255, 44};
	REQUIRE(SourceFontValueRetainedBytes(*sdf.Font));
	config.Observations.push_back(sdf);
	auto absent = config.Observations[0];
	absent.Request.Authored.Id = "absenttext";
	absent.Request.Role = "fallback_font";
	absent.Presence = SourceFontPresence::AbsentFile;
	absent.Font.reset();
	config.Observations.push_back(absent);
	GraphFontConfiguration restored;
	Diagnostic diagnostic;
	REQUIRE(ReadGraphFontConfiguration(Encode(config), restored, Limits::MaximumEvaluationBytes, diagnostic));
	Equal(restored, config);
	CHECK(restored.Observations[1].Request.FontInput->Data->Frames[0].Hash == 0xdeadbeef01234567);
	CHECK(restored.Observations[2].Font->Data->Raster == FontRasterProfile::NativeSignedDistance);
	CHECK(restored.Observations[2].Font->Data->DistanceSpread == 4);
	CHECK(restored.Observations[2].Font->Data->Frames[0].Pixels == std::vector<uint8_t>{255, 255, 255, 44});
	CHECK(restored.Observations[2].Font->Data->Glyphs[0].DistancePaddingPixels == 4);
}
TEST_CASE(
	"font artifacts reject malformed JSON fields enums typed images and request identity atomically",
	"[graph_font_configuration]"
) {
	const auto valid = Json::parse(Encode(Artifact()));
	for (int mutation = 0; mutation < 16; ++mutation) {
		auto j = valid;
		switch (mutation) {
		case 0:
			j["unknown"] = 1;
			break;
		case 1:
			j["context"]["playing"] = 0;
			break;
		case 2:
			j["context"]["textCaseProfile"] = "guess";
			break;
		case 3:
			j["context"]["bitmapTextureProfile"] = "guess";
			break;
		case 4:
			j["readGrants"][0]["write"] = true;
			break;
		case 5:
			j["readGrants"].push_back(j["readGrants"][0]);
			break;
		case 6:
			j["observations"].push_back(j["observations"][0]);
			break;
		case 7:
			j["observations"][0]["request"]["pixelSize"] = -1;
			break;
		case 8:
			j["observations"][0]["request"]["processorRow"] = 0.5;
			break;
		case 9:
			j["observations"][0]["font"]["frames"][0]["format"] = "driver-rgba";
			break;
		case 10:
			j["observations"][0]["font"]["frames"][0]["pixelsHex"] = "0B16212c";
			break;
		case 11:
			j["observations"][0]["font"]["frames"][0]["pixelsHex"] = "0011";
			break;
		case 12:
			j["observations"][0]["font"]["glyphs"][0]["frame"] = 2;
			break;
		case 13:
			j["observations"][0]["font"]["raster"] = "native_signed_distance";
			break;
		case 14:
			j["observations"][0]["request"]["characters"].push_back(65);
			break;
		case 15:
			j["observations"][0]["presence"] = "missing";
			break;
		}
		INFO(mutation);
		Refuse(j.dump());
	}
	auto duplicate = valid.dump();
	duplicate.insert(1, "\"schema\":\"atomic.font_inputs.v1\",");
	Refuse(duplicate);
	Refuse("{\"a\":1e999}");
	Refuse(std::string("{\"a\":\"") + char(0xff) + "\"}");
	Refuse(std::string(65, '[') + std::string(65, ']'));
	auto extra = valid;
	extra["observations"][0]["request"]["authored"] =
		extra["observations"][0]["request"]["authored"].get<std::string>() +
		"output \"ignored\" \"text\" \"surface_out\"\n";
	Refuse(extra.dump());
}
TEST_CASE(
	"font artifact read and write quota failures preserve old owned destinations",
	"[graph_font_configuration]"
) {
	const auto config = Artifact();
	const auto encoded = Encode(config);
	auto destination = config;
	const auto old = destination;
	Diagnostic diagnostic;
	REQUIRE_FALSE(ReadGraphFontConfiguration(encoded, destination, encoded.size(), diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	Equal(destination, old);
	std::string output = "previous complete artifact";
	REQUIRE_FALSE(WriteGraphFontConfiguration(config, output, 1, diagnostic));
	CHECK(output == "previous complete artifact");
	CHECK(diagnostic.Code == Status::LimitExceeded);
	auto malformed = config;
	malformed.ReadGrants[0].Write = true;
	REQUIRE_FALSE(WriteGraphFontConfiguration(malformed, output, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(output == "previous complete artifact");
	auto reserved = config;
	reserved.Observations.reserve(Limits::MaximumNodes);
	REQUIRE_FALSE(WriteGraphFontConfiguration(reserved, output, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(output == "previous complete artifact");
}

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
						("engine-font-artifact-" +
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
}
TEST_CASE(
	"parsed font configuration binds held playback and permits only its exact read grant",
	"[graph_font_configuration]"
) {
	FontFile file;
	GraphFontConfiguration config;
	config.Context.AliasMapKnown = true;
	config.Context.DefaultFontPath = file.Path.string();
	config.Context.Playing = false;
	config.Context.Aliases = {{"fixture", file.Path.string()}};
	config.ReadGrants = {{"text", file.Path, false, "font"}};
	GraphFontConfiguration restored;
	Diagnostic diagnostic;
	REQUIRE(ReadGraphFontConfiguration(Encode(config), restored, Limits::MaximumEvaluationBytes, diagnostic));
	GraphFontInputs inputs;
	REQUIRE(inputs.Replace(restored, {}, Limits::MaximumEvaluationBytes, diagnostic));
	SourceFontContext held;
	EvaluationRequest evaluation;
	REQUIRE(inputs.Bind(true, held, evaluation, Limits::MaximumEvaluationBytes, diagnostic));
	REQUIRE(evaluation.SourceFonts == &held);
	CHECK(held.Playing == true);
	CHECK(inputs.Configuration().Context.Playing == false);
	REQUIRE(evaluation.FontProvider);
	SourceFontRequest request;
	request.Authored = {"text", "pc.text", "", {}, {{"font", std::string{"fixture"}}, {"size", int64_t{10}}}};
	request.Context = held;
	request.Role = "font";
	request.ResolvedPath = file.Path.string();
	request.PixelSize = 10;
	request.Characters = {65};
	SourceFontObservation observed;
	std::string failure;
	REQUIRE(evaluation.FontProvider->Observe(request, Limits::MaximumEvaluationBytes, observed, failure));
	REQUIRE(observed.Font);
	CHECK(observed.Font->Data->Raster == FontRasterProfile::NativeGlyphCoverage);
	REQUIRE(observed.Font->Data->Glyphs.size() == 1);
	CHECK(observed.Font->Data->Glyphs[0].Present);
	const auto retained = observed;
	request.Authored.Id = "ungranted";
	CHECK_FALSE(evaluation.FontProvider->Observe(request, Limits::MaximumEvaluationBytes, observed, failure));
	CHECK(observed == retained);
}

TEST_CASE(
	"font artifacts preserve all named numeric surface formats and exact byte hashes",
	"[graph_font_configuration]"
) {
	for (const auto format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		auto config = Artifact();
		Image image;
		image.Width = image.Height = 1;
		image.Format = format;
		image.Hash = 123456789;
		const auto layout = CheckedSurfaceLayout(1, 1, format, Limits::MaximumArrayBytes);
		REQUIRE(layout);
		image.Pixels.resize(layout->Bytes);
		REQUIRE(StoreSurfacePixel(image, 0, 0, {0.5, 0.25, 0.75, 1}));
		config.Context.InitialFont->Data->Frames[0] = image;
		config.Context.InitialFont->Data->SourceTexture = image;
		config.Observations[0].Request.Context = config.Context;
		config.Observations[0].Font->Data->Frames[0] = image;
		config.Observations[0].Font->Data->SourceTexture = image;
		config.Observations[0].Font->Data->Raster = FontRasterProfile::SourceObserved;
		GraphFontConfiguration restored;
		Diagnostic diagnostic;
		REQUIRE(
			ReadGraphFontConfiguration(Encode(config), restored, Limits::MaximumEvaluationBytes, diagnostic)
		);
		Equal(restored, config);
		CHECK(restored.Observations[0].Font->Data->Frames[0].Pixels == image.Pixels);
		CHECK(restored.Observations[0].Font->Data->Frames[0].Format == format);
		CHECK(restored.Observations[0].Font->Data->Frames[0].Hash == image.Hash);
	}
}
TEST_CASE(
	"font artifact budgets admit near four MiB bitmap input and complete atlas receipt",
	"[graph_font_configuration]"
) {
	GraphFontConfiguration config;
	config.Context.AliasMapKnown = true;
	config.Context.DefaultFontPath = std::string{};
	config.Context.Playing = false;
	auto bitmap = ArtifactFont();
	auto &font = *bitmap.Data;
	font.Raster = FontRasterProfile::BitmapSurface;
	font.Characters = FontCharacterProfile::Utf16;
	font.GlyphMapComplete = true;
	font.Measurements.clear();
	font.SourceTexture.reset();
	font.Glyphs[0].TextureRectangle.reset();
	font.Frames[0] = {4096, 1023, std::vector<uint8_t>(4096 * 1023, 128), 987654321, SurfaceFormat::R8Unorm};
	font.Glyphs[0].Width = 4096;
	font.Glyphs[0].Height = 1023;
	SourceFontObservation observation;
	observation.Request.Authored = {"text", "pc.text", "", {}, {{"text", std::string{"A"}}}};
	observation.Request.Context = config.Context;
	observation.Request.Role = "bitmap_texture";
	observation.Request.FontInput = bitmap;
	observation.Request.Characters = {'A'};
	observation.Font = bitmap;
	observation.Font->Data->SourceTexture = Image{1, 1, {128}, 555, SurfaceFormat::R8Unorm};
	observation.Font->Data->Glyphs[0].TextureRectangle = Vector4{0, 0, 1, 1};
	config.Observations.push_back(std::move(observation));
	REQUIRE(GraphFontConfigurationRetainedBytes(config));
	const auto bytes = Encode(config);
	GraphFontConfiguration restored;
	Diagnostic diagnostic;
	REQUIRE(ReadGraphFontConfiguration(bytes, restored, Limits::MaximumEvaluationBytes, diagnostic));
	Equal(restored, config);
	CHECK(restored.Observations[0].Request.FontInput->Data->Frames[0].Pixels.size() == 4096 * 1023);
}
TEST_CASE(
	"font artifact budgets admit a full native source text literal without structural inflation",
	"[graph_font_configuration]"
) {
	auto config = Artifact();
	config.Observations[0].Request.Authored.Values[0].Data = std::string(Limits::MaximumTextBytes, 'A');
	const auto bytes = Encode(config);
	GraphFontConfiguration restored;
	Diagnostic diagnostic;
	REQUIRE(ReadGraphFontConfiguration(bytes, restored, Limits::MaximumEvaluationBytes, diagnostic));
	Equal(restored, config);
	CHECK(
		std::get<std::string>(restored.Observations[0].Request.Authored.Values[0].Data).size() ==
		Limits::MaximumTextBytes
	);
}
TEST_CASE(
	"font artifact caller caps declared native counts before payload allocation", "[graph_font_configuration]"
) {
	auto malformed = Json::parse(Encode(Artifact()));
	malformed["observations"][0]["request"]["authored"] =
		"imagegraph 9\nnode \"text\" \"pc.text\" \"\" 0 0\nvalue 0 \"text\" \"color_by_letter\" a scalar "
		"4096\n";
	auto destination = Artifact();
	const auto old = destination;
	Diagnostic diagnostic;
	REQUIRE_FALSE(ReadGraphFontConfiguration(malformed.dump(), destination, 256 * 1024, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	Equal(destination, old);
}
TEST_CASE(
	"font artifact writers refuse nonfinite owned samples and malformed UTF8 atomically",
	"[graph_font_configuration]"
) {
	auto config = Artifact();
	std::string destination = "complete previous artifact";
	Diagnostic diagnostic;
	config.Observations[0].Font->Data->Glyphs[0].Advance = std::numeric_limits<double>::infinity();
	CHECK_FALSE(WriteGraphFontConfiguration(config, destination, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(destination == "complete previous artifact");
	config = Artifact();
	config.Observations[0].Font->Data->Identity = std::string(1, char(0xff));
	CHECK_FALSE(WriteGraphFontConfiguration(config, destination, Limits::MaximumEvaluationBytes, diagnostic));
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(destination == "complete previous artifact");
}

TEST_CASE(
	"font artifact seeded text state uses durable node identity and isolated owned fonts",
	"[graph_font_configuration]"
) {
	auto config = Artifact();
	GraphFontConfiguration restored;
	Diagnostic diagnostic;
	REQUIRE(ReadGraphFontConfiguration(Encode(config), restored, Limits::MaximumEvaluationBytes, diagnostic));
	REQUIRE(restored.Context.InitialTextFonts.size() == 1);
	CHECK(restored.Context.InitialTextFonts[0].NodeId == "text");
	restored.Context.InitialTextFonts[0].Primary->Data->Frames[0].Pixels[0] = 88;
	CHECK(config.Context.InitialTextFonts[0].Primary->Data->Frames[0].Pixels[0] == 11);
	CHECK(restored.Context.InitialTextFonts[0].Fallback->Data->Frames[0].Pixels[0] == 11);
	auto j = Json::parse(Encode(config));
	j["context"]["initialTextFonts"].push_back(j["context"]["initialTextFonts"][0]);
	Refuse(j.dump());
	j = Json::parse(Encode(config));
	j["context"]["initialTextFonts"][0]["nodeId"] = "";
	Refuse(j.dump());
	j = Json::parse(Encode(config));
	j["context"]["initialTextFonts"][0]["primary"]["glyphs"][0]["frame"] = 9;
	Refuse(j.dump());
}
