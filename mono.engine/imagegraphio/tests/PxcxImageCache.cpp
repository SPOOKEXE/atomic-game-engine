#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_image_cache")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	constexpr std::string_view Cache = "[{\"width\":1,\"height\":1,\"buffer\":\"eJzjUbLwAwABWAC1\"}]";
	std::string Hash(std::string_view data) {
		const auto hash = engine::bake::SpriteCacheDataHash(data);
		REQUIRE(hash);
		return std::string(hash->data(), hash->size());
	}
	Json Source(std::string type = "Node_Image_Animated") {
		const std::string data(type == "Node_Image" ? Cache.substr(1, Cache.size() - 2) : Cache);
		return Json{
			{"nodes",
			 Json::array({Json{
				 {"id", "image"},
				 {"type", type},
				 {"x", 0},
				 {"y", 0},
				 {"inputs",
				  Json::array({Json{
					  {"r", {{"d", type == "Node_Image" ? Json("gone.png") : Json::array({"gone.png"})}}}
				  }})},
				 {"attri", {{"cache_use", true}, {"cache_data", data}, {"future_source", 17}}},
				 {"atomic_game_engine",
				  {{"version", 1},
				   {"future_engine", 42},
				   {"sprite_cache",
					{{"layout", "rgba8-top-down"}, {"data_hash", Hash(data)}, {"future_cache", 9}}}}},
				 {"future_node", "keep"}
			 }})},
			{"future_project", 31}
		};
	}
	engine::bake::PxcxArchive Archive(const Json &root) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = root.dump() + '\0';
		std::string failure;
		std::vector<std::byte> bytes;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	PxcxImport Import(const Json &root) {
		PxcxImport result;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(Archive(root), result, failure));
		return result;
	}
	std::string Diagnostics(const PxcxImport &imported) {
		std::string text;
		for (const auto &reason : imported.Diagnostics)
			text += reason.Message + "\n";
		return text;
	}
	Json Saved(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive source;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, source, failure));
		return Json::parse(source.GraphJson.begin(), source.GraphJson.end() - 1);
	}
}
TEST_CASE(
	"Source sprite cache metadata survives checked archive and native edits", "[imagegraphio][image_cache]"
) {
	for (const auto type : {"Node_Image", "Node_Image_Sequence", "Node_Image_Animated"}) {
		auto root = Source(type);
		auto imported = Import(root);
		const auto data = root["nodes"][0]["attri"]["cache_data"].get<std::string>();
		INFO(Diagnostics(imported));
		CAPTURE(type);
		REQUIRE(imported.Graph.Nodes[0].Type.starts_with("pc."));
		CHECK(
			imported.Graph.Nodes[0].SourceProperties ==
			std::vector<AuthoredValue>{
				{"cache_use", true},
				{"cache_data", data},
				{"composer_sprite_cache_layout", std::string("rgba8-top-down")},
				{"composer_sprite_cache_data_hash", Hash(data)}
			}
		);
		Diagnostic diagnostic;
		Document native;
		REQUIRE(Read(Write(imported.Graph), native, diagnostic) == Status::Ok);
		CHECK(native == imported.Graph);
		std::vector<std::byte> output;
		REQUIRE(WritePxcxProjection(imported, native, {}, output, diagnostic));
		CHECK(output == imported.Source.OriginalBytes);
		native.Nodes[0].Position.X = 42;
		std::get<bool>(native.Nodes[0].SourceProperties[0].Data) = false;
		const bool saved = WritePxcxProjection(imported, native, {}, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(saved);
		const auto written = Saved(output);
		CHECK(written["nodes"][0]["attri"]["cache_use"] == false);
		CHECK(written["nodes"][0]["attri"]["cache_data"] == root["nodes"][0]["attri"]["cache_data"]);
		CHECK(written["nodes"][0]["attri"]["future_source"] == 17);
		CHECK(written["nodes"][0]["atomic_game_engine"] == root["nodes"][0]["atomic_game_engine"]);
		CHECK(written["future_project"] == 31);
		CHECK(written["nodes"][0]["future_node"] == "keep");
		const auto replay = Import(written);
		CHECK(replay.Graph == native);
	}
}
TEST_CASE(
	"Cache layout identity refuses stale edits without changing archive output", "[imagegraphio][image_cache]"
) {
	const auto imported = Import(Source());
	auto native = imported.Graph;
	native.Nodes[0].Position.X = 1;
	Diagnostic diagnostic;
	std::vector<std::byte> output;
	REQUIRE(WritePxcxProjection(imported, native, {}, output, diagnostic));
	const auto previous = output;
	native.Nodes[0].SourceProperties[1].Data = std::string(Cache) + " ";
	CHECK_FALSE(WritePxcxProjection(imported, native, {}, output, diagnostic));
	CHECK(output == previous);
	native.Nodes[0].SourceProperties[3].Data =
		Hash(std::get<std::string>(native.Nodes[0].SourceProperties[1].Data));
	REQUIRE(WritePxcxProjection(imported, native, {}, output, diagnostic));
	CHECK(Saved(output)["nodes"][0]["atomic_game_engine"]["sprite_cache"]["future_cache"] == 9);
	native.Nodes[0].SourceProperties[2].Data = std::string("guessed-device-format");
	const auto accepted = output;
	CHECK_FALSE(WritePxcxProjection(imported, native, {}, output, diagnostic));
	CHECK(output == accepted);
}
TEST_CASE(
	"Foreign malformed or excessive active caches retain the original source as opaque",
	"[imagegraphio][image_cache]"
) {
	for (const auto mutate : {0, 1, 2, 3}) {
		auto source = Source();
		if (mutate == 0) source["nodes"][0]["atomic_game_engine"]["version"] = 2;
		if (mutate == 1)
			source["nodes"][0]["atomic_game_engine"]["sprite_cache"]["data_hash"] = std::string(64, '0');
		if (mutate == 2) source["nodes"][0]["attri"]["cache_use"] = 1;
		if (mutate == 3)
			source["nodes"][0]["attri"]["cache_data"] =
				std::string(engine::bake::SpriteCacheLimits::MaximumEncodedBytes + 1, 'x');
		auto imported = Import(source);
		CHECK(imported.Graph.Nodes[0].Type.starts_with("pxcx.opaque/"));
		std::vector<std::byte> output;
		Diagnostic diagnostic;
		REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, output, diagnostic));
		CHECK(output == imported.Source.OriginalBytes);
	}
}
TEST_CASE(
	"Clearing known cache annotation fields preserves future native envelope members",
	"[imagegraphio][image_cache]"
) {
	auto imported = Import(Source());
	auto native = imported.Graph;
	native.Nodes[0].SourceProperties.resize(2);
	native.Nodes[0].Position.Y = 17;
	Diagnostic diagnostic;
	std::vector<std::byte> output;
	const bool saved = WritePxcxProjection(imported, native, {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	auto source = Saved(output);
	CHECK(source["nodes"][0]["atomic_game_engine"]["sprite_cache"] == Json{{"future_cache", 9}});
	CHECK(source["nodes"][0]["atomic_game_engine"]["future_engine"] == 42);
	CHECK(Import(source).Graph == native);
}

TEST_CASE(
	"Source image path arrays retain text leaves through real static input edits",
	"[imagegraphio][image_cache]"
) {
	for (const auto type : {"Node_Image_Sequence", "Node_Image_Animated"}) {
		CAPTURE(type);
		auto root = Source(type);
		root["nodes"][0]["inputs"][0]["r"]["d"] = Json::array({"original-one.png", "original-two.webp"});
		root["nodes"][0]["inputs"][0]["future_input"] = 73;
		const auto imported = Import(root);
		INFO(Diagnostics(imported));
		const bool animated = std::string_view(type) == "Node_Image_Animated";
		REQUIRE(imported.Graph.Nodes[0].Type == (animated ? "pc.image_animated" : "pc.image_sequence"));
		auto native = imported.Graph;
		const auto input = std::find_if(
			native.Nodes[0].Values.begin(), native.Nodes[0].Values.end(), [&](const auto &value) {
				return value.Port == (animated ? "path" : "paths");
			}
		);
		REQUIRE(input != native.Nodes[0].Values.end());
		REQUIRE(std::holds_alternative<ArrayValue>(input->Data));
		CHECK(
			std::get<ArrayValue>(input->Data) ==
			ArrayValue{ValueType::Text, {std::string("original-one.png"), std::string("original-two.webp")}}
		);
		Diagnostic diagnostic;
		Document savedNative;
		REQUIRE(Read(Write(native), savedNative, diagnostic) == Status::Ok);
		CHECK(savedNative == native);
		const auto constructorKey =
			std::find_if(native.Keyframes.begin(), native.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == native.Nodes[0].Id && key.Port == input->Port;
			});
		REQUIRE(constructorKey != native.Keyframes.end());
		REQUIRE(std::count_if(native.Keyframes.begin(), native.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == native.Nodes[0].Id && key.Port == input->Port;
				}) == 1);
		REQUIRE(constructorKey->SourceKeyId == "pxc:compact");
		REQUIRE(constructorKey->Kind == KeyframeKind::Normal);
		REQUIRE(constructorKey->Tick == 0);
		REQUIRE(constructorKey->Subframe == 0);
		REQUIRE_FALSE(constructorKey->NegativeFrame);
		REQUIRE(constructorKey->Interpolation == "source");
		REQUIRE_FALSE(constructorKey->SourceDriver);
		REQUIRE_FALSE(constructorKey->SineDriver);
		REQUIRE(constructorKey->Data == input->Data);
		// An admitted static edit keeps its physical constructor key and literal together.
		input->Data =
			ArrayValue{ValueType::Text, {std::string("edited-one.tiff"), std::string("edited-two.png")}};
		constructorKey->Data = input->Data;
		std::vector<std::byte> output;
		const bool saved = WritePxcxProjection(imported, native, {}, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(saved);
		const auto written = Saved(output);
		CHECK(
			written["nodes"][0]["inputs"][0]["r"]["d"] == Json::array({"edited-one.tiff", "edited-two.png"})
		);
		CHECK(written["nodes"][0]["inputs"][0]["future_input"] == 73);
		CHECK(written["nodes"][0]["atomic_game_engine"] == root["nodes"][0]["atomic_game_engine"]);
		const auto reimported = Import(written);
		INFO("Authored: " << Write(native));
		INFO("Reimported: " << Write(reimported.Graph));
		CHECK(reimported.Graph == native);
		const auto previous = output;
		input->Data = ArrayValue{ValueType::Integer, {int64_t{7}}};
		CHECK_FALSE(WritePxcxProjection(imported, native, {}, output, diagnostic));
		CHECK(output == previous);
	}
}
