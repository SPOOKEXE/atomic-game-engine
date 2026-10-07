#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/NoiseField.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_noise_field")
namespace {
	using Json = nlohmann::ordered_json;
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	Json NodeRecord(std::string id) {
		const auto *entry = FindCatalogueEntry("pc.fold_noise");
		REQUIRE(entry);
		return {
			{"id", std::move(id)},
			{"type", entry->SourceNode},
			{"x", 0},
			{"y", 0},
			{"inputs", std::vector<Json>(14, Json::object())},
			{"future_node", "keep"}
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
	PxcxImport Imported(const Json &root) {
		PxcxImport imported;
		std::string failure;
		const bool accepted = ImportPxcxImageGraph(Archive(root), imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.fold_noise");
		return imported;
	}
	PxcxImport Saved(const PxcxImport &source, const Document &desired) {
		Diagnostic diagnostic;
		std::vector<std::byte> bytes;
		const bool accepted = WritePxcxProjection(source, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(accepted);
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport reopened;
		REQUIRE(ImportPxcxImageGraph(archive, reopened, failure));
		CHECK(reopened.Graph == desired);
		return reopened;
	}
} // namespace
TEST_CASE(
	"PXC native noise choices survive source saves without foreign "
	"socket changes",
	"[pxcx_noise_field]"
) {
	auto record = NodeRecord("noise");
	record["atomic_game_engine"] = {
		{"version", 1}, {"future_engine", 17}, {"noise_field", {{"future_field", Json::array({1, 2})}}}
	};
	const auto source = Imported({{"nodes", Json::array({record})}});
	for (int64_t components : {1, 2, 3}) {
		Document desired = source.Graph;
		Diagnostic diagnostic;
		REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
		desired.Nodes[0].Values.push_back({"output_type", EnumValue{components}});
		const auto reopened = Saved(source, desired);
		const auto port =
			NoiseNodePort(reopened.Graph.Nodes[0], "field", PortDirection::Output, &reopened.Graph);
		REQUIRE(port);
		CHECK(port->Type == *NoiseFieldType(2, uint8_t(components)));
		const auto root = Json::parse(
			std::string_view(reopened.Source.GraphJson.data(), reopened.Source.GraphJson.size() - 1)
		);
		CHECK(root["nodes"][0]["inputs"] == record["inputs"]);
		CHECK(root["nodes"][0]["future_node"] == "keep");
		CHECK(root["nodes"][0]["atomic_game_engine"]["future_engine"] == 17);
		CHECK(root["nodes"][0]["atomic_game_engine"]["noise_field"]["future_field"] == Json::array({1, 2}));
	}
}
TEST_CASE("PXC absent native noise choice retains exact source bytes", "[pxcx_noise_field]") {
	const auto source = Imported({{"nodes", Json::array({NodeRecord("noise")})}});
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, source.Graph, {}, bytes, diagnostic));
	CHECK(bytes == source.Source.OriginalBytes);
	auto desired = source.Graph;
	desired.Nodes[0].Position.X = 12;
	const auto reopened = Saved(source, desired);
	CHECK(
		std::none_of(
			reopened.Graph.Nodes[0].Values.begin(),
			reopened.Graph.Nodes[0].Values.end(),
			[](const auto &value) { return value.Port == "output_type"; }
		)
	);
	const auto root =
		Json::parse(std::string_view(reopened.Source.GraphJson.data(), reopened.Source.GraphJson.size() - 1));
	CHECK_FALSE(root["nodes"][0].contains("atomic_game_engine"));
}
TEST_CASE("PXC native noise selector and malformed annotations refuse atomically", "[pxcx_noise_field]") {
	const auto source = Imported({{"nodes", Json::array({NodeRecord("noise")})}});
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	for (Value invalid : {Value{EnumValue{0}}, Value{EnumValue{4}}, Value{int64_t{2}}}) {
		auto desired = source.Graph;
		desired.Nodes[0].Values.push_back({"output_type", invalid});
		auto bytes = sentinel;
		Diagnostic diagnostic;
		CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
		CHECK(bytes == sentinel);
	}
	for (Json field :
		 {Json{nullptr},
		  Json{2},
		  Json{{"output_type", 0}},
		  Json{{"output_type", 4}},
		  Json{{"output_type", 2.5}},
		  Json{{"override_instance", 1}}}) {
		auto record = NodeRecord("noise");
		record["atomic_game_engine"] = {{"version", 1}, {"noise_field", field}};
		auto destination = source;
		std::string failure;
		CHECK_FALSE(ImportPxcxImageGraph(Archive({{"nodes", Json::array({record})}}), destination, failure));
		CHECK(destination.Graph == source.Graph);
		CHECK(destination.Source.OriginalBytes == source.Source.OriginalBytes);
	}
}
TEST_CASE(
	"PXC native noise annotations require the supported version and mapped generator", "[pxcx_noise_field]"
) {
	const auto source = Imported({{"nodes", Json::array({NodeRecord("noise")})}});
	for (Json envelope :
		 {Json{{"noise_field", {{"output_type", 2}}}},
		  Json{{"version", 2}, {"noise_field", {{"output_type", 2}}}},
		  Json{{"version", "1"}, {"noise_field", {{"output_type", 2}}}}}) {
		auto record = NodeRecord("noise");
		record["atomic_game_engine"] = envelope;
		auto destination = source;
		std::string failure;
		CHECK_FALSE(ImportPxcxImageGraph(Archive({{"nodes", Json::array({record})}}), destination, failure));
		CHECK(destination.Graph == source.Graph);
		CHECK(destination.Source.OriginalBytes == source.Source.OriginalBytes);
	}
	Json record = {
		{"id", "number"},
		{"type", "Node_Number_Simple"},
		{"x", 0},
		{"y", 0},
		{"inputs", Json::array({Json{{"r", {{"d", 2}}}}})},
		{"atomic_game_engine", {{"version", 1}, {"noise_field", {{"output_type", 2}}}}}
	};
	auto destination = source;
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(Archive({{"nodes", Json::array({record})}}), destination, failure));
	CHECK(destination.Graph == source.Graph);
	CHECK(destination.Source.OriginalBytes == source.Source.OriginalBytes);
}
TEST_CASE(
	"PXC removing a native noise selector restores scalar default and retains unknown annotation",
	"[pxcx_noise_field]"
) {
	auto record = NodeRecord("noise");
	record["atomic_game_engine"] = {
		{"version", 1}, {"future_engine", 17}, {"noise_field", {{"output_type", 3}, {"future_field", "keep"}}}
	};
	const auto source = Imported({{"nodes", Json::array({record})}});
	auto desired = source.Graph;
	std::erase_if(desired.Nodes[0].Values, [](const auto &value) { return value.Port == "output_type"; });
	const auto reopened = Saved(source, desired);
	CHECK(RasterNoiseComponents(reopened.Graph.Nodes[0], &reopened.Graph) == 1);
	const auto root =
		Json::parse(std::string_view(reopened.Source.GraphJson.data(), reopened.Source.GraphJson.size() - 1));
	CHECK_FALSE(root["nodes"][0]["atomic_game_engine"]["noise_field"].contains("output_type"));
	CHECK(root["nodes"][0]["atomic_game_engine"]["noise_field"]["future_field"] == "keep");
	CHECK(root["nodes"][0]["atomic_game_engine"]["future_engine"] == 17);
}
TEST_CASE(
	"PXC native noise instance selectors retain inheritance and explicit "
	"overrides",
	"[pxcx_noise_field]"
) {
	auto base = NodeRecord("base"), instance = NodeRecord("instance");
	base["atomic_game_engine"] = {{"version", 1}, {"noise_field", {{"output_type", 3}}}};
	instance["instanceBase"] = "base";
	instance["atomic_game_engine"] = {{"version", 1}, {"noise_field", {{"output_type", 2}}}};
	const auto source = Imported({{"nodes", Json::array({base, instance})}});
	REQUIRE(source.Graph.Nodes.size() == 2);
	CHECK(RasterNoiseComponents(source.Graph.Nodes[1], &source.Graph) == 3);
	auto desired = source.Graph;
	desired.Nodes[1].InstanceOverrides.push_back("output_type");
	const auto overridden = Saved(source, desired);
	CHECK(RasterNoiseComponents(overridden.Graph.Nodes[1], &overridden.Graph) == 2);
	desired = overridden.Graph;
	desired.Nodes[1].InstanceOverrides.clear();
	const auto inherited = Saved(overridden, desired);
	CHECK(RasterNoiseComponents(inherited.Graph.Nodes[1], &inherited.Graph) == 3);
}
