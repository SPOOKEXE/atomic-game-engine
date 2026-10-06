#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>

TEST_SUITE_ID("engine.imagegraphfont.wrapped_font_batch")
using namespace engine::imagegraph;
using namespace engine::imagegraphfont;
namespace {
	constexpr std::string_view BitmapFont = R"(STARTFONT 2.1
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
	struct FontFiles {
		std::filesystem::path Directory, Primary, Fallback;
		FontFiles() {
			Directory = std::filesystem::temp_directory_path() /
						("engine-wrapped-font-" +
						 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			REQUIRE(std::filesystem::create_directory(Directory));
			Primary = Directory / "primary.bdf";
			Fallback = Directory / "fallback.bdf";
			Store(Primary, std::string{BitmapFont});
			auto fallback = std::string{BitmapFont};
			fallback.replace(fallback.find("ENCODING 65"), 11, "ENCODING 66");
			Store(Fallback, fallback);
		}
		static void Store(const std::filesystem::path &path, const std::string &bytes) {
			std::ofstream stream(path, std::ios::binary);
			stream.write(bytes.data(), bytes.size());
			stream.close();
			REQUIRE(stream.good());
		}
		~FontFiles() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	};
	Document TextGraph(const FontFiles &files, std::string text, double width) {
		Document graph;
		graph.FormatVersion = 9;
		graph.Nodes = {
			{"text",
			 "pc.text",
			 "",
			 {},
			 {{"text", std::move(text)},
			  {"font", files.Primary.string()},
			  {"size", int64_t{10}},
			  {"interpolate", EnumValue{1}},
			  {"use_full_text_size", true},
			  {"max_line_width", width}}}
		};
		graph.Outputs = {{"out", "text", "surface_out"}};
		return graph;
	}
	void Set(Document &graph, const char *port, Value value) {
		auto &values = graph.Nodes[0].Values;
		for (auto &entry : values)
			if (entry.Port == port) {
				entry.Data = std::move(value);
				return;
			}
		values.push_back({port, std::move(value)});
	}
	Plan Persist(Document &graph) {
		const auto text = Write(graph);
		REQUIRE_FALSE(text.empty());
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
		REQUIRE(restored == graph);
		graph = std::move(restored);
		Plan plan;
		REQUIRE(Compile(graph, plan, diagnostic) == Status::Ok);
		return plan;
	}
	struct Recorder final : SourceFontProvider {
		SourceFontProvider &Provider;
		std::vector<SourceFontObservation> Records;
		explicit Recorder(SourceFontProvider &provider) : Provider(provider) {}
		uint64_t RetainedBytes() const override {
			uint64_t bytes = Provider.RetainedBytes();
			if (sizeof(*this) > UINT64_MAX - bytes) return UINT64_MAX;
			bytes += sizeof(*this);
			const auto add = [&](uint64_t value) {
				if (value > UINT64_MAX - bytes) return false;
				bytes += value;
				return true;
			};
			if (Records.capacity() > UINT64_MAX / sizeof(SourceFontObservation) ||
				!add(Records.capacity() * sizeof(SourceFontObservation)))
				return UINT64_MAX;
			for (const auto &record : Records) {
				const auto retained = SourceFontObservationRetainedBytes(record);
				if (!retained || !add(*retained)) return UINT64_MAX;
			}
			return bytes;
		}
		bool Observe(
			const SourceFontRequest &request,
			uint64_t cap,
			SourceFontObservation &output,
			std::string &failure
		) override {
			const uint64_t held = RetainedBytes();
			if (held > cap) return false;
			// grug leave half remaining room for the test recording copy and metadata growth.
			if (!Provider.Observe(request, (cap - held) / 2, output, failure)) return false;
			const auto retained = SourceFontObservationRetainedBytes(output);
			const uint64_t live = RetainedBytes();
			const uint64_t metadata = (Records.size() + 1) * sizeof(SourceFontObservation);
			if (!retained || live > cap || metadata > cap - live || *retained > cap - live - metadata)
				return false;
			Records.reserve(Records.size() + 1);
			Records.push_back(output);
			return true;
		}
	};
	struct Forbidden final : SourceFontProvider {
		size_t Calls = 0;
		bool
		Observe(const SourceFontRequest &, uint64_t, SourceFontObservation &, std::string &failure) override {
			++Calls;
			failure = "unexpected provider call";
			return false;
		}
	};
	void Bind(
		GraphFontInputs &owner,
		const FontFiles &files,
		SourceFontContext &held,
		EvaluationRequest &request,
		bool fallback = false
	) {
		GraphFontConfiguration configuration;
		configuration.Context.AliasMapKnown = true;
		configuration.Context.Playing = false;
		configuration.Context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
		configuration.Context.BitmapTextureProfile = FontBitmapTextureProfile::NativeFrameUv;
		configuration.ReadGrants = {{"text", files.Primary, false, "font"}};
		if (fallback) configuration.ReadGrants.push_back({"text", files.Fallback, false, "fallback_font"});
		Diagnostic diagnostic;
		REQUIRE(owner.Replace(configuration, {}, Limits::MaximumEvaluationBytes, diagnostic));
		REQUIRE(owner.Bind(false, held, request, Limits::MaximumEvaluationBytes, diagnostic));
	}

}

TEST_CASE("positive all spaces native sizing preserves source substring endpoint swap") {
	FontFiles files;
	auto graph = TextGraph(files, "    ", 4);
	const auto plan = Persist(graph);
	GraphFontInputs owner;
	SourceFontContext held;
	EvaluationRequest request;
	Bind(owner, files, held, request);
	Recorder recorder(*request.FontProvider);
	request.FontProvider = &recorder;
	Image output;
	Diagnostic diagnostic;
	const auto status = Evaluate(graph, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(recorder.Records.size() == 1);
	// At the third start, trailing-space removal crosses start. JS substring swaps1/3.
	// The only emitted slice therefore has two real spaces:2*4wide and one10pixel line.
	const auto &record = recorder.Records[0];
	REQUIRE(record.Request.Measurements.size() == 1);
	CHECK(record.Request.Measurements[0].Text == "    ");
	CHECK(record.Request.Measurements[0].MaximumLineWidth == 4);
	CHECK(record.Request.Measurements[0].LineGap == -1);
	REQUIRE(record.Font);
	REQUIRE(record.Font->Data);
	REQUIRE(record.Font->Data->Measurements.size() == 1);
	CHECK(record.Font->Data->Measurements[0].Width == 8);
	CHECK(record.Font->Data->Measurements[0].Height == 10);
	CHECK(output.Width == 8);
	CHECK(output.Height == 10);
	CHECK(output.Pixels == std::vector<uint8_t>(8 * 10 * 4, 0));
}

TEST_CASE("persisted native bitmap Font feeds owned full text sizing without a file provider") {
	Document graph;
	graph.FormatVersion = 9;
	graph.Nodes = {
		{"source", "image.captured", "", {}, {{"source_id", std::string{"glyph"}}}},
		{"bitmap", "pc.font_bitmap", "", {}, {{"string_map", std::string{"A"}}, {"separation", 0.0}}},
		{"text",
		 "pc.text",
		 "",
		 {},
		 {{"text", std::string{"AA"}},
		  {"interpolate", EnumValue{1}},
		  {"oversample", EnumValue{3}},
		  {"use_full_text_size", true},
		  {"max_line_width", 16.75}}}
	};
	graph.Links = {{"source", "image", "bitmap", "font_surfaces"}, {"bitmap", "font", "text", "font"}};
	graph.Outputs = {{"out", "text", "surface_out"}};
	const auto plan = Persist(graph);
	Image glyph{8, 10, std::vector<uint8_t>(8 * 10 * 4, 255)};
	glyph.Hash = SurfaceHash(glyph);
	const std::array images{RequestImageSource{"glyph", glyph}};
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.Playing = false;
	EvaluationRequest request;
	request.SourceFonts = &context;
	request.ImageSources = images;
	Forbidden forbidden;
	request.FontProvider = &forbidden;
	Image output;
	Diagnostic diagnostic;
	const auto status = Evaluate(graph, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	// The integer socket normalizes16.75 to17; two represented8pixel advances form one16pixel line.
	CHECK(output.Width == 16);
	CHECK(output.Height == 10);
	CHECK(forbidden.Calls == 0);
	CHECK(std::any_of(output.Pixels.begin(), output.Pixels.end(), [](uint8_t byte) { return byte == 255; }));
	const auto prior = output;
	CHECK(Evaluate(graph, plan, "out", request, output, diagnostic, 1) == Status::LimitExceeded);
	CHECK(output == prior);
	REQUIRE(Evaluate(graph, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output == prior);
	CHECK(forbidden.Calls == 0);
}

TEST_CASE(
	"two individually admitted native measurement rows reject aggregate work before provider and output "
	"publication"
) {
	FontFiles files;
	auto graph = TextGraph(files, "", 4);
	Set(graph, "dimension", EnumValue{0});
	Set(graph, "fixed_dimension", Vector2{16, 16});
	Node strings{"strings", "pc.array", "", {}, {{"type", EnumValue{4}}}};
	strings.DynamicInputs = {
		{"input_0", ValueType::Text, std::string(2100, ' ')},
		{"input_1", ValueType::Text, std::string(2100, ' ')}
	};
	graph.Nodes.insert(graph.Nodes.begin(), strings);
	graph.Links.push_back({"strings", "array", "text", "text"});
	auto plan = Persist(graph);
	GraphFontInputs owner;
	SourceFontContext held;
	EvaluationRequest request;
	Bind(owner, files, held, request);
	Recorder recorder(*request.FontProvider);
	request.FontProvider = &recorder;
	Diagnostic diagnostic;
	// The declared quote for either row is8,887,232; together17,774,464 exceeds16,777,216.
	// Fixed16x16 canvases keep this exclusively a measurement-work refusal.
	ImageArray output;
	output.Images = {{1, 1, {11, 12, 13, 14}}};
	const auto prior = output;
	const auto status = EvaluateArray(graph, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "text");
	CHECK(recorder.Records.empty());
	CHECK(output.Images == prior.Images);
	CHECK(output.Items == prior.Items);
	// Prove each long row is independently admissible under the same real provider and canvas.
	for (size_t row = 0; row < 2; ++row) {
		auto single = TextGraph(files, std::get<std::string>(*strings.DynamicInputs[row].Default), 4);
		Set(single, "dimension", EnumValue{0});
		Set(single, "fixed_dimension", Vector2{16, 16});
		const auto singlePlan = Persist(single);
		Image image;
		REQUIRE(Evaluate(single, singlePlan, "out", request, image, diagnostic) == Status::Ok);
		CHECK(image.Width == 16);
		CHECK(image.Height == 16);
	}
	// Lower both source strings below the same aggregate quote and retry the preserved result.
	for (auto &input : graph.Nodes[0].DynamicInputs)
		input.Default = std::string(100, ' ');
	plan = Persist(graph);
	recorder.Records.clear();
	REQUIRE(EvaluateArray(graph, plan, "out", request, output, diagnostic) == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(recorder.Records.size() == 2);
	for (const auto &image : output.Images) {
		CHECK(image.Width == 16);
		CHECK(image.Height == 16);
		CHECK(image.Pixels == std::vector<uint8_t>(16 * 16 * 4, 0));
	}
	CHECK(prior.Images[0].Pixels == std::vector<uint8_t>{11, 12, 13, 14});
}
