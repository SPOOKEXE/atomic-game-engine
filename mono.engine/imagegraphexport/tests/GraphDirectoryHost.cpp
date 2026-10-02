#include <engine/core/Metrics.hpp>
#include <engine/imagegraphexport/GraphDirectoryHost.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
TEST_SUITE_ID("engine.imagegraphexport.graph_directory_host")
TEST_DEPENDS("engine.imagegraph.host_capture")
namespace {
	constexpr std::array<uint8_t, 77> PNG_RGB{
		{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xFD, 0xD4, 0x9A,
		 0x73, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xC0,
		 0x00, 0xC2, 0x0C, 0xFF, 0xFF, 0xFF, 0x67, 0x00, 0x00, 0x1E, 0xEF, 0x04, 0xFC, 0x73, 0x1C, 0x53,
		 0xCC, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}
	};
	void Write(const std::filesystem::path &file) {
		std::ofstream stream(file, std::ios::binary);
		stream.write(reinterpret_cast<const char *>(PNG_RGB.data()), PNG_RGB.size());
		REQUIRE(bool(stream));
	}
}
TEST_CASE(
	"Directory host observes owned native order and source traversal quirks", "[directory_host][imagegraph]"
) {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	const auto root = std::filesystem::temp_directory_path() /
					  ("atomic-directory-host-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	REQUIRE(std::filesystem::create_directory(root));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	} cleanup{root};
	const auto child = root / "child", grandchild = child / "grandchild";
	REQUIRE(std::filesystem::create_directories(grandchild));
	const auto first = root / "z.PNG", second = root / "a.png", third = child / "child.png",
			   fourth = grandchild / "grandchild.png";
	for (const auto &file : {first, second, third, fourth})
		Write(file);
	Node node;
	node.Id = "directory";
	node.Type = "pc.directory_search";
	std::vector<AuthoredValue> controls{
		{"path", root.string()},
		{"extensions", std::string(".png")},
		{"type", EnumValue{0}},
		{"recursive", false}
	};
	std::array<GraphDirectoryGrant, 1> directories{{{node.Id, root}}};
	std::vector<GraphFileGrant> files;
	for (const auto &file : {first, second, third, fourth})
		files.push_back({node.Id, file, false, file.string()});
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .25;
	HostNodeInvocation in{node, request, controls, {}, 16 * 1024 * 1024};
	GraphDirectoryObservation observation;
	std::string failure;
	REQUIRE(ObserveGraphDirectory(in, directories, observation, failure));
	CHECK(observation.Listings.size() == 2);
	CHECK(observation.Listings[0].Directory == root);
	CHECK(observation.Listings[1].Directory == child);
	HostNodeCapture capture;
	REQUIRE(CaptureGraphDirectory(in, observation, files, {}, capture, failure));
	REQUIRE(capture.ImageArrays.size() == 1);
	CHECK(capture.ImageArrays[0].Port == "outputs");
	CHECK(capture.ImageArrays[0].Frames.size() == 3);
	CHECK(capture.Tick == 2);
	CHECK(capture.Subframe == .25);
	auto actual = std::get<ArrayValue>(capture.Outputs[0].Data);
	CHECK(
		std::find(actual.Elements.begin(), actual.Elements.end(), ElementValue{fourth.string()}) ==
		actual.Elements.end()
	);
	auto &entries = observation.Listings[0].Entries;
	auto one =
		std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.File == first; });
	auto two =
		std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.File == second; });
	REQUIRE(one != entries.end());
	REQUIRE(two != entries.end());
	std::iter_swap(one, two);
	REQUIRE(CaptureGraphDirectory(in, observation, files, {}, capture, failure));
	const auto reordered = std::get<ArrayValue>(capture.Outputs[0].Data);
	CHECK(reordered.Elements[0] == actual.Elements[1]);
	CHECK(reordered.Elements[1] == actual.Elements[0]);
	GraphDirectoryHost provider(directories, files, {});
	REQUIRE(provider.Capture(in, capture, failure));
	const auto original = capture.ImageArrays[0].Frames;
	SECTION("compiled image and text graph outputs use the actual granted file provider") {
		node.Values = controls;
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {node};
		document.Outputs = {{"images", node.Id, "outputs"}, {"paths", node.Id, "paths"}};
		Plan plan;
		Diagnostic diagnostic;
		INFO(diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		GraphFileHost fileProvider(files, {}, directories);
		request.HostProvider = &fileProvider;
		ImageArray images;
		REQUIRE(EvaluateArray(document, plan, "images", request, images, diagnostic) == Status::Ok);
		CHECK(images.Images.size() == 3);
		EvaluatedValue paths;
		REQUIRE(EvaluateValue(document, plan, "paths", request, paths, diagnostic) == Status::Ok);
		CHECK(std::get<ArrayValue>(paths.Data).Elements.size() == 3);
		REQUIRE(ExecuteGraphHostNode(document, plan, request, node.Id, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.size() == 3);
		document.Nodes[0].Values[2].Data = EnumValue{1};
		document.Outputs = {{"text", node.Id, "outputs"}};
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue text;
		REQUIRE(EvaluateValue(document, plan, "text", request, text, diagnostic) == Status::Ok);
		const auto &array = std::get<ArrayValue>(text.Data);
		CHECK(array.ElementType == ValueType::Text);
		CHECK(array.Elements.empty());
		REQUIRE(ExecuteGraphHostNode(document, plan, request, node.Id, capture, failure));
		CHECK(capture.ImageArrays.empty());
		CHECK(std::get<ArrayValue>(capture.Outputs[1].Data).Elements.empty());
	}
	SECTION("linked and animated selectors cannot silently change the compiled output type") {
		node.Values = controls;
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {node};
		document.Outputs = {{"images", node.Id, "outputs"}};
		document.Junctions = {{"selector", "", ValueType::Enum, EnumValue{1}}};
		document.Links = {{"selector", "value", node.Id, "type"}};
		Plan plan;
		Diagnostic diagnostic;
		CHECK(Compile(document, plan, diagnostic) == Status::UnsupportedExecution);
		CHECK(diagnostic.NodeId == node.Id);
		CHECK(diagnostic.Port == "type");
		CHECK(diagnostic.Message.find("static authored type") != std::string::npos);
		document.Links.clear();
		document.Nodes[0].SourceAnimatedInputs = {"type"};
		CHECK(Compile(document, plan, diagnostic) == Status::UnsupportedExecution);
		CHECK(diagnostic.NodeId == node.Id);
		CHECK(diagnostic.Port == "type");
	}
	SECTION("owned order byte accounting and malformed grants preserve capture") {
		const auto bytes = GraphDirectoryObservationBytes(observation);
		REQUIRE(bytes);
		CHECK(*bytes < in.MaximumOperationBytes / 8);
		auto invalid = observation;
		invalid.Listings.resize(65);
		CHECK_FALSE(GraphDirectoryObservationBytes(invalid));
		directories[0].NodeId = "other";
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == original);
	}
	SECTION("each source image retains its original dimensions") {
		constexpr std::array<uint8_t, 70> small{137, 80, 78, 71, 13, 10,  26,  10,	0,	 0,	  0,   13,
												73,	 72, 68, 82, 0,	 0,	  0,   1,	0,	 0,	  0,   1,
												8,	 6,	 0,	 0,	 0,	 31,  21,  196, 137, 0,	  0,   0,
												13,	 73, 68, 65, 84, 120, 156, 99,	248, 207, 192, 240,
												31,	 0,	 5,	 0,	 1,	 255, 137, 153, 61,	 29,  0,   0,
												0,	 0,	 73, 69, 78, 68,  174, 66,	96,	 130};
		{
			std::ofstream stream(first, std::ios::binary);
			stream.write(reinterpret_cast<const char *>(small.data()), small.size());
			REQUIRE(bool(stream));
		}
		REQUIRE(provider.Capture(in, capture, failure));
		const auto &ordered = std::get<ArrayValue>(capture.Outputs[0].Data).Elements;
		REQUIRE(ordered.size() == 3);
		for (size_t index = 0; index < ordered.size(); ++index) {
			const auto expected = std::get<std::string>(ordered[index]) == first.string() ? 1u : 2u;
			CHECK(capture.ImageArrays[0].Frames[index].Width == expected);
			CHECK(capture.ImageArrays[0].Frames[index].Height == expected);
		}
	}
	SECTION("source decoder failures are skipped while grants and native caps are enforced") {
		auto corrupt = PNG_RGB;
		corrupt[49] ^= 0x40;
		{
			std::ofstream stream(first, std::ios::binary);
			stream.write(reinterpret_cast<const char *>(corrupt.data()), corrupt.size());
			REQUIRE(bool(stream));
		}
		REQUIRE(provider.Capture(in, capture, failure));
		CHECK(failure.empty());
		CHECK(capture.ImageArrays[0].Frames.size() == 2);
		const auto &paths = std::get<ArrayValue>(capture.Outputs[0].Data).Elements;
		CHECK(std::find(paths.begin(), paths.end(), ElementValue{first.string()}) == paths.end());
		const auto skipped = capture.ImageArrays[0].Frames;
		files[0].Resource = "denied";
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == skipped);
		files[0].Resource = first.string();
		corrupt = PNG_RGB;
		corrupt[16] = 0x7f;
		{
			std::ofstream stream(first, std::ios::binary);
			stream.write(reinterpret_cast<const char *>(corrupt.data()), corrupt.size());
			REQUIRE(bool(stream));
		}
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == skipped);
		corrupt = PNG_RGB;
		corrupt[28] = 1;
		{
			std::ofstream stream(first, std::ios::binary);
			stream.write(reinterpret_cast<const char *>(corrupt.data()), corrupt.size());
			REQUIRE(bool(stream));
		}
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == skipped);
	}
	SECTION("a disabled image codec cannot be mistaken for a skipped invalid sprite") {
		engine::assets::ContentPolicy denied;
		denied.Allow(engine::assets::ContentForm::Png, false);
		GraphDirectoryHost restricted(directories, files, denied);
		CHECK_FALSE(restricted.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == original);
	}
	SECTION("provider owns one native order until explicit refresh") {
		const auto later = root / "later.png";
		Write(later);
		request.Tick = 99;
		REQUIRE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.size() == 3);
		CHECK(capture.Tick == 99);
		provider.Refresh(node.Id);
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.size() == 3);
		auto expanded = files;
		expanded.push_back({node.Id, later, false, later.string()});
		GraphDirectoryHost refreshed(directories, expanded, {});
		REQUIRE(refreshed.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.size() == 4);
	}
	SECTION("metrics separate actual filesystem work from recorded order replay") {
		const auto count = [](std::string_view name) {
			for (const auto &counter : engine::core::Metrics::Snapshot().Counters)
				if (counter.Name.Text() == name) return counter.Value;
			return 0.;
		};
		const auto enumeration = count("image composer directory enumeration operations");
		const auto entries = count("image composer directory entries observed");
		const auto reads = count("image composer raster read operations");
		const auto bytes = count("image composer raster encoded bytes read");
		const auto decodes = count("image composer raster decode operations");
		const auto replays = count("image composer directory order replays");
		const auto pixels = count("image composer directory image payload bytes emitted");
		REQUIRE(provider.Capture(in, capture, failure));
		CHECK(count("image composer directory enumeration operations") == enumeration);
		CHECK(count("image composer directory entries observed") == entries);
		CHECK(count("image composer raster read operations") == reads + 3);
		CHECK(count("image composer raster encoded bytes read") == bytes + 3 * PNG_RGB.size());
		CHECK(count("image composer raster decode operations") == decodes + 3);
		CHECK(count("image composer directory order replays") == replays + 1);
		CHECK(count("image composer directory image payload bytes emitted") == pixels + 3 * 2 * 2 * 4);
	}
	SECTION("source extension filters retain literal case and Text output remains empty") {
		controls[1].Data = std::string(".PNG");
		REQUIRE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.empty());
		controls[1].Data = std::string(".png");
		controls[2].Data = EnumValue{1};
		REQUIRE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays.empty());
		REQUIRE(capture.Outputs.size() == 2);
		CHECK(std::get<ArrayValue>(capture.Outputs[1].Data).ElementType == ValueType::Text);
		CHECK(std::get<ArrayValue>(capture.Outputs[1].Data).Elements.empty());
	}
	SECTION("recursive traversal and a source trailing root separator are observed explicitly") {
		controls[3].Data = true;
		directories[0].Root = root / "";
		controls[0].Data = directories[0].Root.string();
		REQUIRE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.size() == 4);
	}
	SECTION("stale or duplicated recorded listings preserve the prior capture") {
		auto corrupted = observation;
		corrupted.Listings[0].Entries.push_back(corrupted.Listings[0].Entries[0]);
		CHECK_FALSE(CaptureGraphDirectory(in, corrupted, files, {}, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == original);
		corrupted = observation;
		corrupted.Recursive = true;
		CHECK_FALSE(CaptureGraphDirectory(in, corrupted, files, {}, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == original);
		in.MaximumOperationBytes = 32;
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == original);
	}
	SECTION("each selected file retains its own read grant") {
		files[0].Resource = "different-resource";
		CHECK_FALSE(provider.Capture(in, capture, failure));
		CHECK(capture.ImageArrays[0].Frames == original);
	}
}
