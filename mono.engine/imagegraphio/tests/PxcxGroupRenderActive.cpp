#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_group_render_active")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;

	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}};
	}
	Json Wire(std::string id) {
		return {{"from_node", std::move(id)}, {"from_index", 0}, {"from_tag", 0}};
	}
	Json Record(std::string id, std::string type, Json inputs = Json::array()) {
		return {
			{"id", std::move(id)},
			{"type", std::move(type)},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", {{"retained", "verbatim value"}}}
		};
	}
	Json Project() {
		auto group = Record("group", "Node_Group");
		group["attri"] = {{"custom_input_list", Json::array()}, {"custom_output_list", {"output"}}};
		auto number = Record("number", "Node_Number_Simple", Json::array({Fixed(2.5)}));
		number["group"] = "group";
		auto output = Record("output", "Node_Group_Output", Json::array({Wire("number")}));
		output["group"] = "group";
		auto sink = Record("sink", "Node_Project_Output", Json::array({Wire("group"), Json::object()}));
		return {
			{"attributes", {{"surface_dimension", {2, 2}}}},
			{"future_project", {{"opaque", Json::array({1, "unchanged", false})}}},
			{"nodes", Json::array({group, number, output, sink})}
		};
	}
	engine::bake::PxcxArchive Archive(const Json &project) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		INFO(failure);
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	PxcxImport Import(const Json &project) {
		const auto source = Archive(project);
		PxcxImport imported;
		std::string failure;
		INFO(failure);
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		CHECK(imported.Source.GraphJson == source.GraphJson);
		return imported;
	}
	PxcxImport Reopen(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive source;
		PxcxImport imported;
		std::string failure;
		INFO(failure);
		REQUIRE(engine::bake::ReadPxcx(bytes, source, failure));
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		return imported;
	}
	Group &FindGroup(Document &document, std::string_view id) {
		const auto group =
			std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &value) {
				return value.Id == id;
			});
		REQUIRE(group != document.Groups.end());
		return *group;
	}
	void CheckNativeRoundtrip(const Document &document) {
		Document restored;
		Diagnostic diagnostic;
		const auto status = Read(Write(document), restored, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(restored == document);
	}
	Json SavedJson(const PxcxImport &imported) {
		return Json::parse(imported.Source.GraphJson.begin(), imported.Source.GraphJson.end() - 1);
	}
}

TEST_CASE(
	"Source group render flag retains explicit false true and absent states through no-op saves",
	"[imagegraphio][groups][render_active]"
) {
	for (int state : {-1, 0, 1}) {
		auto project = Project();
		if (state >= 0) project["nodes"][0]["render"] = state != 0;
		auto imported = Import(project);
		CHECK(FindGroup(imported.Graph, "group").RenderActive == (state != 0));
		CheckNativeRoundtrip(imported.Graph);
		Diagnostic diagnostic;
		std::vector<std::byte> bytes;
		const bool accepted = WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(accepted);
		CHECK(bytes == imported.Source.OriginalBytes);
	}
}

TEST_CASE(
	"Source group rendering edits survive PXC reopen without changing foreign fields",
	"[imagegraphio][groups][render_active]"
) {
	for (bool initiallyActive : {false, true}) {
		auto project = Project();
		project["nodes"][0]["render"] = initiallyActive;
		auto imported = Import(project);
		const auto originalBytes = imported.Source.OriginalBytes;
		auto authored = imported.Graph;
		FindGroup(authored, "group").RenderActive = !initiallyActive;
		Diagnostic diagnostic;
		std::vector<std::byte> bytes;
		const bool accepted = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(accepted);
		auto reopened = Reopen(bytes);
		CHECK(FindGroup(reopened.Graph, "group").RenderActive == !initiallyActive);
		CheckNativeRoundtrip(reopened.Graph);
		project["nodes"][0]["render"] = !initiallyActive;
		CHECK(SavedJson(reopened) == project);
		CHECK(imported.Source.OriginalBytes == originalBytes);
		CHECK(FindGroup(imported.Graph, "group").RenderActive == initiallyActive);
		FindGroup(reopened.Graph, "group").RenderActive = initiallyActive;
		REQUIRE(WritePxcxProjection(Reopen(bytes), reopened.Graph, {}, bytes, diagnostic));
		auto reverted = Reopen(bytes);
		CHECK(FindGroup(reverted.Graph, "group").RenderActive == initiallyActive);
	}
}

TEST_CASE(
	"Source group rendering edits do not inherit the base instance scheduling flag",
	"[imagegraphio][groups][render_active][instance]"
) {
	auto project = Project();
	project["nodes"][0]["render"] = false;
	auto copy = Record("copy", "Node_Group");
	copy["render"] = true;
	copy["instanceBase"] = "group";
	copy["attri"] = {{"custom_input_list", Json::array()}, {"custom_output_list", {"copy-output"}}};
	auto number = Record("copy-number", "Node_Number_Simple", Json::array({Fixed(7.5)}));
	number["group"] = "copy";
	auto output = Record("copy-output", "Node_Group_Output", Json::array({Wire("copy-number")}));
	output["group"] = "copy";
	project["nodes"].push_back(copy);
	project["nodes"].push_back(number);
	project["nodes"].push_back(output);
	auto imported = Import(project);
	CHECK_FALSE(FindGroup(imported.Graph, "group").RenderActive);
	CHECK(FindGroup(imported.Graph, "copy").RenderActive);
	auto authored = imported.Graph;
	FindGroup(authored, "copy").RenderActive = false;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	auto reopened = Reopen(bytes);
	CHECK_FALSE(FindGroup(reopened.Graph, "group").RenderActive);
	CHECK_FALSE(FindGroup(reopened.Graph, "copy").RenderActive);
	CheckNativeRoundtrip(reopened.Graph);
	CHECK_FALSE(SavedJson(reopened)["nodes"][0]["render"].get<bool>());
	CHECK_FALSE(SavedJson(reopened)["nodes"][4]["render"].get<bool>());
}

TEST_CASE(
	"Malformed source group rendering fields refuse atomically",
	"[imagegraphio][groups][render_active][atomic]"
) {
	auto imported = Import(Project());
	const auto originalBytes = imported.Source.OriginalBytes;
	const auto originalGraph = imported.Graph;
	for (const Json &invalid :
		 {Json(0), Json(1), Json("false"), Json(nullptr), Json::array(), Json::object()}) {
		auto malformed = Project();
		malformed["nodes"][0]["render"] = invalid;
		std::string failure;
		CHECK_FALSE(ImportPxcxImageGraph(Archive(malformed), imported, failure));
		CHECK_FALSE(failure.empty());
		CHECK(imported.Source.OriginalBytes == originalBytes);
		CHECK(imported.Graph == originalGraph);
	}
}

TEST_CASE(
	"Unsupported projection changes cannot partially publish a group rendering edit",
	"[imagegraphio][groups][render_active][atomic]"
) {
	auto imported = Import(Project());
	auto authored = imported.Graph;
	FindGroup(authored, "group").RenderActive = false;
	authored.Outputs.front().Id = "unmapped-output-name";
	const std::vector<std::byte> sentinel{std::byte{0x42}, std::byte{0x17}};
	std::vector<std::byte> bytes = sentinel;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(imported, authored, {}, bytes, diagnostic));
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(bytes == sentinel);
	CHECK(FindGroup(imported.Graph, "group").RenderActive);
}

TEST_CASE(
	"New source groups serialize their authored disabled scheduling flag",
	"[imagegraphio][groups][render_active]"
) {
	auto imported = Import(Project());
	auto authored = imported.Graph;
	Group added;
	added.Id = "added";
	added.Name = "disabled group";
	added.RenderActive = false;
	authored.Groups.push_back(added);
	authored.FormatVersion = 11;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	auto reopened = Reopen(bytes);
	CHECK_FALSE(FindGroup(reopened.Graph, "added").RenderActive);
	CHECK(FindGroup(reopened.Graph, "added").Name == "disabled group");
	CheckNativeRoundtrip(reopened.Graph);
	const auto source = SavedJson(reopened);
	const auto record = std::find_if(source["nodes"].begin(), source["nodes"].end(), [](const auto &node) {
		return node["id"] == "added";
	});
	REQUIRE(record != source["nodes"].end());
	CHECK_FALSE((*record)["render"].template get<bool>());
}

TEST_CASE(
	"Literal source edits retain a disabled group's render flag", "[imagegraphio][groups][render_active]"
) {
	auto project = Project();
	project["nodes"][0]["render"] = false;
	auto imported = Import(project);
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"number", "value", Value{3.75}}};
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	auto reopened = Reopen(bytes);
	CHECK_FALSE(FindGroup(reopened.Graph, "group").RenderActive);
	project["nodes"][1]["inputs"][0] = Fixed(3.75);
	CHECK(SavedJson(reopened) == project);
	CheckNativeRoundtrip(reopened.Graph);
}

TEST_CASE(
	"Group and Collection pure function controls preserve no-op archives and native saves",
	"[imagegraphio][groups][pure_function]"
) {
	for (const char *sourceType : {"Node_Group", "Node_Collection"}) {
		for (int flag : {-1, 0, 1}) {
			auto project = Project();
			project["nodes"][0]["type"] = sourceType;
			if (flag >= 0) project["nodes"][0]["attri"]["pure_function"] = flag != 0;
			auto imported = Import(project);
			CHECK(FindGroup(imported.Graph, "group").PureFunction == (flag != 0));
			CheckNativeRoundtrip(imported.Graph);
			Diagnostic diagnostic;
			std::vector<std::byte> bytes;
			const bool accepted = WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(accepted);
			CHECK(bytes == imported.Source.OriginalBytes);
		}
	}
}

TEST_CASE(
	"Pure function edits preserve source controls through repeated PXC reopen",
	"[imagegraphio][groups][pure_function]"
) {
	auto project = Project();
	project["nodes"][0]["attri"]["pure_function"] = false;
	project["nodes"][0]["render"] = false;
	auto imported = Import(project);
	auto authored = imported.Graph;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	for (bool pure : {true, false, true}) {
		FindGroup(authored, "group").PureFunction = pure;
		const bool accepted = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(accepted);
		imported = Reopen(bytes);
		CHECK(FindGroup(imported.Graph, "group").PureFunction == pure);
		CHECK_FALSE(FindGroup(imported.Graph, "group").RenderActive);
		project["nodes"][0]["attri"]["pure_function"] = pure;
		CHECK(SavedJson(imported) == project);
		CheckNativeRoundtrip(imported.Graph);
		authored = imported.Graph;
	}
}

TEST_CASE(
	"Malformed group pure function controls and untracked edits refuse atomically",
	"[imagegraphio][groups][pure_function][atomic]"
) {
	auto imported = Import(Project());
	const auto original = imported;
	for (const Json &flag : {Json(0), Json(1), Json("false"), Json(nullptr), Json::array(), Json::object()}) {
		auto project = Project();
		project["nodes"][0]["attri"]["pure_function"] = flag;
		std::string failure;
		CHECK_FALSE(ImportPxcxImageGraph(Archive(project), imported, failure));
		CHECK_FALSE(failure.empty());
		CHECK(imported.Graph == original.Graph);
		CHECK(imported.Source.OriginalBytes == original.Source.OriginalBytes);
	}
	auto authored = imported.Graph;
	FindGroup(authored, "group").PureFunction = false;
	authored.FormatVersion = 11;
	authored.Outputs.front().Id = "unmapped-output-name";
	const std::vector<std::byte> sentinel{std::byte{0x42}};
	std::vector<std::byte> bytes = sentinel;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(imported, authored, {}, bytes, diagnostic));
	CHECK(bytes == sentinel);
	CHECK(FindGroup(imported.Graph, "group").PureFunction);
}

TEST_CASE(
	"Source Group instance pure function switches remain local authored attributes",
	"[imagegraphio][groups][pure_function][instance]"
) {
	auto project = Project();
	project["nodes"][0]["attri"]["pure_function"] = false;
	auto copy = Record("copy", "Node_Group");
	copy["instanceBase"] = "group";
	copy["attri"] = {
		{"custom_input_list", Json::array()}, {"custom_output_list", {"copy-output"}}, {"pure_function", true}
	};
	auto number = Record("copy-number", "Node_Number_Simple", Json::array({Fixed(7.5)}));
	number["group"] = "copy";
	auto output = Record("copy-output", "Node_Group_Output", Json::array({Wire("copy-number")}));
	output["group"] = "copy";
	project["nodes"].push_back(copy);
	project["nodes"].push_back(number);
	project["nodes"].push_back(output);
	auto imported = Import(project);
	CHECK_FALSE(FindGroup(imported.Graph, "group").PureFunction);
	CHECK(FindGroup(imported.Graph, "copy").PureFunction);
	auto authored = imported.Graph;
	FindGroup(authored, "copy").PureFunction = false;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool accepted = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	auto reopened = Reopen(bytes);
	CHECK_FALSE(FindGroup(reopened.Graph, "group").PureFunction);
	CHECK_FALSE(FindGroup(reopened.Graph, "copy").PureFunction);
	CheckNativeRoundtrip(reopened.Graph);
	CHECK_FALSE(SavedJson(reopened)["nodes"][0]["attri"]["pure_function"].get<bool>());
	CHECK_FALSE(SavedJson(reopened)["nodes"][4]["attri"]["pure_function"].get<bool>());
}
