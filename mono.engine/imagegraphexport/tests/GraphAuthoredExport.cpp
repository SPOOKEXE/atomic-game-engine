#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
TEST_SUITE_ID("engine.imagegraphexport.graph_authored_export")
TEST_DEPENDS("engine.imagegraph.document")
namespace {
	using namespace engine::imagegraph;
	struct Fixture {
		Node Export;
		EvaluationRequest Request;
		TimelineSettings Timeline{10, 0, 9, "loop", 25};
		std::vector<AuthoredValue> Inputs{
			{"directory", std::string{"/tmp/pc-export-grant"}},
			{"file_name", std::string{"tile.png"}},
			{"template", std::string{"%d%n%3f_%{i+1}"}},
			{"type", EnumValue{1}},
			{"format", EnumValue{0}},
			{"frame_step", int64_t{2}},
			{"sequence_begin", int64_t{4}},
			{"quality_2", 23.0},
			{"bit_rate_mbps", 2.0},
			{"scale", 2.0},
			{"frame_timing", EnumValue{0}},
			{"framerate", 1.0},
			{"frame_time_ms", 33.0},
			{"subformat", EnumValue{2}},
			{"custom_range", true},
			{"frame_range", Vector2{2, 6}},
			{"loop", false},
			{"render_region", false},
			{"export_regions", ArrayValue{ValueType::Text, {}}}
		};
		engine::imagegraphexport::GraphExportSettings Grants;
		Fixture() {
			Export.Id = "export";
			Export.Type = "pc.export";
			Grants.Input = "/tmp/project.graph";
			Grants.Output = "/tmp/pc-export-grant";
			Grants.OutputId = "preview";
		}
		void Set(std::string_view port, Value value) {
			std::find_if(Inputs.begin(), Inputs.end(), [&](auto &v) { return v.Port == port; })->Data =
				std::move(value);
		}
		HostNodeInvocation Call() {
			return {Export, Request, Inputs, {}, 1048576, &Timeline};
		}
	};
}
TEST_CASE(
	"Authored export plans source one-based ranges, templates and codec settings", "[assetc][imagegraph]"
) {
	Fixture fixture;
	std::vector<engine::imagegraphexport::GraphExportSettings> exports;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	REQUIRE(exports.size() == 3);
	CHECK(exports[0].Output == "/tmp/pc-export-grant/tile006_1.png");
	CHECK(exports[1].Output == "/tmp/pc-export-grant/tile008_1.png");
	CHECK(exports[2].Frames.First == 5);
	CHECK(exports[2].Frames.Last == 5);
	CHECK(exports[0].FrameMilliseconds == 40);
	CHECK(exports[0].Plays == 1);
	CHECK(exports[0].Scale == 2);
	fixture.Set("type", EnumValue{2});
	fixture.Set("format", EnumValue{3});
	fixture.Set("frame_timing", EnumValue{1});
	fixture.Set("frame_time_ms", 75.0);
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	REQUIRE(exports.size() == 1);
	CHECK(exports[0].Animation);
	CHECK(exports[0].Frames.First == 1);
	CHECK(exports[0].Frames.Last == 5);
	CHECK(exports[0].Frames.Step == 2);
	CHECK(exports[0].FrameMilliseconds == 75);
	CHECK(exports[0].Output.extension() == ".mp4");
}
TEST_CASE(
	"Authored export requires bounded unique paths and recorded named region ranges", "[assetc][imagegraph]"
) {
	Fixture fixture;
	std::vector<engine::imagegraphexport::GraphExportSettings> exports;
	std::string failure;
	fixture.Set("template", std::string{"%d%n"});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	fixture.Set("template", std::string{"%1d%n%f"});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	fixture.Set("template", std::string{"%d%n%{i/0}"});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	fixture.Set("template", std::string{"%d%n%f_%r"});
	fixture.Set("type", EnumValue{2});
	fixture.Set("render_region", true);
	fixture.Set("export_regions", ArrayValue{ValueType::Text, {std::string{"intro"}, std::string{"outro"}}});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	const std::array<engine::imagegraphexport::GraphExportRegion, 2> regions{
		{{"intro", {0, 3, 1}}, {"outro", {7, 9, 1}}}
	};
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, regions, exports, failure
		)
	);
	REQUIRE(exports.size() == 2);
	CHECK(exports[0].Frames.First == 0);
	CHECK(exports[1].Frames.First == 7);
	CHECK(exports[1].Output == "/tmp/pc-export-grant/tile12_outro.gif");
	fixture.Set("frame_step", int64_t{0});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, regions, exports, failure
		)
	);
}

TEST_CASE(
	"Authored array exports use source array indices and APNG exact frame delays", "[assetc][imagegraph]"
) {
	Fixture fixture;
	fixture.Set("type", EnumValue{0});
	fixture.Set("template", std::string{"%d%n_%{i+1}"});
	std::vector<engine::imagegraphexport::GraphExportSettings> exports;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure, 2
		)
	);
	REQUIRE(exports.size() == 2);
	CHECK(exports[0].ArrayIndex == 0);
	CHECK(exports[1].ArrayIndex == 1);
	CHECK(exports[1].Output == "/tmp/pc-export-grant/tile_2.png");
	fixture.Set("type", EnumValue{2});
	fixture.Set("format", EnumValue{1});
	fixture.Set("frame_timing", EnumValue{1});
	fixture.Set("frame_time_ms", 75.0);
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	REQUIRE(exports.size() == 1);
	CHECK(exports[0].DelayNumerator == 1);
	CHECK(exports[0].DelayDenominator == 14);
	fixture.Set("frame_timing", EnumValue{0});
	fixture.Set("framerate", 1.0);
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	CHECK(exports[0].DelayNumerator == 1);
	CHECK(exports[0].DelayDenominator == 25);
}

TEST_CASE(
	"Authored GIF routes retain native quantization and external batching controls", "[assetc][imagegraph]"
) {
	Fixture fixture;
	fixture.Set("type", EnumValue{2});
	fixture.Inputs.push_back({"use_built_in_gif_encoder", true});
	fixture.Inputs.push_back({"quality", int64_t{3}});
	fixture.Inputs.push_back({"batch_gif", int64_t{2}});
	std::vector<engine::imagegraphexport::GraphExportSettings> exports;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	REQUIRE(exports.size() == 1);
	CHECK(exports[0].NativeGif);
	CHECK(exports[0].NativeGifQuality == 3);
	CHECK(exports[0].GifBatchSize == 2);
	fixture.Set("quality", int64_t{4});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
	CHECK(exports.empty());
	fixture.Set("quality", int64_t{2});
	fixture.Set("batch_gif", int64_t{-1});
	CHECK_FALSE(
		engine::imagegraphexport::PlanAuthoredGraphExport(
			fixture.Call(), fixture.Grants, {}, exports, failure
		)
	);
}

TEST_CASE(
	"Authored export resolves ordered document regions through the real file codec", "[assetc][imagegraph]"
) {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-authored-region-test";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code e;
			std::filesystem::remove_all(Path, e);
		}
	} cleanup{root};
	Document document;
	document.FormatVersion = 9;
	document.Project.emplace();
	document.Project->AnimationRegions.push_back({"intro", {255, 0, 0, 255}, FrameTime{2}, FrameTime{3}});
	document.Timeline = TimelineSettings{4, 0, 3, "loop", 30};
	document.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}}
	);
	document.Nodes.push_back(
		{"export",
		 "pc.export",
		 "",
		 {},
		 {{"directory", root.string()},
		  {"file_name", std::string{"tile"}},
		  {"template", std::string{"%d%n%f_%r"}},
		  {"type", EnumValue{1}},
		  {"render_region", true},
		  {"export_regions", ArrayValue{ValueType::Text, {std::string{"intro"}}}}}}
	);
	document.Links.push_back({"solid", "image", "export", "surface"});
	document.Outputs.push_back({"preview", "export", "preview"});
	std::string encoded = Write(document);
	REQUIRE_FALSE(encoded.empty());
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = root / "input.graph";
	settings.Output = root;
	settings.OutputId = "preview";
	{
		std::ofstream graph(settings.Input);
		graph << encoded;
	}
	std::string failure;
	const auto exported = engine::imagegraphexport::ExportAuthoredGraphNode(settings, "export", {}, failure);
	INFO(failure);
	REQUIRE(exported);
	CHECK(std::filesystem::exists(root / "tile3_intro.png"));
	CHECK(std::filesystem::exists(root / "tile4_intro.png"));
	CHECK_FALSE(std::filesystem::exists(root / "tile1_intro.png"));
	document.Project->AnimationRegions[0].Start.Subframe = .5;
	encoded = Write(document);
	REQUIRE_FALSE(encoded.empty());
	{
		std::ofstream graph(settings.Input);
		graph << encoded;
	}
	CHECK_FALSE(engine::imagegraphexport::ExportAuthoredGraphNode(settings, "export", {}, failure));
	CHECK(std::filesystem::exists(root / "tile3_intro.png"));
}

TEST_CASE(
	"Live authored export adds an ephemeral preview and preserves signed fractional frames",
	"[imagegraph][export]"
) {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-live-authored-export-test";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	Document document;
	document.FormatVersion = 9;
	document.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}}
	);
	document.Nodes.push_back(
		{"export",
		 "pc.export",
		 "",
		 {},
		 {{"directory", root.string()},
		  {"file_name", std::string{"tile"}},
		  {"template", std::string{"%d%n_%f"}},
		  {"type", EnumValue{0}}}}
	);
	document.Links.push_back({"solid", "image", "export", "surface"});
	document.Outputs.push_back({"other", "solid", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .5;
	request.NegativeFrame = true;
	engine::imagegraphexport::GraphExportSettings grants;
	grants.Input = root / "unsaved-context.graph";
	grants.Output = root;
	std::string failure;
	const bool result =
		engine::imagegraphexport::ExportAuthoredGraphNode(document, plan, request, grants, "export", failure);
	INFO(failure);
	REQUIRE(result);
	CHECK(std::filesystem::exists(root / "tile_-1.5.png"));
	CHECK_FALSE(std::filesystem::exists(grants.Input));
	REQUIRE(document.Outputs.size() == 1);
	CHECK(document.Outputs[0].Id == "other");
}

TEST_CASE("authored sequence stages every frame before replacing any destination", "[imagegraph][export]") {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-authored-export-batch-test";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	} cleanup{root};
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
	document.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
		{"export",
		 "pc.export",
		 "",
		 {},
		 {{"directory", root.string()},
		  {"file_name", std::string{"tile"}},
		  {"template", std::string{"%d%n%f"}},
		  {"type", EnumValue{1}}}}
	};
	document.Links = {{"solid", "image", "export", "surface"}};
	document.Outputs = {{"preview", "export", "preview"}};
	const auto nested = root / "nested" / "frames";
	for (auto &value : document.Nodes[1].Values)
		if (value.Port == "template") value.Data = std::string{"%dnested/frames/%n%f"};
	const auto first = nested / "tile1.png", second = nested / "tile2.png";
	std::filesystem::create_directories(nested);
	{
		std::ofstream output(first, std::ios::binary);
		output << "previous first";
	}
	const auto text = [](const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), {});
	};
	SECTION("a later evaluation failure preserves earlier bytes and creates no new output") {
		document.Keyframes = {
			{"solid", "width", 0, int64_t{1}, "step"}, {"solid", "width", 1, int64_t{0}, "step"}
		};
	}
	SECTION("all frames publish successfully") {}
	SECTION("new nested parents are created for a successful batch") {
		std::filesystem::remove_all(root / "nested");
	}
	SECTION("a failed batch removes its newly created empty nested parents") {
		std::filesystem::remove_all(root / "nested");
		document.Keyframes = {
			{"solid", "width", 0, int64_t{1}, "step"}, {"solid", "width", 1, int64_t{0}, "step"}
		};
	}
	const bool hadPrevious = std::filesystem::exists(first);
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	engine::imagegraphexport::GraphExportSettings grants;
	grants.Input = root / "unsaved.graph";
	grants.Output = root;
	grants.OutputId = "preview";
	EvaluationRequest request;
	std::string failure;
	const auto exported =
		engine::imagegraphexport::ExportAuthoredGraphNode(document, plan, request, grants, "export", failure);
	INFO(failure);
	if (!document.Keyframes.empty()) {
		CHECK_FALSE(exported);
		if (hadPrevious)
			CHECK(text(first) == "previous first");
		else
			CHECK_FALSE(std::filesystem::exists(root / "nested"));
		CHECK_FALSE(std::filesystem::exists(second));
	} else {
		REQUIRE(exported);
		CHECK(text(first).starts_with(std::string{"\x89PNG", 4}));
		CHECK(text(second).starts_with(std::string{"\x89PNG", 4}));
	}
	for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
		CHECK_FALSE(entry.path().filename().string().starts_with(".graph-export-"));
}

TEST_CASE(
	"authored batch rolls back an earlier replacement when later publication is refused",
	"[imagegraph][export]"
) {
#ifdef _WIN32
	SKIP("literal executable fixture uses the POSIX Python interpreter");
#else
	using namespace engine::imagegraph;
	if (!std::filesystem::exists("/usr/bin/python3")) SKIP("explicit Python fixture interpreter unavailable");
	const auto root = std::filesystem::temp_directory_path() / "atomic-export-publication-rollback-test";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	} cleanup{root};
	const auto first = root / "tile1.jpg", second = root / "tile2.jpg";
	{
		std::ofstream output(first, std::ios::binary);
		output << "previous first";
	}
	const auto encoder = root / "encoder.py", marker = root / "calls";
	bool failEncoding = false;
	SECTION("second encoder fails before any file publication") {
		failEncoding = true;
	}
	SECTION("second destination changes after staging and the first publication is restored") {}
	{
		std::ofstream script(encoder);
		script << "#!/usr/bin/python3\nfrom pathlib import Path\nimport sys\nmarker=Path("
			   << std::quoted(marker.string()) << ")\n";
		script << "if marker.exists():\n";
		if (failEncoding)
			script << "    sys.exit(42)\n";
		else
			script << "    Path(" << std::quoted(second.string()) << ").symlink_to("
				   << std::quoted(first.string()) << ")\n";
		script << "marker.write_text('called')\nPath(sys.argv[-1]).write_bytes(b'\\xff\\xd8\\xfffixture')\n";
	}
	std::filesystem::permissions(encoder, std::filesystem::perms::owner_all);
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
	document.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
		{"export",
		 "pc.export",
		 "",
		 {},
		 {{"directory", root.string()},
		  {"file_name", std::string{"tile"}},
		  {"template", std::string{"%d%n%f"}},
		  {"type", EnumValue{1}},
		  {"format", EnumValue{1}}}}
	};
	document.Links = {{"solid", "image", "export", "surface"}};
	document.Outputs = {{"preview", "export", "preview"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphexport::GraphExportSettings grants;
	grants.Input = root / "unsaved.graph";
	grants.Output = root;
	grants.OutputId = "preview";
	grants.ImageEncoder = encoder;
	std::string failure;
	std::vector<std::filesystem::path> retained{root / "previous-retained"};
	CHECK_FALSE(
		engine::imagegraphexport::ExportAuthoredGraphNode(
			document, plan, EvaluationRequest{}, grants, "export", failure, &retained
		)
	);
	INFO(failure);
	std::ifstream restored(first, std::ios::binary);
	CHECK(std::string(std::istreambuf_iterator<char>(restored), {}) == "previous first");
	CHECK(retained == std::vector<std::filesystem::path>{root / "previous-retained"});
	if (!failEncoding) {
		CHECK(failure.find("changed before publication") != std::string::npos);
		REQUIRE(std::filesystem::is_symlink(second));
		std::filesystem::remove(second); // Remove the deliberate fixture mutation, not an exported file.
	}
	CHECK_FALSE(std::filesystem::exists(second));
	for (const auto &entry : std::filesystem::directory_iterator(root))
		CHECK_FALSE(entry.path().filename().string().starts_with(".graph-export-"));
#endif
}
