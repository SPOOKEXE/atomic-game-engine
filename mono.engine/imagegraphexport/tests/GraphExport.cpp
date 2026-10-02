#include <engine/bake/GifSequence.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraphexport.graph_export")
TEST_DEPENDS("engine.imagegraphexport.runner")
TEST_DEPENDS("engine.parallel.process")

TEST_CASE("graph exporter builds literal encoder argv for each source format", "[assetc][imagegraph]") {
	engine::imagegraphexport::GraphExportSettings settings;
	settings.ImageEncoder = "/explicit/image encoder";
	settings.VideoEncoder = "/explicit/video encoder";
	std::filesystem::path executable;
	std::vector<std::string> arguments;
	std::string failure;
	for (const std::string extension : {".jpg", ".webp", ".ico", ".txt", ".gif", ".mp4", ".webm"}) {
		settings.Output = "output" + extension;
		settings.Animation = extension == ".gif" || extension == ".mp4" || extension == ".webm";
		const size_t count = settings.Animation ? 3 : 1;
		REQUIRE(
			engine::imagegraphexport::BuildGraphEncoderArguments(
				settings,
				"/frames with spaces",
				"/target/output" + extension,
				count,
				executable,
				arguments,
				failure
			)
		);
		CHECK(arguments.back() == "/target/output" + extension);
		CHECK(
			executable ==
			(extension == ".mp4" || extension == ".webm" ? settings.VideoEncoder : settings.ImageEncoder)
		);
		if (extension == ".mp4" || extension == ".webm") {
			CHECK(
				std::find(arguments.begin(), arguments.end(), "/frames with spaces/frame%08d.png") !=
				arguments.end()
			);
		} else {
			CHECK(
				std::find(arguments.begin(), arguments.end(), "/frames with spaces/frame00000000.png") !=
				arguments.end()
			);
		}
	}
	settings.Output = "animation.webp";
	settings.Animation = true;
	CHECK(
		engine::imagegraphexport::BuildGraphEncoderArguments(
			settings, "/frames", "/output.webp", 2, executable, arguments, failure
		)
	);
	CHECK(std::find(arguments.begin(), arguments.end(), "webp:lossless=true") != arguments.end());
	settings.ImageEncoder = "convert";
	CHECK_FALSE(
		engine::imagegraphexport::BuildGraphEncoderArguments(
			settings, "/frames", "/output.webp", 2, executable, arguments, failure
		)
	);
	CHECK(failure.find("explicit absolute") != std::string::npos);
}

TEST_CASE(
	"native graph export publishes real bytes and failed graph preserves prior output", "[assetc][imagegraph]"
) {
	const auto root = std::filesystem::temp_directory_path() / "atomic-graph-export-test";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
	} cleanup{root};
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = root / "source.graph";
	settings.Output = root / "target.bmp";
	settings.OutputId = "final";
	{
		std::ofstream stream(settings.Input);
		stream << "imagegraph 1\nnode \"solid\" \"image.solid\" \"\" 0 0\n"
				  "value 0 \"solid\" \"width\" i 1\nvalue 0 \"solid\" \"height\" i 1\n"
				  "value 0 \"solid\" \"colour\" c 255 0 0 255\noutput \"final\" \"solid\" \"image\"\n";
	}
	std::string failure;
	REQUIRE(engine::imagegraphexport::ExportGraph(settings, failure));
	std::ifstream published(settings.Output, std::ios::binary);
	const std::string bytes{std::istreambuf_iterator<char>(published), std::istreambuf_iterator<char>()};
	REQUIRE(bytes.size() == 58);
	CHECK(bytes.substr(0, 2) == "BM");
	{
		std::ofstream stream(settings.Input);
		stream << "imagegraph 1\nnode \"input\" \"image.captured\" \"\" 0 0\n"
				  "value 0 \"input\" \"source_id\" s \"photo\"\noutput \"final\" \"input\" \"image\"\n";
	}
	settings.ImageInputs.push_back({"photo", settings.Output});
	CHECK(engine::imagegraphexport::ExportGraph(settings, failure));
	std::ifstream roundTrip(settings.Output, std::ios::binary);
	CHECK(std::string(std::istreambuf_iterator<char>(roundTrip), std::istreambuf_iterator<char>()) == bytes);
	settings.OutputId = "missing";
	CHECK_FALSE(engine::imagegraphexport::ExportGraph(settings, failure));
	std::ifstream retained(settings.Output, std::ios::binary);
	CHECK(std::string(std::istreambuf_iterator<char>(retained), std::istreambuf_iterator<char>()) == bytes);
	for (const auto &entry : std::filesystem::directory_iterator(root))
		CHECK(entry.path().filename().string().find(".graph-export-") == std::string::npos);
}

TEST_CASE("GIF batch export stages bounded groups and merges literal ordered paths", "[assetc][imagegraph]") {
	if (!std::filesystem::exists("/usr/bin/python3")) SKIP("Python fixture executable unavailable");
	const auto root = std::filesystem::temp_directory_path() / "atomic-graph-gif-batch-test";
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
	const auto executable = root / "literal encoder";
	{
		std::ofstream script(executable);
		script << "#!/usr/bin/python3\nimport sys,pathlib,json,base64\na=sys.argv[1:]\n"
				  "with open(__file__+'.log','a') as f: f.write(json.dumps(a)+'\\n')\n"
				  "inputs=[x for x in a[:-1] if x.endswith(('.png','.gif'))]\n"
				  "assert inputs and all(pathlib.Path(x).is_file() for x in inputs)\n"
				  "pathlib.Path(a[-1]).write_bytes(base64.b64decode('R0lGODlhAQABAIAAAAAAAP///"
				  "yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'))\n";
	}
	std::filesystem::permissions(
		executable,
		std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
			std::filesystem::perms::owner_write
	);
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = root / "source.graph";
	settings.Output = root / "result.gif";
	settings.OutputId = "final";
	settings.ImageEncoder = executable;
	settings.Animation = true;
	settings.Frames = {0, 2, 1};
	settings.GifBatchSize = 2;
	{
		std::ofstream graph(settings.Input);
		graph << "imagegraph 1\nnode \"solid\" \"image.solid\" \"\" 0 0\n"
				 "value 0 \"solid\" \"width\" i 1\nvalue 0 \"solid\" \"height\" i 1\n"
				 "value 0 \"solid\" \"colour\" c 255 0 0 255\noutput \"final\" \"solid\" \"image\"\n";
	}
	std::string failure;
	REQUIRE(engine::imagegraphexport::ExportGraph(settings, failure));
	std::ifstream log(executable.string() + ".log");
	std::string first, second, merge;
	REQUIRE(static_cast<bool>(std::getline(log, first)));
	REQUIRE(static_cast<bool>(std::getline(log, second)));
	REQUIRE(static_cast<bool>(std::getline(log, merge)));
	CHECK(first.find("frame00000001.png") != std::string::npos);
	CHECK(second.find("frame00000001.png") == std::string::npos);
	CHECK(merge.find("batch-0.gif") < merge.find("batch-2.gif"));
	CHECK(merge.find("-delay") == std::string::npos);
	CHECK(std::filesystem::exists(settings.Output));
	settings.GifBatchSize = 4097;
	CHECK_FALSE(engine::imagegraphexport::ExportGraph(settings, failure));
	CHECK(std::filesystem::exists(settings.Output));
}

TEST_CASE(
	"CPU GIF export decodes real frames and retained APNG frames remain bounded", "[imagegraph][export]"
) {
	const auto root = std::filesystem::temp_directory_path() / "atomic-native-gif-export-test";
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
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = root / "source.graph";
	settings.Output = root / "output.gif";
	settings.OutputId = "final";
	settings.Animation = true;
	settings.Frames = {0, 2, 1};
	settings.FrameMilliseconds = 40;
	settings.NativeGif = true;
	settings.NativeGifQuality = 3;
	settings.RetainTemporaryFrames = true;
	{
		std::ofstream graph(settings.Input);
		graph << "imagegraph 1\nnode \"solid\" \"image.solid\" \"\" 0 0\n"
				 "value 0 \"solid\" \"width\" i 1\nvalue 0 \"solid\" \"height\" i 1\n"
				 "value 0 \"solid\" \"colour\" c 255 0 0 255\noutput \"final\" \"solid\" \"image\"\n";
	}
	std::filesystem::path retained;
	std::string failure;
	const bool written = engine::imagegraphexport::ExportGraph(settings, failure, &retained);
	INFO(failure);
	REQUIRE(written);
	CHECK(std::filesystem::exists(retained / "frame00000002.png"));
	std::ifstream stream(settings.Output, std::ios::binary);
	std::string text{std::istreambuf_iterator<char>(stream), {}};
	engine::assets::TextureSequenceData decoded;
	REQUIRE(
		engine::bake::ReadGifSequence(
			{reinterpret_cast<const std::byte *>(text.data()), text.size()}, decoded, failure
		)
	);
	REQUIRE(decoded.FrameDurations.size() == 3);
	CHECK(decoded.Width == 1);
	CHECK(decoded.Height == 1);
	CHECK(decoded.FrameDurations[0] == .04f);
	CHECK(decoded.Pixels[0] == std::byte{255});
	settings.NativeGif = false;
	settings.Output = root / "output.apng";
	REQUIRE(engine::imagegraphexport::ExportGraph(settings, failure, &retained));
	CHECK(std::filesystem::exists(retained / "frame00000000.png"));
	CHECK(std::filesystem::exists(retained / "frame00000002.png"));
	settings.RetainTemporaryFrames = false;
	settings.Output = root / "clean.apng";
	REQUIRE(engine::imagegraphexport::ExportGraph(settings, failure, &retained));
	CHECK(retained.empty());
}

TEST_CASE(
	"live host execution resolves signed frame controls without reading a graph file", "[imagegraph][host]"
) {
	using namespace engine::imagegraph;
	struct Provider final : HostNodeProvider {
		bool Capture(const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &) override {
			output.Authored = invocation.Authored;
			output.Tick = invocation.Request.Tick;
			output.Subframe = invocation.Request.Subframe;
			output.NegativeFrame = invocation.Request.NegativeFrame;
			output.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
			output.Outputs = {
				{"content", std::string{"live contents"}}, {"path", std::string{"not-on-disk"}}
			};
			return true;
		}
	} provider;
	Document document;
	document.Nodes.push_back({"read", "pc.text_file_read", "", {}, {{"path", std::string{"not-on-disk"}}}});
	document.Outputs.push_back({"text", "read", "content"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 4;
	request.Subframe = .25;
	request.NegativeFrame = true;
	request.HostProvider = &provider;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::ExecuteGraphHostNode(document, plan, request, "read", capture, failure)
	);
	CHECK(capture.Tick == 4);
	CHECK(capture.Subframe == .25);
	CHECK(capture.NegativeFrame);
	request.HostProvider = nullptr;
	request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(value.Data) == "live contents");
}
