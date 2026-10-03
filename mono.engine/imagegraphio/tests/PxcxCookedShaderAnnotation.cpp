#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_cooked_shader_annotation")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::ordered_json;
	std::string Name(char digit) {
		return std::string(64, digit) + ".ashader";
	}
	Json Source(Json envelope = nullptr) {
		Json node{
			{"id", "shader"},
			{"type", "Node_HLSL"},
			{"x", 0},
			{"y", 0},
			{"inputs",
			 Json::array(
				 {Json{{"r", {{"d", ""}}}},
				  Json{{"r", {{"d", "output.color=float4(1,0,0,1);"}}}},
				  Json::object(),
				  Json{{"r", {{"d", ""}}}},
				  Json{{"r", {{"d", ""}}}}}
			 )},
			{"attri", {{"future_source", 7}}},
			{"future_node", "keep"}
		};
		if (!envelope.is_null()) node["atomic_game_engine"] = std::move(envelope);
		return Json{{"nodes", Json::array({node})}, {"future_project", 11}};
	}
	engine::bake::PxcxArchive Archive(const Json &root) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = root.dump();
		source.GraphJson.push_back('\0');
		std::string failure;
		std::vector<std::byte> bytes;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	PxcxImport Import(const Json &root) {
		PxcxImport imported;
		std::string failure;
		const bool ok = ImportPxcxImageGraph(Archive(root), imported, failure);
		INFO(failure);
		REQUIRE(ok);
		REQUIRE(imported.Graph.Nodes[0].Type == "pc.hlsl");
		return imported;
	}
} // namespace
TEST_CASE(
	"Native HLSL annotations survive checked PXC edits and native saves", "[imagegraphio][cooked_annotation]"
) {
	auto imported = Import(Source(
		Json{{"version", 1}, {"composer_cooked_shader", Name('a')}, {"future_engine", Json{{"opaque", 29}}}}
	));
	REQUIRE(imported.Graph.FormatVersion == 9);
	REQUIRE(
		imported.Graph.Nodes[0].SourceProperties ==
		std::vector<AuthoredValue>{{"composer_cooked_shader", Name('a')}}
	);
	Document native;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), native, diagnostic) == Status::Ok);
	CHECK(native == imported.Graph);
	auto preview = native;
	preview.Outputs = {{"preview", "shader", "surface"}};
	Plan plan;
	const auto compiled = Compile(preview, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, native, {}, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
	native.Nodes[0].Position.X = 37;
	for (auto &value : native.Nodes[0].Values)
		if (value.Port == "main") value.Data = std::string("output.color=float4(0,1,0,1);");
	native.Nodes[0].SourceProperties[0].Data = Name('b');
	const bool ok = WritePxcxProjection(imported, native, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(ok);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport replay;
	REQUIRE(ImportPxcxImageGraph(archive, replay, failure));
	CHECK(replay.Graph == native);
	const Json root = Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	CHECK(root["nodes"][0]["atomic_game_engine"]["future_engine"]["opaque"] == 29);
	CHECK(root["nodes"][0]["attri"] == Json{{"future_source", 7}});
	CHECK_FALSE(root["nodes"][0]["attri"].contains("composer_cooked_shader"));
	CHECK(root["future_project"] == 11);
	CHECK(root["nodes"][0]["future_node"] == "keep");
	native.Nodes[0].SourceProperties.clear();
	REQUIRE(WritePxcxProjection(imported, native, {}, bytes, diagnostic));
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	REQUIRE(ImportPxcxImageGraph(archive, replay, failure));
	CHECK(replay.Graph.Nodes[0].SourceProperties.empty());
	const Json cleared = Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	CHECK(cleared["nodes"][0]["atomic_game_engine"]["future_engine"]["opaque"] == 29);
	CHECK_FALSE(cleared["nodes"][0]["atomic_game_engine"].contains("composer_cooked_shader"));
}
TEST_CASE(
	"Annotation insertion and incompatible annotations preserve caller "
	"outputs on refusal",
	"[imagegraphio][cooked_annotation]"
) {
	auto imported = Import(Source());
	auto desired = imported.Graph;
	desired.FormatVersion = 9;
	desired.Nodes[0].SourceProperties.push_back({"composer_cooked_shader", Name('c')});
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	const auto previous = bytes;
	for (const std::string &bad : {Name('G'), std::string("../name.ashader"), Name('a') + "extra"}) {
		desired.Nodes[0].SourceProperties[0].Data = bad;
		CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
		CHECK(bytes == previous);
	}
	desired.Nodes[0].SourceProperties[0].Data = Name('a');
	desired.Nodes[0].SourceProperties.push_back(desired.Nodes[0].SourceProperties[0]);
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == previous);
	for (const Json &bad :
		 {Json{{"version", 2}, {"composer_cooked_shader", Name('a')}},
		  Json{{"version", 1}, {"composer_cooked_shader", 3}},
		  Json{{"composer_cooked_shader", Name('a')}},
		  Json{{"version", 1}, {"composer_cooked_shader", Name('G')}},
		  Json("collision")}) {
		std::string failure;
		auto old = imported;
		CHECK_FALSE(ImportPxcxImageGraph(Archive(Source(bad)), old, failure));
		CHECK(old.Graph == imported.Graph);
		CHECK(old.Source.OriginalBytes == imported.Source.OriginalBytes);
	}
	auto wrong = Source(Json{{"version", 1}, {"composer_cooked_shader", Name('a')}});
	wrong["nodes"][0]["type"] = "Unmapped_Node";
	std::string failure;
	auto old = imported;
	CHECK_FALSE(ImportPxcxImageGraph(Archive(wrong), old, failure));
	CHECK(old.Graph == imported.Graph);
	PxcxImportOptions options;
	options.MaximumOperationBytes = 1;
	CHECK_FALSE(ImportPxcxImageGraph(
		Archive(Source(Json{{"version", 1}, {"composer_cooked_shader", Name('a')}})), old, failure, options
	));
	CHECK(old.Graph == imported.Graph);
}
