#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcxappend")
TEST_DEPENDS("engine.imagegraphio.pxcximport")

namespace {
	using Json = nlohmann::ordered_json;
	using namespace engine::imagegraphio;
	Json ValueJson(Json data) {
		return {{"r", {{"d", std::move(data)}}}};
	}
	Json WireJson(std::string node) {
		return {{"from_node", std::move(node)}, {"from_index", 0}, {"from_tag", 0}};
	}
	Json SolidNode(std::string id) {
		Json inputs = Json::array(
			{Json{{"r", {{"d", {1, 1}}}}, {"attri", {{"use_project_dimension", 1}}}},
			 ValueJson(0xFF402010u),
			 ValueJson(false),
			 Json{{"r", {{"d", -4}}}, {"attri", {{"mask_alpha_only", false}}}},
			 ValueJson(false),
			 ValueJson(-4)}
		);
		return {{"id", std::move(id)}, {"type", "Node_Solid"}, {"x", 0}, {"y", 0}, {"inputs", inputs}};
	}
	Json GroupNode(std::string id, std::string source, std::string input, std::string output) {
		return {
			{"id", std::move(id)},
			{"type", "Node_Group"},
			{"x", 0},
			{"y", 0},
			{"inputs", Json::array({WireJson(std::move(source))})},
			{"attri",
			 {{"custom_input_list", Json::array({input})}, {"custom_output_list", Json::array({output})}}}
		};
	}
	Json GroupInputNode(std::string id, std::string group) {
		return {
			{"id", std::move(id)},
			{"type", "Node_Group_Input"},
			{"x", 0},
			{"y", 0},
			{"group", std::move(group)},
			{"inputs", Json::array({ValueJson(0), ValueJson(Json::array({0, 1})), ValueJson(4)})}
		};
	}
	Json GroupOutputNode(std::string id, std::string group, std::string source) {
		return {
			{"id", std::move(id)},
			{"type", "Node_Group_Output"},
			{"x", 0},
			{"y", 0},
			{"group", std::move(group)},
			{"inputs", Json::array({WireJson(std::move(source))})}
		};
	}

	engine::bake::PxcxArchive Archive(std::string graph) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = std::move(graph);
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	engine::bake::PxcxArchive Destination() {
		return Archive(
			R"JSON({"globals":{"future_global":"keep"},"attri":{"future_project":"keep"},"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":17}],"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":3}}],"attri":{"future_node":"keep"},"future_node_field":{"id":"number","text":"number"}},{"id":"opaque","type":"Future_Node","x":3,"y":4,"inputs":[{"r":{"d":5},"from_node":"number","from_index":0,"future_wire":{"keep":8}}],"future_opaque":{"keep":true}}]})JSON"
		);
	}
	engine::bake::PxcxArchive Incoming() {
		return Archive(
			R"JSON({"timelines":{"type":"Folder","contents":[{"name":"incoming","future":2}]},"nodes":[{"id":"number","type":"Node_Number_Simple","x":10,"y":20,"inputs":[{"r":{"d":9},"future_input":{"keep":"untouched"}}],"future_node_field":{"id":"number","text":"number"}},{"id":"math","type":"Node_Math","x":30,"y":40,"inputs":[{"r":{"d":0}},{"r":{"d":-4},"from_node":"number","from_index":0,"future_wire":{"keep":true}},{"r":{"d":90}},{"r":{"d":1}},{"r":{"d":false}},{"r":{"d":0}},{"r":{"d":[0,1]}},{"r":{"d":[0,1]}},{"r":{"d":false}}],"future_node_field":{"id":"number","text":"number"}},{"id":"future","type":"Vendor_Future","x":50,"y":60,"inputs":[{"r":{"d":"foreign"},"future_text":"number","future_key_tail":{"keys":[{"id":"number","tail":{"keep":1}}]}}],"future_node_field":{"id":"number","text":"number"}}]})JSON"
		);
	}
	Json Graph(const engine::imagegraphio::PxcxImport &project) {
		return Json::parse(project.Source.GraphJson.begin(), project.Source.GraphJson.end() - 1);
	}
}

TEST_CASE(
	"PXC append remaps nested groups and inline memberships while keeping source metadata",
	"[imagegraphio][pxcx_append][groups]"
) {
	Json destination = {
		{"global_node",
		 {{"inputs",
		   Json::array(
			   {Json{{"global_name", "speed"}, {"global_type", 1}, {"global_disp", 0}, {"r", {{"d", 3}}}}}
		   )}}},
		{"nodes",
		 Json::array(
			 {SolidNode("dest-root"),
			  GroupNode("dest-group", "dest-root", "dest-input", "dest-output"),
			  GroupInputNode("dest-input", "dest-group"),
			  GroupOutputNode("dest-output", "dest-group", "dest-input"),
			  GroupNode("base", "dest-root", "base-dest-input", "base-dest-output"),
			  GroupInputNode("base-dest-input", "base"),
			  GroupOutputNode("base-dest-output", "base", "base-dest-input")}
		 )}
	};
	Json incoming = {
		{"global_node",
		 {{"inputs",
		   Json::array(
			   {Json{{"global_name", "speed"}, {"global_type", 1}, {"global_disp", 0}, {"r", {{"d", 99}}}}}
		   )}}},
		{"metadata",
		 {{"description", "saved group"},
		  {"author", "fixture"},
		  {"contact", "none"},
		  {"alias", "group-alias"},
		  {"file_id", 73},
		  {"tags", Json::array({"Animation", "Utility"})},
		  {"version", 121092},
		  {"isDefault", false},
		  {"preview_frames", 8},
		  {"deprecated", false},
		  {"aut_id", 42}}},
		{"nodes",
		 Json::array(
			 {SolidNode("root"),
			  GroupNode("base", "root", "base-input", "base-output"),
			  GroupInputNode("base-input", "base"),
			  GroupOutputNode("base-output", "base", "base-input"),
			  GroupNode("parent", "root", "parent-input", "parent-output"),
			  GroupInputNode("parent-input", "parent"),
			  GroupOutputNode("parent-output", "parent", "parent-input"),
			  Json{
				  {"id", "nested-instance"},
				  {"type", "Node_Group"},
				  {"x", 5},
				  {"y", 6},
				  {"group", "parent"},
				  {"instanceBase", "base"},
				  {"inputs", Json::array({WireJson("parent-input")})},
				  {"attri",
				   {{"custom_input_list", Json::array({"instance-input"})},
					{"custom_output_list", Json::array({"instance-output"})}}}
			  },
			  GroupInputNode("instance-input", "nested-instance"),
			  GroupOutputNode("instance-output", "nested-instance", "instance-input"),
			  Json{
				  {"id", "root-instance"},
				  {"type", "Node_Group"},
				  {"x", 7},
				  {"y", 8},
				  {"instanceBase", "base"},
				  {"inputs", Json::array({WireJson("root")})},
				  {"attri",
				   {{"custom_input_list", Json::array({"root-instance-input"})},
					{"custom_output_list", Json::array({"root-instance-output"})}}}
			  },
			  GroupInputNode("root-instance-input", "root-instance"),
			  GroupOutputNode("root-instance-output", "root-instance", "root-instance-input"),
			  Json{
				  {"id", "cache"},
				  {"type", "Node_Cache"},
				  {"x", 10},
				  {"y", 11},
				  {"inputs", Json::array()},
				  {"attri", {{"serialize", true}, {"cache_group", Json::array({"root", "base"})}}},
				  {"cache", "[]"}
			  },
			  Json{
				  {"id", "simulation"},
				  {"type", "Node_VerletSim_Inline"},
				  {"x", 12},
				  {"y", 13},
				  {"inputs",
				   Json::array(
					   {ValueJson(3),
						ValueJson(Json::array({0.25, 0.75})),
						Json{{"r", {{"d", {2, 2}}}}, {"attri", {{"use_project_dimension", 0}}}},
						ValueJson(5)}
				   )},
				  {"attri", {{"members", Json::array({"root", "base", "cache"})}, {"shape", 1}}}
			  },
			  Json{
				  {"id", "external"},
				  {"type", "Vendor_Future"},
				  {"x", 14},
				  {"y", 15},
				  {"inputs", Json::array({ValueJson(-4)})},
				  {"inspectInputs",
				   Json::array({Json{
					   {"r", {{"d", 17}}},
					   {"from_node", "dest-root"},
					   {"from_index", 0},
					   {"from_tag", 9},
					   {"future", {{"keep", true}}}
				   }})},
				  {"future_field", {{"node_id", "root"}, {"text", "root"}}}
			  },
			  Json{
				  {"id", "canvas-context"},
				  {"type", "Node_Canvas_Group"},
				  {"x", 16},
				  {"y", 17},
				  {"inputs", Json::array()},
				  {"attri",
				   {{"custom_input_list", Json::array({"base-input"})},
					{"custom_output_list", Json::array({"base-output"})}}},
				  {"future_field", {{"keep", "source"}}}
			  }}
		 )}
	};
	for (auto &node : incoming["nodes"]) {
		if (node["id"] != "external") continue;
		for (size_t index = 1; index < 6; ++index)
			node["inspectInputs"].push_back(WireJson("root"));
		node["outputs"] = Json::array({WireJson("root")});
		node["outputMeta"] = Json::array({WireJson("root")});
		node["inputMeta"] = Json::array({WireJson("root")});
		node["tool"] = "root";
	}
	for (auto &node : incoming["nodes"])
		if (node["id"] == "root" || node["id"] == "base" || node["id"] == "cache")
			node["ictx"] = "simulation";
	const auto destinationArchive = Archive(destination.dump());
	const auto incomingArchive = Archive(incoming.dump());
	PxcxAppendOptions options;
	options.Namespace = "placed";
	options.Context = "dest-group";
	PxcxAppendResult result;
	engine::imagegraph::Diagnostic diagnostic;
	const bool appended = AppendPxcxProject(destinationArchive, incomingArchive, options, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(appended);
	CHECK(result.Nodes.size() == incoming["nodes"].size());
	const auto graph = Graph(result.Project);
	CHECK(graph["global_node"] == destination["global_node"]);
	const auto appendedGroup =
		std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
			return node.value("id", "") == "placed/base";
		});
	REQUIRE(appendedGroup != graph["nodes"].end());
	CHECK((*appendedGroup)["group"] == "dest-group");
	const auto nested = std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
		return node.value("id", "") == "placed/nested-instance";
	});
	REQUIRE(nested != graph["nodes"].end());
	CHECK((*nested)["group"] == "placed/parent");
	CHECK((*nested)["instanceBase"] == "placed/base");
	const auto canvas = std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
		return node.value("id", "") == "placed/canvas-context";
	});
	REQUIRE(canvas != graph["nodes"].end());
	CHECK((*canvas)["type"] == "Node_Canvas_Group");
	CHECK((*canvas)["group"] == "dest-group");
	CHECK((*canvas)["attri"]["custom_input_list"] == Json::array({"placed/base-input"}));
	CHECK((*canvas)["attri"]["custom_output_list"] == Json::array({"placed/base-output"}));
	CHECK((*canvas)["future_field"]["keep"] == "source");
	const auto rootInstance =
		std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
			return node.value("id", "") == "placed/root-instance";
		});
	REQUIRE(rootInstance != graph["nodes"].end());
	CHECK((*rootInstance)["instanceBase"] == "base");
	const auto collection = std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
		return node.value("id", "") == "placed/simulation";
	});
	REQUIRE(collection != graph["nodes"].end());
	CHECK((*collection)["attri"]["members"] == Json::array({"placed/root", "placed/base", "placed/cache"}));
	CHECK((*collection)["attri"]["shape"] == 1);
	const auto cache = std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
		return node.value("id", "") == "placed/cache";
	});
	REQUIRE(cache != graph["nodes"].end());
	CHECK((*cache)["attri"]["cache_group"] == Json::array({"root", "base"}));
	CHECK(result.MetadataJson == incoming["metadata"].dump());
	const auto external = std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
		return node.value("id", "") == "placed/external";
	});
	REQUIRE(external != graph["nodes"].end());
	CHECK_FALSE((*external)["inspectInputs"][0].contains("from_node"));
	CHECK_FALSE((*external)["inspectInputs"][0].contains("from_index"));
	CHECK_FALSE((*external)["inspectInputs"][0].contains("from_tag"));
	CHECK((*external)["inspectInputs"][0]["r"]["d"] == 17);
	CHECK((*external)["inspectInputs"][0]["future"]["keep"] == true);
	for (const size_t index : {1, 2, 4})
		CHECK((*external)["inspectInputs"][index]["from_node"] == "placed/root");
	for (const size_t index : {3, 5})
		CHECK((*external)["inspectInputs"][index]["from_node"] == "root");
	for (const auto *field : {"outputs", "outputMeta", "inputMeta"})
		CHECK((*external)[field][0]["from_node"] == "root");
	CHECK((*external)["tool"] == "root");
	CHECK((*external)["future_field"]["node_id"] == "root");
	CHECK((*external)["future_field"]["text"] == "root");
	REQUIRE_FALSE(result.Project.Graph.ProjectGlobalNodeId.empty());
	const auto global = std::find_if(
		result.Project.Graph.Nodes.begin(), result.Project.Graph.Nodes.end(), [&](const auto &node) {
			return node.Id == result.Project.Graph.ProjectGlobalNodeId;
		}
	);
	REQUIRE(global != result.Project.Graph.Nodes.end());
	CHECK(global->Type == "pc.global_scope");
}

TEST_CASE(
	"PXC append preserves destination ownership and remaps incoming source records",
	"[imagegraphio][pxcx_append]"
) {
	const auto destination = Destination();
	const auto incoming = Incoming();
	PxcxAppendOptions options;
	options.Namespace = "placed";
	options.Offset = {5, -2};
	PxcxAppendResult result;
	engine::imagegraph::Diagnostic diagnostic;
	const bool appended = AppendPxcxProject(destination, incoming, options, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(appended);
	REQUIRE(result.Nodes.size() == 3);
	CHECK(result.Nodes[0] == (PxcxAppendedNode{"number", "placed/number", true}));
	CHECK(result.Nodes[1] == (PxcxAppendedNode{"math", "placed/math", true}));
	CHECK(result.Nodes[2] == (PxcxAppendedNode{"future", "placed/future", true}));
	const auto graph = Graph(result.Project);
	REQUIRE(graph["nodes"].size() == 5);
	CHECK(graph["nodes"][0]["id"] == "number");
	CHECK(graph["nodes"][1]["id"] == "opaque");
	CHECK(graph["nodes"][2]["id"] == "placed/number");
	CHECK(graph["nodes"][3]["id"] == "placed/math");
	CHECK(graph["nodes"][2]["x"] == 15);
	CHECK(graph["nodes"][2]["y"] == 18);
	CHECK(graph["nodes"][2]["future_node_field"]["id"] == "number");
	CHECK(graph["nodes"][2]["future_node_field"]["text"] == "number");
	CHECK(graph["nodes"][2]["inputs"][0]["future_input"]["keep"] == "untouched");
	CHECK_FALSE(graph["nodes"][3]["inputs"][0].contains("from_node"));
	CHECK(graph["nodes"][3]["inputs"][1]["from_node"] == "placed/number");
	CHECK(graph["nodes"][3]["inputs"][1]["future_wire"]["keep"] == true);
	CHECK(graph["nodes"][4]["type"] == "Vendor_Future");
	CHECK(graph["nodes"][4]["id"] == "placed/future");
	CHECK(graph["nodes"][4]["inputs"][0]["future_text"] == "number");
	CHECK(graph["nodes"][4]["inputs"][0]["future_key_tail"]["keys"][0]["tail"]["keep"] == 1);
	CHECK(graph["nodes"][4]["future_node_field"]["id"] == "number");
	CHECK(graph["globals"]["future_global"] == "keep");
	CHECK(graph["attri"]["future_project"] == "keep");
	CHECK(graph["aRegion"][0]["future"] == 17);
	CHECK(graph["nodes"][0]["attri"]["future_node"] == "keep");
	CHECK(graph["timelines"]["contents"].size() == 1);

	const auto native = std::find_if(
		result.Project.Graph.Nodes.begin(), result.Project.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "placed/number";
		}
	);
	REQUIRE(native != result.Project.Graph.Nodes.end());
	CHECK(native->Type == "pc.number_simple");
	CHECK(
		std::any_of(
			result.Project.Graph.Links.begin(), result.Project.Graph.Links.end(), [](const auto &link) {
				return link.FromNode == "placed/number" && link.ToNode == "placed/math";
			}
		)
	);
	auto evaluated = result.Project.Graph;
	std::erase_if(evaluated.Nodes, [](const auto &node) {
		return !engine::imagegraph::FindSchema(node.Type);
	});
	std::erase_if(evaluated.Links, [&](const auto &link) {
		return std::none_of(
				   evaluated.Nodes.begin(),
				   evaluated.Nodes.end(),
				   [&](const auto &node) { return node.Id == link.FromNode; }
			   ) ||
			   std::none_of(evaluated.Nodes.begin(), evaluated.Nodes.end(), [&](const auto &node) {
				   return node.Id == link.ToNode;
			   });
	});
	evaluated.Outputs = {{"selected", "placed/number", "number"}};
	engine::imagegraph::Plan plan;
	engine::imagegraph::Diagnostic evaluationDiagnostic;
	const auto compiled = engine::imagegraph::Compile(evaluated, plan, evaluationDiagnostic);
	INFO(evaluationDiagnostic.Message);
	REQUIRE(compiled == engine::imagegraph::Status::Ok);
	engine::imagegraph::EvaluatedValue output;
	REQUIRE(
		engine::imagegraph::EvaluateValue(evaluated, plan, "selected", {}, output, evaluationDiagnostic) ==
		engine::imagegraph::Status::Ok
	);
	const auto *number = std::get_if<double>(&output.Data);
	REQUIRE(number != nullptr);
	CHECK(*number == 9);
}

TEST_CASE(
	"PXC append refuses namespace collisions and stale or overbudget archives atomically",
	"[imagegraphio][pxcx_append]"
) {
	const auto destination = Archive(
		R"JSON({"nodes":[{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":4}}]},{"id":"used/number","type":"Future_Node","x":0,"y":0,"inputs":[]}]})JSON"
	);
	const auto incoming = Incoming();
	PxcxAppendOptions options;
	options.Namespace = "used";
	PxcxAppendResult result;
	result.Nodes.push_back({"prior", "prior/id", true});
	result.Project.Source.OriginalBytes = {std::byte{0x2a}};
	result.MetadataJson = "prior metadata";
	const auto priorNodes = result.Nodes;
	const auto priorBytes = result.Project.Source.OriginalBytes;
	const auto priorMetadata = result.MetadataJson;
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(AppendPxcxProject(destination, incoming, options, result, diagnostic));
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);

	options.Namespace = "fresh";
	options.MaximumOperationBytes = 1;
	CHECK_FALSE(AppendPxcxProject(Destination(), incoming, options, result, diagnostic));
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);
	CHECK(diagnostic.Code == engine::imagegraph::Status::LimitExceeded);

	options.MaximumOperationBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
	auto stale = incoming;
	stale.GraphJson[stale.GraphJson.find("\"number\"")] = 'x';
	CHECK_FALSE(AppendPxcxProject(Destination(), stale, options, result, diagnostic));
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);

	options.MaximumOperationBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
	options.Namespace = "bad/name";
	CHECK_FALSE(AppendPxcxProject(Destination(), incoming, options, result, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);
	options.Namespace = "fresh";
	options.Context = "missing-group";
	CHECK_FALSE(AppendPxcxProject(Destination(), incoming, options, result, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);

	Json oversized = Json::parse(incoming.GraphJson.begin(), incoming.GraphJson.end() - 1);
	oversized["nodes"][0]["future_records"] = Json::array();
	for (size_t index = 0; index < 12000; ++index)
		oversized["nodes"][0]["future_records"].push_back(Json::object());
	auto oversizedArchive = Archive(oversized.dump());
	options.Context.clear();
	options.MaximumOperationBytes = 4 * 1024 * 1024;
	CHECK_FALSE(AppendPxcxProject(Destination(), oversizedArchive, options, result, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::LimitExceeded);
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);

	options.MaximumOperationBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
	auto overflowing = Json::parse(incoming.GraphJson.begin(), incoming.GraphJson.end() - 1);
	overflowing["nodes"][0]["x"] = std::numeric_limits<double>::max();
	auto overflowingArchive = Archive(overflowing.dump());
	options.Offset.X = std::numeric_limits<double>::max();
	CHECK_FALSE(AppendPxcxProject(Destination(), overflowingArchive, options, result, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(result.Nodes == priorNodes);
	CHECK(result.Project.Source.OriginalBytes == priorBytes);
	CHECK(result.MetadataJson == priorMetadata);
}
