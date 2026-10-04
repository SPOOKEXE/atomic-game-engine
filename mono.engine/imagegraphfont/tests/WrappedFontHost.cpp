#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>

TEST_SUITE_ID("engine.imagegraphfont.wrapped_font_host")
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
	void CheckMeasurement(
		const SourceFontObservation &record, std::string_view raw, double wrap, double width, double height
	) {
		REQUIRE(record.Request.Measurements.size() == 1);
		const auto &requested = record.Request.Measurements[0];
		CHECK(requested.Text == raw);
		CHECK(requested.MaximumLineWidth == wrap);
		CHECK(requested.LineGap == -1);
		CHECK(requested.Width == 0);
		CHECK(requested.Height == 0);
		REQUIRE(record.Font);
		REQUIRE(record.Font->Data);
		REQUIRE(record.Font->Data->Measurements.size() == 1);
		const auto &measured = record.Font->Data->Measurements[0];
		CHECK(measured.Text == raw);
		CHECK(measured.MaximumLineWidth == wrap);
		CHECK(measured.LineGap == -1);
		CHECK(measured.Width == width);
		CHECK(measured.Height == height);
		CHECK(record.Request.Authored.Id == "text");
		CHECK(record.Request.Authored.Type == "pc.text");
		CHECK(record.Font->Data->LineHeight == 10);
		CHECK(record.Font->Data->Raster == FontRasterProfile::NativeGlyphCoverage);
		CHECK(record.Font->Data->Characters == FontCharacterProfile::UnicodeScalar);
		CHECK_FALSE(record.Font->Data->GlyphMapComplete);
		REQUIRE(record.Font->Data->Frames.size() == 1);
		const auto &frame = record.Font->Data->Frames[0];
		CHECK(frame.Width == 5);
		CHECK(frame.Height == 7);
		REQUIRE(frame.Pixels.size() == 140);
		CHECK(frame.Pixels[3] == 0);
		CHECK(frame.Pixels[7] == 255);
	}
}

TEST_CASE("persisted granted BDF full text sizing preserves integer socket normalization") {
	FontFiles files;
	struct Sample {
		const char *Text;
		double Wrap, ResolvedWrap, Width, Height;
	};
	// Hand-traced Split_TextBlock with literal A8, space4, missing/tab/CR0, line10.
	const std::array samples{
		Sample{"A A", 12, 12, 8, 20},
		Sample{"AA AA", 12.75, 13, 16, 20},
		Sample{"AA", 16, 16, 16, 10},
		Sample{"A\tA", 12, 12, 16, 10},
		Sample{"A\u00e9A", 12, 12, 16, 10},
		Sample{"A\U0001f600A", 12, 12, 16, 10},
		Sample{" A ", 12, 12, 12, 10},
		Sample{"A\nA", 12, 12, 8, 20},
		Sample{"A\rA", 12, 12, 16, 10},
		Sample{"A\r\nA", 12, 12, 8, 20},
		Sample{"\n\nA", 12, 12, 8, 30},
		Sample{"A\n", 12, 12, 8, 10},
		Sample{"A A", -12.75, -13, 20, 10},
		Sample{"A", .5, 0, 8, 10},
		Sample{"A", -.5, 0, 8, 10},
		Sample{"A", 7, 7, 0, 0}
	};
	for (const auto &sample : samples) {
		INFO(sample.Text);
		INFO(sample.Wrap);
		auto graph = TextGraph(files, sample.Text, sample.Wrap);
		const auto plan = Persist(graph);
		GraphFontInputs owner;
		SourceFontContext held;
		EvaluationRequest request;
		Bind(owner, files, held, request);
		request.Tick = 17;
		request.Subframe = .5;
		request.NegativeFrame = true;
		Recorder recorder(*request.FontProvider);
		request.FontProvider = &recorder;
		Image output;
		Diagnostic diagnostic;
		const auto status = Evaluate(graph, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(recorder.Records.size() == 1);
		const auto &record = recorder.Records[0];
		if (sample.ResolvedWrap != 0)
			CheckMeasurement(record, sample.Text, sample.ResolvedWrap, sample.Width, sample.Height);
		else {
			// The source integer socket rounds ties to zero, selecting unwrapped full-text sizing.
			CHECK(record.Request.Measurements.empty());
			REQUIRE(record.Font);
			CHECK(record.Font->Data->Measurements.empty());
		}
		CHECK(record.Request.ProcessorRow == 0);
		CHECK(record.Request.Tick == 17);
		CHECK(record.Request.Subframe == .5);
		CHECK(record.Request.NegativeFrame);
		CHECK(record.Request.Context == held);
		CHECK(record.Request.ResolvedPath == files.Primary.string());
		CHECK(record.Request.Role == "font");
		CHECK(output.Width == std::max(1.0, sample.Width));
		CHECK(output.Height == std::max(1.0, sample.Height));
		CHECK(output.Pixels.size() == output.Width * output.Height * 4);
	}
}

TEST_CASE("BDF full text receipt measures final case before trim and ignores drawing spacing controls") {
	FontFiles files;
	for (bool recorded : {false, true}) {
		auto graph = TextGraph(files, recorded ? "a\xC3\x9F" : "aa aa", 12.75);
		Set(graph, "change_case", EnumValue{2});
		Set(graph, "trim", true);
		Set(graph, "range", Vector2{0, .5});
		Set(graph, "letter_spacing", -1.0);
		Set(graph, "line_height", -2.0);
		Set(graph, "monospaced", true);
		const auto plan = Persist(graph);
		GraphFontInputs owner;
		SourceFontContext held;
		EvaluationRequest request;
		Bind(owner, files, held, request);
		if (recorded) held.TextTransforms = {{"a\xC3\x9F", "AA AA", 2}};
		Recorder recorder(*request.FontProvider);
		request.FontProvider = &recorder;
		Image output;
		Diagnostic diagnostic;
		const auto status = Evaluate(graph, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(recorder.Records.size() == 1);
		CheckMeasurement(recorder.Records[0], "AA AA", 13, 16, 20);
		CHECK(output.Width == 16);
		CHECK(output.Height == 20);
		CHECK(
			std::find(
				recorder.Records[0].Request.Characters.begin(),
				recorder.Records[0].Request.Characters.end(),
				uint32_t{'W'}
			) != recorder.Records[0].Request.Characters.end()
		);
	}
}

TEST_CASE("native Unicode uppercase missing glyphs retain zero advance in full size measurement") {
	FontFiles files;
	auto graph = TextGraph(files, "a\xC3\x9F", 12);
	Set(graph, "change_case", EnumValue{2});
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
	CheckMeasurement(recorder.Records[0], "ASS", 12, 8, 10);
	CHECK(output.Width == 8);
	CHECK(output.Height == 10);
}

TEST_CASE("full size requests reach both real granted roles before fallback selects represented glyphs") {
	FontFiles files;
	auto graph = TextGraph(files, "B B", 12);
	Set(graph, "fallback_font", files.Fallback.string());
	const auto plan = Persist(graph);
	GraphFontInputs owner;
	SourceFontContext held;
	EvaluationRequest request;
	Bind(owner, files, held, request, true);
	Recorder recorder(*request.FontProvider);
	request.FontProvider = &recorder;
	Image output;
	Diagnostic diagnostic;
	const auto status = Evaluate(graph, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(recorder.Records.size() == 2);
	CHECK(recorder.Records[0].Request.Role == "font");
	CHECK(recorder.Records[1].Request.Role == "fallback_font");
	CheckMeasurement(recorder.Records[1], "B B", 12, 8, 20);
	CHECK(output.Width == 8);
	CHECK(output.Height == 20);
	REQUIRE(recorder.Records[0].Request.Measurements.size() == 1);
	CHECK(recorder.Records[0].Request.Measurements[0].Text == "B B");
	CHECK(recorder.Records[1].Request.ResolvedPath == files.Fallback.string());
}

TEST_CASE("exact source measurement recording overrides native arithmetic without provider invocation") {
	FontFiles files;
	auto graph = TextGraph(files, "A A", 12);
	const auto plan = Persist(graph);
	GraphFontInputs owner;
	SourceFontContext held;
	EvaluationRequest request;
	Bind(owner, files, held, request);
	Recorder recorder(*request.FontProvider);
	request.FontProvider = &recorder;
	Image first;
	Diagnostic diagnostic;
	REQUIRE(Evaluate(graph, plan, "out", request, first, diagnostic) == Status::Ok);
	REQUIRE(recorder.Records.size() == 1);
	auto &font = *recorder.Records[0].Font->Data;
	font.Raster = FontRasterProfile::SourceObserved;
	font.Measurements[0].Width = 3;
	font.Measurements[0].Height = 7;
	REQUIRE(SourceFontObservationRetainedBytes(recorder.Records[0]));
	Forbidden forbidden;
	request.FontProvider = &forbidden;
	request.FontObservations = recorder.Records;
	Image exact;
	REQUIRE(Evaluate(graph, plan, "out", request, exact, diagnostic) == Status::Ok);
	CHECK(exact.Width == 3);
	CHECK(exact.Height == 7);
	CHECK(forbidden.Calls == 0);
	auto stale = recorder.Records;
	stale[0].Request.Measurements[0].MaximumLineWidth = 12.25;
	request.FontObservations = stale;
	const auto prior = exact;
	CHECK(Evaluate(graph, plan, "out", request, exact, diagnostic) == Status::InvalidValue);
	CHECK(exact == prior);
	CHECK(forbidden.Calls == 0);
}

TEST_CASE(
	"retained playing font measures changed full text and budget refusal preserves prior state for retry"
) {
	FontFiles files;
	auto graph = TextGraph(files, "A A", 12);
	auto plan = Persist(graph);
	GraphFontInputs owner;
	SourceFontContext held;
	EvaluationRequest request;
	Bind(owner, files, held, request);
	StatefulEvaluationResult first;
	Diagnostic diagnostic;
	REQUIRE(EvaluateStateful(graph, plan, "out", request, first, diagnostic) == Status::Ok);
	REQUIRE(std::holds_alternative<Image>(first.Output));
	const auto priorOutput = std::get<Image>(first.Output);
	const auto priorData = first.Data;
	Set(graph, "text", std::string{"AA AA"});
	plan = Persist(graph);
	held.Playing = true;
	request.Tick = 1;
	request.DataReplay = &first.Data;
	Forbidden forbidden;
	request.FontProvider = &forbidden;
	StatefulEvaluationResult result = first;
	CHECK(EvaluateStateful(graph, plan, "out", request, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(std::get<Image>(result.Output) == priorOutput);
	CHECK(result.Data == priorData);
	CHECK(std::get<Image>(first.Output) == priorOutput);
	CHECK(first.Data == priorData);
	const auto status = EvaluateStateful(graph, plan, "out", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(std::holds_alternative<Image>(result.Output));
	const auto &output = std::get<Image>(result.Output);
	CHECK(output.Width == 16);
	CHECK(output.Height == 20);
	CHECK(forbidden.Calls == 0);
}

TEST_CASE("real host measurement publication preserves prior receipt under cap refusal then retries") {
	FontFiles files;
	SourceFontRequest request;
	request.Authored = {"text", "pc.text", "", {}, {{"font", files.Primary.string()}, {"size", int64_t{10}}}};
	request.Context.AliasMapKnown = true;
	request.Context.Playing = false;
	request.Role = "font";
	request.ResolvedPath = files.Primary.string();
	request.PixelSize = 10;
	request.Characters = {32, 65};
	request.Measurements = {{"A A", 12.75, -1, 0, 0}};
	std::array grants{GraphFontFileGrant{"text", files.Primary, false, "font"}};
	GraphFontHost host(grants, {});
	SourceFontObservation output;
	std::string failure;
	REQUIRE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CheckMeasurement(output, "A A", 12.75, 8, 20);
	const auto prior = output;
	request.Measurements[0].Text = "AA AA";
	CHECK_FALSE(host.Observe(request, 1, output, failure));
	CHECK(output == prior);
	CHECK_FALSE(failure.empty());
	REQUIRE(host.Observe(request, Limits::MaximumEvaluationBytes, output, failure));
	CheckMeasurement(output, "AA AA", 12.75, 16, 20);
	CHECK(prior.Font->Data->Measurements[0].Text == "A A");
}
