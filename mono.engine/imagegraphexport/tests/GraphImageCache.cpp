#include "GraphImageCacheHost.hpp"

#include <engine/imagegraphexport/GraphImageCache.hpp>
#include <engine/imagegraphexport/Runner.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <fstream>

TEST_SUITE_ID("engine.imagegraphexport.graph_image_cache")
TEST_DEPENDS("engine.imagegraphio.source_image_edit")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	constexpr std::string_view TwoFrames =
		R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	std::string Identity(std::string_view data) {
		const auto hash = engine::bake::SpriteCacheDataHash(data);
		REQUIRE(hash);
		return std::string(hash->data(), hash->size());
	}
	Document CachedSequence(bool annotated = true) {
		Document document;
		document.FormatVersion = 9;
		Node node;
		node.Id = "images";
		node.Type = "pc.image_sequence";
		node.Values = {
			{"paths", ArrayValue{ValueType::Text, {std::string("ungranted-and-absent.png")}}},
			{"padding", Vector4{}},
			{"canvas_size", EnumValue{0}},
			{"sizing_method", EnumValue{0}}
		};
		node.SourceProperties = {{"cache_use", true}, {"cache_data", std::string(TwoFrames)}};
		if (annotated) {
			node.SourceProperties.push_back({"composer_sprite_cache_layout", std::string("rgba8-top-down")});
			node.SourceProperties.push_back({"composer_sprite_cache_data_hash", Identity(TwoFrames)});
		}
		document.Nodes = {node};
		document.Outputs = {{"images", node.Id, "surfaces_out"}};
		return document;
	}
	struct Folder {
		std::filesystem::path Path;
		Folder() {
			static std::atomic<uint64_t> sequence = 0;
			Path = std::filesystem::temp_directory_path() /
				   ("source-image-cache-" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
					std::to_string(sequence.fetch_add(1)));
			REQUIRE(std::filesystem::create_directory(Path));
		}
		~Folder() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	};
}
TEST_CASE(
	"A real cached image-array graph renders exact pixels without original file grants",
	"[imagegraph][image_cache]"
) {
	auto document = CachedSequence();
	GraphFileHost host({}, {});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &host;
	ImageArray output;
	const auto rendered = EvaluateArray(document, plan, "images", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(rendered == Status::Ok);
	REQUIRE(output.Images.size() == 2);
	CHECK(output.Images[0].Width == 1);
	CHECK(output.Images[1].Width == 2);
	CHECK(output.Images[0].Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	CHECK(output.Images[1].Pixels == std::vector<uint8_t>{200, 10, 40, 0, 255, 128, 64, 255});
	Document native;
	REQUIRE(Read(Write(document), native, diagnostic) == Status::Ok);
	REQUIRE(Compile(native, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(native, plan, "images", request, replay, diagnostic) == Status::Ok);
	CHECK(replay.Images == output.Images);
	CHECK(replay.Items == output.Items);
	const auto previous = output;
	std::get<std::string>(native.Nodes[0].SourceProperties[1].Data) += " ";
	REQUIRE(Compile(native, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateArray(native, plan, "images", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output.Images == previous.Images);
	CHECK(output.Items == previous.Items);
}
TEST_CASE(
	"Foreign image cache playback requires a fresh nonconflicting layout observation",
	"[imagegraph][image_cache]"
) {
	auto document = CachedSequence(false);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	ImageArray output;
	GraphFileHost denied({}, {});
	request.HostProvider = &denied;
	CHECK(
		EvaluateArray(document, plan, "images", request, output, diagnostic) == Status::UnsupportedExecution
	);
	std::array<GraphImageCacheLayoutObservation, 1> layouts{
		{{"images", Identity(TwoFrames), engine::bake::SpriteCacheLayout::Rgba8TopDown}}
	};
	GraphFileHost observed({}, {}, {}, layouts);
	request.HostProvider = &observed;
	REQUIRE(EvaluateArray(document, plan, "images", request, output, diagnostic) == Status::Ok);
	CHECK(output.Images[0].Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	const auto previous = output;
	layouts[0].DataHash = std::string(64, '0');
	CHECK(
		EvaluateArray(document, plan, "images", request, output, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(output.Images == previous.Images);
	CHECK(output.Items == previous.Items);
	document = CachedSequence();
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	layouts[0].DataHash = Identity(TwoFrames);
	layouts[0].Layout = engine::bake::SpriteCacheLayout::Bgra8TopDown;
	CHECK(
		EvaluateArray(document, plan, "images", request, output, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(output.Images == previous.Images);
	CHECK(output.Items == previous.Items);
}
TEST_CASE(
	"Live cache authoring reads exact granted originals while an older cache remains enabled",
	"[imagegraph][image_cache]"
) {
	Folder folder;
	const Image first{1, 1, {99, 66, 33, 127}}, second{2, 1, {5, 10, 15, 255, 20, 25, 30, 0}};
	std::string failure;
	const auto one = folder.Path / "one.png", two = folder.Path / "two.png";
	REQUIRE(runner::WriteStillImage(one, first, failure));
	REQUIRE(runner::WriteStillImage(two, second, failure));
	auto document = CachedSequence();
	document.Nodes[0].Values[0].Data = ArrayValue{ValueType::Text, {one.string(), two.string()}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	HostNodeCapture controls;
	REQUIRE(PrepareHostCapture(document, plan, "images", request, controls, diagnostic) == Status::Ok);
	const std::array<GraphFileGrant, 2> grants{
		{{"images", one, false, one.string()}, {"images", two, false, two.string()}}
	};
	engine::imagegraphio::SourceImageFrameObservation prepared;
	const bool admitted = PrepareGraphSourceImages(controls, grants, {}, 7, 11, true, prepared, failure);
	INFO(failure);
	REQUIRE(admitted);
	REQUIRE(prepared.Frames.size() == 2);
	CHECK(prepared.Frames[0].Pixels == first.Pixels);
	CHECK(prepared.Frames[1].Pixels == second.Pixels);
	REQUIRE(prepared.EncodedCache);
	REQUIRE(prepared.CacheLayout);
	std::vector<engine::bake::SpriteCacheFrame> decoded;
	REQUIRE(
		engine::bake::ReadSpriteCache(
			*prepared.EncodedCache, *prepared.CacheLayout, decoded, failure, 16 * 1024 * 1024
		)
	);
	CHECK(decoded[0].Rgba == first.Pixels);
	CHECK(decoded[1].Rgba == second.Pixels);
	const auto previous = prepared.Frames;
	const auto oldText = prepared.EncodedCache;
	CHECK_FALSE(PrepareGraphSourceImages(controls, {}, {}, 7, 11, true, prepared, failure));
	CHECK(prepared.Frames == previous);
	CHECK(prepared.EncodedCache == oldText);
	CHECK_FALSE(PrepareGraphSourceImages(controls, grants, {}, 7, 11, true, prepared, failure, 128));
	CHECK(prepared.Frames == previous);
	CHECK(prepared.EncodedCache == oldText);
}

TEST_CASE(
	"Saved cache decoding admits prior backing capacity before replacing its image ledger",
	"[imagegraph][image_cache]"
) {
	auto document = CachedSequence();
	EvaluationRequest request;
	std::vector<Image> frames{{1, 1, {1, 2, 3, 4}}};
	frames[0].Pixels.reserve(4096);
	const auto previous = frames;
	const auto retainedCapacity = frames[0].Pixels.capacity();
	bool enabled = false;
	std::string failure;
	HostNodeInvocation invocation{document.Nodes[0], request, {}, {}, 1024};
	CHECK_FALSE(ReadGraphSavedImageCache(invocation, {}, frames, enabled, failure));
	CHECK(failure == "prior saved image cache storage exceeds operation bounds");
	CHECK(frames == previous);
	CHECK(frames[0].Pixels.capacity() == retainedCapacity);
	CHECK_FALSE(enabled);
	invocation.MaximumOperationBytes = 16 * 1024 * 1024;
	REQUIRE(ReadGraphSavedImageCache(invocation, {}, frames, enabled, failure));
	REQUIRE(frames.size() == 2);
	CHECK(enabled);
	CHECK(frames[0].Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	CHECK(frames[1].Pixels == std::vector<uint8_t>{200, 10, 40, 0, 255, 128, 64, 255});
}
