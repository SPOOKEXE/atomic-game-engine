#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/SourceFrameCacheLoading.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <utility>

TEST_SUITE_ID("engine.imagegraphio.source_frame_cache_loading")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace bake = engine::bake;
namespace {
	Node Cache(std::string_view type, std::string text) {
		Node node{"cache", std::string(type)};
		node.SourceProperties = {{"serialize", true}, {"cache", std::move(text)}};
		return node;
	}
	SourceFrameCacheLayoutObservation Observation(const Node &node) {
		const auto hash = bake::SpriteCacheDataHash(SourceFrameCacheSavedText(node));
		REQUIRE(hash);
		return {node.Id, std::string(hash->data(), hash->size()), bake::SpriteCacheLayout::Rgba8TopDown};
	}
	constexpr std::string_view Sparse =
		R"([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null,[],[null,[],{}],false,{},null])";
}

TEST_CASE("Source cache loading retains original slot extent and nested noone positions", "[imagegraphio]") {
	for (const std::string_view type : {"pc.cache", "pc.cache_array"}) {
		const auto node = Cache(type, std::string(Sparse));
		DataReplayEntry row;
		Diagnostic diagnostic;
		REQUIRE(DecodeSourceFrameCacheLoading(node, Observation(node), row, diagnostic) == Status::Ok);
		CHECK(row.SourceFrameCacheSerializedSlots == 7);
		CHECK(row.LoadedCacheData == Sparse);
		REQUIRE(row.Values.size() == 5);
		CHECK(row.Values[2].Frame == 2);
		CHECK(std::get<SurfaceValue>(row.Values[2].Data).Data.Pixels == std::vector<uint8_t>{12, 34, 56, 78});
		CHECK(row.Values[3].Frame == 4);
		CHECK(std::get<ArrayValue>(row.Values[3].Data).Items.empty());
		CHECK(row.Values[4].Frame == 5);
		const auto &nested = std::get<ArrayValue>(row.Values[4].Data);
		REQUIRE(nested.Items.size() == 3);
		CHECK(std::get<int64_t>(std::get<ElementValue>(nested.Items[0].Data)) == -4);
		CHECK(std::get<std::vector<SourceArrayItem>>(nested.Items[1].Data).empty());
		CHECK(std::get<int64_t>(std::get<ElementValue>(nested.Items[2].Data)) == -4);
	}
}

TEST_CASE("Empty and entirely absent source inventories have distinct exact lengths", "[imagegraphio]") {
	for (const auto &[text, count] :
		 {std::pair<std::string, uint64_t>{"[]", 0}, {"[null,42,true,{},\"text\"]", 5}}) {
		const auto node = Cache("pc.cache", text);
		DataReplayEntry row;
		Diagnostic diagnostic;
		REQUIRE(DecodeSourceFrameCacheLoading(node, Observation(node), row, diagnostic) == Status::Ok);
		CHECK(row.SourceFrameCacheSerializedSlots == count);
		CHECK(row.Values.size() == 2);
	}
}

TEST_CASE("Source loading refuses malformed buffers and bounded scans atomically", "[imagegraphio]") {
	DataReplayEntry sentinel;
	sentinel.NodeId = "prior";
	sentinel.LoadedCacheData = "prior exact text";
	const auto original = sentinel;
	Diagnostic diagnostic;
	for (const auto &text :
		 {std::string{"[null,{\"width\":1,\"height\":1,\"buffer\":\"bad\"}]"},
		  std::string{"[null,"},
		  std::string{"{}"}}) {
		const auto node = Cache("pc.cache", text);
		CHECK(DecodeSourceFrameCacheLoading(node, Observation(node), sentinel, diagnostic) != Status::Ok);
		CHECK(sentinel == original);
		CHECK(diagnostic.NodeId == "cache");
		CHECK(diagnostic.Port == "cache");
	}
	std::string tooMany = "[null";
	for (uint32_t index = 1; index <= bake::SurfaceCacheLimits::MaximumItems; ++index)
		tooMany += ",null";
	tooMany += ']';
	const auto excessive = Cache("pc.cache", tooMany);
	CHECK(
		DecodeSourceFrameCacheLoading(excessive, Observation(excessive), sentinel, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(sentinel == original);
	const auto valid = Cache("pc.cache", std::string(Sparse));
	CHECK(
		DecodeSourceFrameCacheLoading(valid, Observation(valid), sentinel, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(sentinel == original);
	std::string nested(70, '[');
	nested += '0';
	nested += std::string(70, ']');
	const auto deep = Cache("pc.cache", nested);
	CHECK(
		DecodeSourceFrameCacheLoading(deep, Observation(deep), sentinel, diagnostic) == Status::LimitExceeded
	);
	CHECK(sentinel == original);
}

TEST_CASE("Source cache loading cooks complete v2 inventories through native documents", "[imagegraphio]") {
	Document source;
	source.FormatVersion = 9;
	source.Nodes = {Cache("pc.cache", std::string(Sparse)), Cache("pc.cache_array", "[null,null]")};
	source.Nodes[1].Id = "array";
	std::array observations{Observation(source.Nodes[0]), Observation(source.Nodes[1])};
	DataReplayState loaded;
	Diagnostic diagnostic;
	REQUIRE(DecodeSourceFrameCachesLoading(source, observations, loaded, diagnostic) == Status::Ok);
	REQUIRE(loaded.Entries.size() == 2);
	CHECK(loaded.Entries[0].SourceFrameCacheSerializedSlots == 7);
	CHECK(loaded.Entries[1].SourceFrameCacheSerializedSlots == 2);
	Document cooked;
	const auto status = CookSourceFrameCachesLoading(source, observations, cooked, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(cooked), restored, diagnostic) == Status::Ok);
	for (size_t index = 0; index < restored.Nodes.size(); ++index) {
		DataReplayEntry row;
		REQUIRE(DecodeSourceFrameCacheReceipt(restored.Nodes[index], row, diagnostic) == Status::Ok);
		CHECK(row == loaded.Entries[index]);
	}
	const auto original = source;
	REQUIRE(CookSourceFrameCachesLoading(source, observations, source, diagnostic) == Status::Ok);
	CHECK(source == cooked);
	CHECK(original.Nodes[0].SourceProperties.size() == 2);
	const auto retained = cooked;
	CHECK(
		CookSourceFrameCachesLoading(original, observations, cooked, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(cooked == retained);
}

TEST_CASE(
	"PXC import cooks source inventories only with exact explicit layout observations", "[imagegraphio]"
) {
	using Json = nlohmann::ordered_json;
	Json project = {
		{"animator", {{"frames_total", 2}, {"playback", 0}, {"framerate", 24}}},
		{"nodes",
		 Json::array(
			 {{{"id", "cache"},
			   {"type", "Node_Cache"},
			   {"x", 0},
			   {"y", 0},
			   {"inputs", Json::array({Json::object(), {{"r", {{"d", false}}}}})},
			   {"attri", {{"serialize", true}}},
			   {"cache", std::string(Sparse)}}}
		 )}
	};
	bake::PxcxArchive packet;
	packet.MetadataNumber = 121092;
	packet.MetadataText = "1.22.10.201";
	packet.GraphJson = project.dump() + '\0';
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(bake::WritePxcx(packet, bytes, failure));
	bake::PxcxArchive archive;
	REQUIRE(bake::ReadPxcx(bytes, archive, failure));
	PxcxImport cold;
	REQUIRE(ImportPxcxImageGraph(archive, cold, failure));
	REQUIRE(cold.Graph.Nodes.size() == 1);
	REQUIRE(cold.Graph.Nodes[0].Type == "pc.cache");
	CHECK(cold.Graph.Nodes[0].SourceProperties.size() == 2);
	PxcxImportOptions options;
	options.FrameCacheLayouts = {Observation(cold.Graph.Nodes[0])};
	PxcxImport loaded;
	const bool imported = ImportPxcxImageGraph(archive, loaded, failure, options);
	INFO(failure);
	REQUIRE(imported);
	CHECK(loaded.Options.FrameCacheLayouts[0].NodeId == "cache");
	CHECK(loaded.Source.OriginalBytes == bytes);
	DataReplayEntry row;
	Diagnostic diagnostic;
	REQUIRE(DecodeSourceFrameCacheReceipt(loaded.Graph.Nodes[0], row, diagnostic) == Status::Ok);
	CHECK(row.SourceFrameCacheSerializedSlots == 7);
	CHECK(row.LoadedCacheData == Sparse);
	const auto retained = loaded.Graph;
	options.FrameCacheLayouts[0].DataHash[0] = options.FrameCacheLayouts[0].DataHash[0] == '0' ? '1' : '0';
	CHECK_FALSE(ImportPxcxImageGraph(archive, loaded, failure, options));
	CHECK(loaded.Graph == retained);
	options.FrameCacheLayouts[0].Layout = bake::SpriteCacheLayout(255);
	CHECK_FALSE(ImportPxcxImageGraph(archive, loaded, failure, options));
	CHECK(loaded.Graph == retained);
	options.FrameCacheLayouts = {Observation(cold.Graph.Nodes[0]), Observation(cold.Graph.Nodes[0])};
	CHECK_FALSE(ImportPxcxImageGraph(archive, loaded, failure, options));
	CHECK(loaded.Graph == retained);
	options.FrameCacheLayouts.resize(1);
	options.MaximumOperationBytes = 1;
	CHECK_FALSE(ImportPxcxImageGraph(archive, loaded, failure, options));
	CHECK(loaded.Graph == retained);
}
