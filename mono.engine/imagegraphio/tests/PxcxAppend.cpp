#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
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
	Json Collection(std::string id, std::string parent = {}, std::string path = {}) {
		Json attributes = {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}};
		if (!path.empty()) attributes["path"] = std::move(path);
		Json node = {
			{"id", std::move(id)},
			{"type", "Node_Collection"},
			{"x", 0},
			{"y", 0},
			{"inputs", Json::array()},
			{"attri", std::move(attributes)}
		};
		if (!parent.empty()) node["group"] = std::move(parent);
		return node;
	}
	PxcxAppendResult AppendIncoming(Json incoming) {
		PxcxAppendOptions options;
		options.Namespace = "postload";
		PxcxAppendResult result;
		engine::imagegraph::Diagnostic diagnostic;
		const bool appended =
			AppendPxcxProject(Destination(), Archive(incoming.dump()), options, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(appended);
		return result;
	}
	Json ArchiveGraph(const engine::bake::PxcxArchive &archive) {
		return Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
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
	std::vector<std::pair<std::string, std::string>> retiredCommonWriters;
	for (const auto &owner : evaluated.SourceCommonOwners)
		if (owner.NativeOwnerKind == engine::imagegraph::SourceCommonNativeOwnerKind::Node &&
			std::none_of(evaluated.Nodes.begin(), evaluated.Nodes.end(), [&](const auto &node) {
				return node.Id == owner.NativeOwnerId;
			}))
			retiredCommonWriters.emplace_back(owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
	if (!retiredCommonWriters.empty()) {
		REQUIRE(evaluated.SourceAnimators);
		const auto retiredCommonWriter = [&](std::string_view ownerId, std::string_view port) {
			return std::any_of(
				retiredCommonWriters.begin(), retiredCommonWriters.end(), [&](const auto &writer) {
					return writer.first == ownerId && writer.second == port;
				}
			);
		};
		std::erase_if(evaluated.SourceAnimators->Detached, [&](const auto &row) {
			return retiredCommonWriter(row.OwnerId, row.Id);
		});
		std::erase_if(evaluated.SourceAnimators->DetachedValues, [&](const auto &row) {
			return retiredCommonWriter(row.NodeId, row.Port);
		});
	}
	std::erase_if(evaluated.SourceCommonOwners, [&](const auto &owner) {
		return owner.NativeOwnerKind == engine::imagegraph::SourceCommonNativeOwnerKind::Node &&
			   std::none_of(evaluated.Nodes.begin(), evaluated.Nodes.end(), [&](const auto &node) {
				   return node.Id == owner.NativeOwnerId;
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

TEST_CASE(
	"PXC post-load prepares only top-level collection managers with source defaults and overrides",
	"[imagegraphio][pxcx_append][post_load]"
) {
	Json metadata = {
		{"description", "saved description"},
		{"author", nullptr},
		{"contact", "saved contact"},
		{"file_id", nullptr},
		{"tags", Json::array({"Animation", "Utility"})},
		{"version", 121092},
		{"isDefault", true},
		{"preview_frames", nullptr},
		{"deprecated", true},
		{"aut_id", 71},
		{"future_metadata", "ignored"}
	};
	Json incoming = {
		{"metadata", metadata},
		{"nodes",
		 Json::array(
			 {Collection("top"),
			  Collection("nested", "top"),
			  Json{
				  {"id", "ordinary"},
				  {"type", "Node_Number_Simple"},
				  {"x", 0},
				  {"y", 0},
				  {"inputs", Json::array({ValueJson(4)})}
			  },
			  Json{
				  {"id", "inline"},
				  {"type", "Node_Collection_Inline"},
				  {"x", 0},
				  {"y", 0},
				  {"inputs", Json::array()},
				  {"attri", {{"members", Json::array({"ordinary"})}}}
			  }}
		 )}
	};
	auto appended = AppendIncoming(std::move(incoming));
	const auto originalGraph = appended.Project.Source.GraphJson;
	const auto originalBytes = appended.Project.Source.OriginalBytes;
	PxcxAppendPostLoad loaded;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(PreparePxcxAppendPostLoad(appended, "ignored-multiple-root-path.pxcx", loaded, diagnostic));
	INFO(diagnostic.Message);
	REQUIRE(loaded.Collections.size() == 1);
	CHECK(loaded.Collections.front().NodeId == "postload/top");
	CHECK_FALSE(loaded.Source.has_value());
	CHECK(appended.Project.Source.GraphJson == originalGraph);
	CHECK(appended.Project.Source.OriginalBytes == originalBytes);
	const auto manager = Json::parse(loaded.Collections.front().MetadataJson);
	CHECK(manager["description"] == "saved description");
	CHECK(manager["author"] == "");
	CHECK(manager["contact"] == "saved contact");
	CHECK(manager["alias"] == "");
	CHECK(manager["file_id"] == 0);
	CHECK(manager["tags"] == Json::array({"Animation", "Utility"}));
	CHECK(manager["version"] == 121092);
	CHECK(manager["isDefault"] == true);
	CHECK(manager["preview_frames"] == 1);
	CHECK(manager["deprecated"] == true);
	CHECK(manager["aut_id"] == 71);
	CHECK_FALSE(manager.contains("future_metadata"));
}

TEST_CASE(
	"PXC post-load changes only a single collection source path and creates no node metadata fields",
	"[imagegraphio][pxcx_append][post_load][path]"
) {
	Json incoming = {
		{"metadata", {{"description", "manager only"}, {"file_id", 19}}},
		{"nodes", Json::array({Collection("only", {}, "old-project.pxcx")})}
	};
	auto appended = AppendIncoming(std::move(incoming));
	const auto priorGraph = appended.Project.Source.GraphJson;
	const auto priorBytes = appended.Project.Source.OriginalBytes;
	PxcxAppendPostLoad loaded;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(PreparePxcxAppendPostLoad(appended, "new-project.pxcx", loaded, diagnostic));
	INFO(diagnostic.Message);
	REQUIRE(loaded.Collections.size() == 1);
	REQUIRE(loaded.Source.has_value());
	CHECK(appended.Project.Source.GraphJson == priorGraph);
	CHECK(appended.Project.Source.OriginalBytes == priorBytes);
	auto expectedGraph = Json::parse(priorGraph.begin(), priorGraph.end() - 1);
	auto graphCollection =
		std::find_if(expectedGraph["nodes"].begin(), expectedGraph["nodes"].end(), [](const auto &node) {
			return node.value("id", "") == "postload/only";
		});
	REQUIRE(graphCollection != expectedGraph["nodes"].end());
	(*graphCollection)["attri"]["path"] = "new-project.pxcx";
	const auto graph = ArchiveGraph(*loaded.Source);
	CHECK(graph == expectedGraph);
	const auto collection = std::find_if(graph["nodes"].begin(), graph["nodes"].end(), [](const auto &node) {
		return node.value("id", "") == "postload/only";
	});
	REQUIRE(collection != graph["nodes"].end());
	CHECK((*collection)["attri"]["path"] == "new-project.pxcx");
	CHECK_FALSE((*collection)["attri"].contains("description"));
	CHECK_FALSE((*collection)["attri"].contains("file_id"));
	const auto manager = Json::parse(loaded.Collections.front().MetadataJson);
	CHECK(manager["description"] == "manager only");
	CHECK(manager["file_id"] == 19);
}

TEST_CASE(
	"PXC post-load defaults an empty manager and skips source path for multiple top-level nodes",
	"[imagegraphio][pxcx_append][post_load][defaults]"
) {
	Json incoming = {
		{"nodes",
		 Json::array(
			 {Collection("only"),
			  Json{
				  {"id", "ordinary"},
				  {"type", "Node_Number_Simple"},
				  {"x", 0},
				  {"y", 0},
				  {"inputs", Json::array({ValueJson(2)})}
			  }}
		 )}
	};
	auto appended = AppendIncoming(std::move(incoming));
	PxcxAppendPostLoad loaded;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(PreparePxcxAppendPostLoad(appended, "should-not-be-written.pxcx", loaded, diagnostic));
	INFO(diagnostic.Message);
	REQUIRE(loaded.Collections.size() == 1);
	CHECK_FALSE(loaded.Source.has_value());
	const auto manager = Json::parse(loaded.Collections.front().MetadataJson);
	CHECK(manager["description"] == "");
	CHECK(manager["author"] == "");
	CHECK(manager["contact"] == "");
	CHECK(manager["alias"] == "");
	CHECK(manager["file_id"] == 0);
	CHECK(manager["tags"].empty());
	CHECK(manager["version"] == 121092);
	CHECK(manager["isDefault"] == false);
	CHECK(manager["preview_frames"] == 1);
	CHECK(manager["deprecated"] == false);
	CHECK(manager["aut_id"] == 0);
}

TEST_CASE(
	"PXC post-load recognizes the pinned collection subclass whitelist in source order",
	"[imagegraphio][pxcx_append][post_load][collections]"
) {
	const std::vector<std::string> types = {
		"Node_Group",
		"Node_Collection",
		"Node_Canvas_Group",
		"Node_DynaSurf",
		"Node_Feedback",
		"Node_Iterate",
		"Node_Iterate_Each",
		"Node_Iterate_Filter",
		"Node_Iterate_Sort",
		"Node_Iterator",
		"Node_Pixel_Builder",
		"Node_Smoke_Group",
		"Node_Strand_Group",
		"Node_VFX_Group"
	};
	Json sourceNodes = Json::array();
	PxcxAppendResult appended;
	for (size_t index = 0; index < types.size(); ++index) {
		const auto id = "group-" + std::to_string(index);
		sourceNodes.push_back(
			{{"id", "fresh/" + id}, {"type", types[index]}, {"x", 0}, {"y", 0}, {"inputs", Json::array()}}
		);
		appended.Nodes.push_back({id, "fresh/" + id, true});
	}
	sourceNodes.push_back(
		{{"id", "fresh/inline"},
		 {"type", "Node_Collection_Inline"},
		 {"x", 0},
		 {"y", 0},
		 {"inputs", Json::array()}}
	);
	appended.Nodes.push_back({"inline", "fresh/inline", true});
	sourceNodes.push_back(
		{{"id", "fresh/ordinary"},
		 {"type", "Node_Number_Simple"},
		 {"x", 0},
		 {"y", 0},
		 {"inputs", Json::array({ValueJson(1)})}}
	);
	appended.Nodes.push_back({"ordinary", "fresh/ordinary", true});
	appended.Project.Source = Archive(Json{{"nodes", std::move(sourceNodes)}}.dump());

	PxcxAppendPostLoad loaded;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(PreparePxcxAppendPostLoad(appended, "multiple-roots.pxcx", loaded, diagnostic));
	INFO(diagnostic.Message);
	REQUIRE(loaded.Collections.size() == types.size());
	for (size_t index = 0; index < types.size(); ++index) {
		CHECK(loaded.Collections[index].NodeId == "fresh/group-" + std::to_string(index));
		CHECK(Json::parse(loaded.Collections[index].MetadataJson)["version"] == 121092);
	}
	CHECK_FALSE(loaded.Source.has_value());
}

TEST_CASE(
	"PXC collection metadata defaults cover nested pinned collection types and ignore source metadata",
	"[imagegraphio][pxcx_append][collection_metadata]"
) {
	const std::vector<std::string> types = {
		"Node_Group",
		"Node_Collection",
		"Node_Canvas_Group",
		"Node_DynaSurf",
		"Node_Feedback",
		"Node_Iterate",
		"Node_Iterate_Each",
		"Node_Iterate_Filter",
		"Node_Iterate_Sort",
		"Node_Iterator",
		"Node_Pixel_Builder",
		"Node_Smoke_Group",
		"Node_Strand_Group",
		"Node_VFX_Group"
	};
	Json nodes = Json::array();
	for (size_t index = 0; index < types.size(); ++index)
		nodes.push_back(
			{{"id", "group-" + std::to_string(index)},
			 {"type", types[index]},
			 {"x", 0},
			 {"y", 0},
			 {"inputs", Json::array()}}
		);
	nodes[0]["attri"] = {{"future_payload", Collection("attri-lookalike")}};
	nodes.push_back(Collection("nested-collection", "group-1"));
	Json graph = {
		{"metadata", {{"description", "root override"}, {"author", "root author"}}},
		{"nodes", std::move(nodes)},
		{"future_payload", {{"nodes", Json::array({Collection("payload-lookalike")})}}}
	};
	auto archive = Archive(graph.dump());
	std::vector<PxcxCollectionMetadata> result;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(PreparePxcxCollectionMetadata(archive, result, diagnostic));
	INFO(diagnostic.Message);
	REQUIRE(result.size() == types.size() + 1);
	for (size_t index = 0; index < result.size(); ++index) {
		const auto metadata = Json::parse(result[index].MetadataJson);
		CHECK(metadata["description"] == "");
		CHECK(metadata["author"] == "");
		CHECK(metadata["version"] == 121092);
	}
	CHECK(result.back().NodeId == "nested-collection");
	CHECK(std::none_of(result.begin(), result.end(), [](const auto &entry) {
		return entry.NodeId == "attri-lookalike" || entry.NodeId == "payload-lookalike";
	}));
}

TEST_CASE(
	"PXC collection metadata refuses untracked archives and low budgets atomically",
	"[imagegraphio][pxcx_append][collection_metadata][atomic]"
) {
	auto archive = Archive(Json{{"nodes", Json::array({Collection("only")})}}.dump());
	std::vector<PxcxCollectionMetadata> result{{"prior", "unchanged"}};
	const auto prior = result;
	engine::imagegraph::Diagnostic diagnostic;
	archive.GraphJson.insert(0, " ");
	CHECK_FALSE(PreparePxcxCollectionMetadata(archive, result, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(result == prior);
	archive.GraphJson.erase(0, 1);
	CHECK_FALSE(PreparePxcxCollectionMetadata(archive, result, diagnostic, 1));
	CHECK(diagnostic.Code == engine::imagegraph::Status::LimitExceeded);
	CHECK(result == prior);
}

TEST_CASE(
	"PXC Collection save emits descendants postorder with relative positions and source references",
	"[imagegraphio][pxcx_append][collection_save]"
) {
	Json root = Collection("selected", "outside");
	root["x"] = 100;
	root["y"] = 200;
	root["attri"]["future_root"] = {{"id", "keep-root"}, {"type", "Node_Collection"}};
	Json firstChild = Collection("child-a", "selected");
	firstChild["x"] = 110;
	firstChild["y"] = 220;
	firstChild["attri"]["future_node"] = "keep-child";
	Json grandchild = {
		{"id", "grandchild"},
		{"type", "Node_Feedback"},
		{"x", 111},
		{"y", 222},
		{"group", "child-a"},
		{"inputs", Json::array({WireJson("external-source")})},
		{"instanceBase", "external-base"},
		{"ictx", "external-context"},
		{"tool", "external-tool"},
		{"future_node", {{"identity", "external-reference"}}}
	};
	Json secondChild = Collection("child-b", "selected");
	secondChild["x"] = 120;
	secondChild["y"] = 230;
	Json external = {
		{"id", "external-source"},
		{"type", "Node_Number_Simple"},
		{"x", 5},
		{"y", 7},
		{"inputs", Json::array({ValueJson(9)})}
	};
	Json graph = {
		{"global_node",
		 {{"inputs",
		   Json::array(
			   {Json{{"global_name", "speed"}, {"global_type", 1}, {"global_disp", 0}, {"r", {{"d", 3}}}}}
		   )}}},
		{"attri", {{"description", "project metadata must not leak"}}},
		{"timelines", {{"contents", Json::array({"project timeline"})}}},
		{"nodes", Json::array({root, firstChild, grandchild, secondChild, external})}
	};
	auto archive = Archive(graph.dump());
	PxcxCollectionSave result;
	engine::imagegraph::Diagnostic diagnostic;
	const std::string manager =
		R"JSON({"description":"collection description","author":null,"file_id":41,"version":3,"versionStr":"old","future_manager":"drop"})JSON";
	REQUIRE(PreparePxcxCollectionSave(archive, "selected", manager, result, diagnostic));
	INFO(diagnostic.Message);
	const auto saved = Json::parse(result.GraphJson);
	CHECK(saved.size() == 3);
	CHECK(saved["version"] == 121092);
	CHECK(saved["versionStr"] == "1.21.10.203");
	CHECK_FALSE(saved.contains("global_node"));
	CHECK_FALSE(saved.contains("attri"));
	CHECK_FALSE(saved.contains("timelines"));
	REQUIRE(saved["nodes"].size() == 4);
	for (const auto &node : saved["nodes"])
		CHECK(node["version"] == 121092);
	CHECK(saved["nodes"][0]["id"] == "grandchild");
	CHECK(saved["nodes"][1]["id"] == "child-a");
	CHECK(saved["nodes"][2]["id"] == "child-b");
	CHECK(saved["nodes"][3]["id"] == "selected");
	CHECK(saved["nodes"][0]["x"] == 11);
	CHECK(saved["nodes"][0]["y"] == 22);
	CHECK(saved["nodes"][1]["x"] == 10);
	CHECK(saved["nodes"][1]["y"] == 20);
	CHECK(saved["nodes"][2]["x"] == 20);
	CHECK(saved["nodes"][2]["y"] == 30);
	CHECK(saved["nodes"][3]["x"] == 0);
	CHECK(saved["nodes"][3]["y"] == 0);
	CHECK(saved["nodes"][3]["group"] == -4);
	CHECK(saved["nodes"][0]["group"] == "child-a");
	CHECK(saved["nodes"][0]["inputs"][0]["from_node"] == "external-source");
	CHECK(saved["nodes"][0]["instanceBase"] == "external-base");
	CHECK(saved["nodes"][0]["ictx"] == "external-context");
	CHECK(saved["nodes"][0]["tool"] == "external-tool");
	CHECK(saved["nodes"][0]["future_node"]["identity"] == "external-reference");
	CHECK(saved["nodes"][1]["attri"]["future_node"] == "keep-child");
	CHECK(saved["nodes"][3]["attri"]["future_root"] == root["attri"]["future_root"]);
	REQUIRE(result.MetadataJson.has_value());
	const auto metadata = Json::parse(*result.MetadataJson);
	CHECK(metadata.size() == 12);
	CHECK(metadata["description"] == "collection description");
	CHECK(metadata["author"] == "");
	CHECK(metadata["file_id"] == 41);
	CHECK(metadata["version"] == 121092);
	CHECK(metadata["versionStr"] == "1.21.10.203");
	CHECK_FALSE(metadata.contains("future_manager"));
}

TEST_CASE(
	"PXC Collection save scales only expanded source input animators by the project frame count",
	"[imagegraphio][pxcx_append][collection_save][animation]"
) {
	const auto keys = [](double before, double after, std::string value) {
		return Json::array(
			{Json::array({Json::array({0, before}), value}),
			 Json::array({Json::array({1, after}), value + "-after"})}
		);
	};
	Json node = Collection("animated");
	node["inputs"] = Json::array(
		{Json{{"r", keys(5.5, -2.75, "normal")}},
		 Json{{"r", {{"d", Json::array({2, 4, 8})}}}},
		 Json{{"r", keys(10, 20, "kind-one")}, {"animators", Json::array({keys(2.5, 20, "axis")})}}}
	);
	const Json driver = {
		{"type", 77}, {"r", Json::array({Json::array({Json::array({0, 9}), "opaque-driver"})})}
	};
	for (auto &key : node["inputs"][0]["r"]) {
		key.push_back(Json::array({0.25, 0.75}));
		key.push_back(Json::array({0.5, 0.5}));
		key.push_back("ease-in");
		key.push_back("ease-out");
		key.push_back(true);
		key.push_back(driver);
		key.push_back(17);
	}
	node["inspectInputs"] = Json::array(
		{Json{{"r", keys(1, 2, "inspect-zero")}},
		 Json{{"r", keys(3, 4, "inspect-one")}},
		 Json{{"r", keys(5, 6, "inspect-two")}},
		 Json{{"r", keys(7, 8, "output-trigger")}},
		 Json{{"r", keys(9, 10, "inspect-four")}, {"animators", Json::array({keys(4, 8, "inspect-axis")})}},
		 Json{{"r", keys(11, 12, "surplus-inspector")}}}
	);
	node["outputs"] = Json::array({Json{{"r", keys(2, 4, "opaque-output")}}});
	node["outputMeta"] = {{"r", keys(3, 6, "opaque-output-meta")}};
	node["attri"]["animator_lookalike"] = keys(4, 8, "opaque-attribute");
	auto archive = Archive(Json{{"animator", {{"frames_total", 11}}}, {"nodes", Json::array({node})}}.dump());
	PxcxCollectionSave result;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(PreparePxcxCollectionSave(archive, "animated", std::nullopt, result, diagnostic));
	INFO(diagnostic.Message);
	const auto saved = Json::parse(result.GraphJson);
	const auto &exported = saved["nodes"][0];
	CHECK(exported["inputs"][0]["r"][0][0][0] == 0);
	CHECK(exported["inputs"][0]["r"][0][0][1].get<double>() == Catch::Approx(0.55));
	CHECK(exported["inputs"][0]["r"][0][1] == "normal");
	CHECK(exported["inputs"][0]["r"][1][0][0] == 1);
	CHECK(exported["inputs"][0]["r"][1][0][1].get<double>() == Catch::Approx(-0.275));
	for (size_t index = 0; index < node["inputs"][0]["r"].size(); ++index)
		for (size_t field = 1; field < node["inputs"][0]["r"][index].size(); ++field)
			CHECK(exported["inputs"][0]["r"][index][field] == node["inputs"][0]["r"][index][field]);
	CHECK(exported["inputs"][1]["r"]["d"] == Json::array({2, 4, 8}));
	CHECK(exported["inputs"][2]["r"][0][0][1].get<double>() == Catch::Approx(1.0));
	CHECK(exported["inputs"][2]["animators"][0][0][0][1].get<double>() == Catch::Approx(0.25));
	CHECK(exported["inspectInputs"][0]["r"][0][0][1].get<double>() == Catch::Approx(0.1));
	CHECK(exported["inspectInputs"][1]["r"][0][0][1].get<double>() == Catch::Approx(0.3));
	CHECK(exported["inspectInputs"][2]["r"][0][0][1].get<double>() == Catch::Approx(0.5));
	CHECK(exported["inspectInputs"][4]["r"][0][0][1].get<double>() == Catch::Approx(0.9));
	CHECK(exported["inspectInputs"][4]["animators"][0][0][0][1].get<double>() == Catch::Approx(0.4));
	CHECK(exported["inspectInputs"][3]["r"] == node["inspectInputs"][3]["r"]);
	CHECK(exported["inspectInputs"][5]["r"] == node["inspectInputs"][5]["r"]);
	CHECK(exported["outputs"] == node["outputs"]);
	CHECK(exported["outputMeta"] == node["outputMeta"]);
	CHECK(exported["attri"] == node["attri"]);
}

TEST_CASE(
	"PXC Collection save compacts eligible single expanded keys and preserves opaque tails",
	"[imagegraphio][pxcx_append][collection_save][animation]"
) {
	const auto expanded = [](Json marker, Json value, bool driverObject = false) {
		Json key = Json::array(
			{std::move(marker),
			 std::move(value),
			 Json::array({0.25, 0.75}),
			 Json::array({0.5, 0.5}),
			 "ease-in",
			 "ease-out",
			 true,
			 driverObject ? Json{{"type", 77}} : Json(0),
			 17}
		);
		return Json::array({std::move(key)});
	};
	const auto source = [](std::string id, std::string type, Json data) {
		return Json{
			{"id", std::move(id)},
			{"type", std::move(type)},
			{"x", 0},
			{"y", 0},
			{"group", "root"},
			{"inputs", Json::array({Json{{"r", std::move(data)}}})}
		};
	};
	Json root = Collection("root");
	root["inputs"].push_back(Json{{"r", expanded(Json::array({0, 16}), true)}});
	root["attri"]["custom_input_list"] = Json::array({"group-input"});
	Json vector2 = source("vector", "Node_Vector2", expanded(Json::array({0, 5}), 2.5));
	Json boolean = source("boolean", "Node_Trigger_Bool", expanded(Json::array({1, 7}), true));
	Json driven = source("driven", "Node_Vector2", expanded(Json::array({1, 8}), 3.5, true));
	Json trigger = source("trigger", "Node_Trigger", expanded(Json::array({0, 9}), true));
	Json opaque = source("opaque", "Node_Vector2", expanded(Json::array({0, 18}), 4.5));
	opaque["inputs"][0]["r"][0].push_back("future-key-field");
	opaque["inputs"].push_back(Json{{"r", expanded(Json::array({0, 19, 0, 99}), 5.5)}});
	for (size_t index = 1; index < 99; ++index)
		vector2["inputs"].push_back(ValueJson(0));
	vector2["inputs"].push_back(Json{{"r", expanded(Json::array({0, 17}), "unknown-slot")}});
	Json any = source("any", "Node_Tunnel_In", expanded(Json::array({0, 18}), "name"));
	any["inputs"].push_back(Json{{"r", expanded(Json::array({0, 19}), "runtime-value")}});
	Json axes = SolidNode("axes");
	axes["group"] = "root";
	axes["inputs"][0]["sep_axis"] = true;
	axes["inputs"][0]["r"] = expanded(Json::array({0, 5}), Json::array({2, 4}));
	axes["inputs"][0]["animators"] =
		Json::array({expanded(Json::array({0, 6}), 2), expanded(Json::array({1, 8, 0}), 4, true)});
	Json groupInput = GroupInputNode("group-input", "root");
	groupInput["inputs"][2] = ValueJson(19);
	Json inspectors = Collection("inspectors", "root");
	inspectors["inspectInputs"] = Json::array(
		{Json{{"r", expanded(Json::array({0, 10}), "inspect-action-zero")}},
		 Json{{"r", expanded(Json::array({0, 11}), "inspect-action-one")}},
		 Json{{"r", expanded(Json::array({0, 12}), true)}},
		 Json{{"r", expanded(Json::array({0, 13}), "output-trigger")}},
		 Json{{"r", expanded(Json::array({1, 14}), 14.0)}},
		 Json{{"r", expanded(Json::array({0, 15}), "surplus")}}}
	);
	inspectors["outputs"] = Json::array({Json{{"r", expanded(Json::array({0, 16}), "output")}}});
	inspectors["future_input"] = {{"r", expanded(Json::array({0, 17}), "future")}};
	Json graph = {
		{"animator", {{"frames_total", 21}}},
		{"nodes",
		 Json::array({root, vector2, boolean, driven, trigger, inspectors, opaque, groupInput, axes, any})}
	};
	const auto archive = Archive(graph.dump());
	const auto originalBytes = archive.OriginalBytes;
	PxcxCollectionSave result;
	engine::imagegraph::Diagnostic diagnostic;
	const bool prepared = PreparePxcxCollectionSave(archive, "root", std::nullopt, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(prepared);
	CHECK(archive.OriginalBytes == originalBytes);
	const auto saved = Json::parse(result.GraphJson);
	const auto findNode = [&](std::string_view id) -> const Json & {
		const auto found = std::find_if(saved["nodes"].begin(), saved["nodes"].end(), [&](const Json &node) {
			return node["id"] == id;
		});
		REQUIRE(found != saved["nodes"].end());
		return *found;
	};
	CHECK((findNode("vector")["inputs"][0]["r"] == Json{{"d", 2.5}}));
	CHECK((findNode("boolean")["inputs"][0]["r"] == Json{{"d", true}}));
	CHECK(findNode("driven")["inputs"][0]["r"][0][0][1].get<double>() == Catch::Approx(0.4));
	CHECK(findNode("driven")["inputs"][0]["r"][0][7] == driven["inputs"][0]["r"][0][7]);
	CHECK(findNode("trigger")["inputs"][0]["r"][0][0][1].get<double>() == Catch::Approx(0.45));
	const auto &savedInspect = findNode("inspectors")["inspectInputs"];
	CHECK((savedInspect[0]["r"] == Json{{"d", "inspect-action-zero"}}));
	CHECK((savedInspect[1]["r"] == Json{{"d", "inspect-action-one"}}));
	CHECK(savedInspect[2]["r"][0][0][1].get<double>() == Catch::Approx(0.6));
	CHECK(savedInspect[3] == inspectors["inspectInputs"][3]);
	CHECK((savedInspect[4]["r"] == Json{{"d", 14.0}}));
	CHECK(savedInspect[5] == inspectors["inspectInputs"][5]);
	CHECK(findNode("inspectors")["outputs"] == inspectors["outputs"]);
	CHECK(findNode("inspectors")["future_input"] == inspectors["future_input"]);
	CHECK(findNode("opaque")["inputs"][0]["r"][0][0][1].get<double>() == Catch::Approx(0.9));
	CHECK(findNode("opaque")["inputs"][0]["r"][0][9] == "future-key-field");
	CHECK(findNode("opaque")["inputs"][1]["r"][0][0][1].get<double>() == Catch::Approx(0.95));
	CHECK(findNode("opaque")["inputs"][1]["r"][0][0][3] == 99);
	CHECK(findNode("vector")["inputs"][99]["r"][0][1] == "unknown-slot");
	CHECK(findNode("vector")["inputs"][99]["r"][0][0][1].get<double>() == Catch::Approx(0.85));
	CHECK(findNode("any")["inputs"][1]["r"][0][0][1].get<double>() == Catch::Approx(0.95));
	CHECK(findNode("group-input")["inputs"] == groupInput["inputs"]);
	CHECK(findNode("root")["inputs"][0]["r"][0][0][1].get<double>() == Catch::Approx(0.8));
	CHECK(findNode("root")["inputs"][0]["r"][0][1] == true);
	const auto &savedAxes = findNode("axes")["inputs"][0];
	CHECK(savedAxes["sep_axis"] == true);
	CHECK((savedAxes["r"] == Json{{"d", Json::array({2, 4})}}));
	CHECK((savedAxes["animators"][0] == Json{{"d", 2}}));
	CHECK(savedAxes["animators"][1][0][0][1].get<double>() == Catch::Approx(0.4));
	CHECK(savedAxes["animators"][1][0][7] == axes["inputs"][0]["animators"][1][0][7]);
}

TEST_CASE(
	"PXC Collection save permits one-frame and missing-frame projects when expanded keys compact",
	"[imagegraphio][pxcx_append][collection_save][animation]"
) {
	for (const auto frameCount : {std::optional<int>{1}, std::optional<int>{}}) {
		Json root = Collection("root");
		Json child = SolidNode("axes");
		child["group"] = "root";
		child["inputs"][0]["sep_axis"] = true;
		child["inputs"][0]["r"] = Json::array({Json::array({Json::array({1, 6, 0}), Json::array({2, 4})})});
		child["inputs"][0]["animators"] = Json::array(
			{Json::array({Json::array({Json::array({0, 3}), 2})}),
			 Json::array({Json::array({Json::array({1, 6, 0}), 4})})}
		);
		Json graph = {{"nodes", Json::array({root, child})}};
		if (frameCount) graph["animator"] = {{"frames_total", *frameCount}};
		const auto archive = Archive(graph.dump());
		PxcxCollectionSave result;
		engine::imagegraph::Diagnostic diagnostic;
		CAPTURE(frameCount);
		const bool prepared = PreparePxcxCollectionSave(archive, "root", std::nullopt, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(prepared);
		const auto saved = Json::parse(result.GraphJson);
		CHECK((saved["nodes"][0]["inputs"][0]["r"] == Json{{"d", Json::array({2, 4})}}));
		CHECK((saved["nodes"][0]["inputs"][0]["animators"][0] == Json{{"d", 2}}));
		CHECK((saved["nodes"][0]["inputs"][0]["animators"][1] == Json{{"d", 4}}));
	}
}

TEST_CASE(
	"PXC Collection save still needs multiple frames for keys that cannot compact",
	"[imagegraphio][pxcx_append][collection_save][animation][atomic]"
) {
	Json root = Collection("root");
	Json key = Json::array({Json::array({0, 3}), 3.0});
	std::string type = "Node_Vector2";
	SECTION("a source Trigger stays expanded") {
		type = "Node_Trigger";
		key[1] = true;
	}
	SECTION("a driver stays expanded") {
		key = Json::array({Json::array({0, 3}), 3.0, 0, 0, 0, 0, false, Json{{"type", 77}}});
	}
	SECTION("an opaque key tail stays expanded") {
		key = Json::array({Json::array({0, 3}), 3.0, 0, 0, 0, 0, false, 0, 17, "opaque-tail"});
	}
	SECTION("an opaque marker tail stays expanded") {
		key[0].push_back(99);
	}
	SECTION("an unknown source node stays expanded") {
		type = "Node_Future";
	}
	Json child = {
		{"id", "child"},
		{"type", type},
		{"x", 0},
		{"y", 0},
		{"group", "root"},
		{"inputs", Json::array({Json{{"r", Json::array({key})}}})}
	};
	const auto archive =
		Archive(Json{{"animator", {{"frames_total", 1}}}, {"nodes", Json::array({root, child})}}.dump());
	PxcxCollectionSave result{"previous graph", std::string("previous metadata")};
	const auto prior = result;
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(PreparePxcxCollectionSave(archive, "root", std::nullopt, result, diagnostic));
	CHECK(result == prior);
}

TEST_CASE(
	"PXC Collection save recognizes every pinned Collection family as a root",
	"[imagegraphio][pxcx_append][collection_save][families]"
) {
	const std::vector<std::string> types = {
		"Node_Group",
		"Node_Collection",
		"Node_Canvas_Group",
		"Node_DynaSurf",
		"Node_Feedback",
		"Node_Iterate",
		"Node_Iterate_Each",
		"Node_Iterate_Filter",
		"Node_Iterate_Sort",
		"Node_Iterator",
		"Node_Pixel_Builder",
		"Node_Smoke_Group",
		"Node_Strand_Group",
		"Node_VFX_Group"
	};
	for (const auto &type : types) {
		Json node = Collection("root");
		node["type"] = type;
		auto archive = Archive(Json{{"nodes", Json::array({node})}}.dump());
		PxcxCollectionSave result;
		engine::imagegraph::Diagnostic diagnostic;
		CAPTURE(type);
		REQUIRE(PreparePxcxCollectionSave(archive, "root", std::nullopt, result, diagnostic));
		CHECK_FALSE(result.MetadataJson.has_value());
		const auto saved = Json::parse(result.GraphJson);
		REQUIRE(saved["nodes"].size() == 1);
		CHECK(saved["nodes"][0]["id"] == "root");
	}
}

TEST_CASE(
	"PXC Collection save refusals preserve the prior result",
	"[imagegraphio][pxcx_append][collection_save][atomic]"
) {
	auto archive = Archive(Json{{"nodes", Json::array({Collection("root")})}}.dump());
	PxcxCollectionSave prior{"previous graph", std::string("previous metadata")};
	PxcxCollectionSave result = prior;
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(PreparePxcxCollectionSave(archive, "missing", std::nullopt, result, diagnostic));
	CHECK(result == prior);
	Json ordinary = {
		{"id", "root"},
		{"type", "Node_Number_Simple"},
		{"x", 0},
		{"y", 0},
		{"inputs", Json::array({ValueJson(1)})}
	};
	auto nonCollection = Archive(Json{{"nodes", Json::array({ordinary})}}.dump());
	CHECK_FALSE(PreparePxcxCollectionSave(nonCollection, "root", std::nullopt, result, diagnostic));
	CHECK(result == prior);
	CHECK_FALSE(
		PreparePxcxCollectionSave(archive, "root", std::string_view("{bad json"), result, diagnostic)
	);
	CHECK(result == prior);
	CHECK_FALSE(PreparePxcxCollectionSave(archive, "root", std::nullopt, result, diagnostic, 1));
	CHECK(result == prior);
	Json cyclicRoot = Collection("root", "child");
	Json cyclicChild = Collection("child", "root");
	auto cyclic = Archive(Json{{"nodes", Json::array({cyclicRoot, cyclicChild})}}.dump());
	CHECK_FALSE(PreparePxcxCollectionSave(cyclic, "root", std::nullopt, result, diagnostic));
	CHECK(result == prior);
	const auto keyed = [](std::optional<int> frameCount) {
		Json source = Collection("root");
		source["inputs"] =
			Json::array({Json{{"r", Json::array({Json::array({Json::array({0, 5}), "keyed"})})}}});
		Json root = {{"nodes", Json::array({source})}};
		if (frameCount) root["animator"] = {{"frames_total", *frameCount}};
		return Archive(root.dump());
	};
	for (const auto &keyedArchive : {keyed(std::nullopt), keyed(1)}) {
		CHECK_FALSE(PreparePxcxCollectionSave(keyedArchive, "root", std::nullopt, result, diagnostic));
		CHECK(result == prior);
	}
	auto stale = archive;
	stale.GraphJson.insert(0, " ");
	CHECK_FALSE(PreparePxcxCollectionSave(stale, "root", std::nullopt, result, diagnostic));
	CHECK(result == prior);
}

TEST_CASE(
	"PXC post-load metadata, budget, and stale mapping refusals preserve the prior result",
	"[imagegraphio][pxcx_append][post_load][atomic]"
) {
	auto appended = AppendIncoming(
		Json{{"metadata", {{"description", "valid"}}}, {"nodes", Json::array({Collection("only")})}}
	);
	PxcxAppendPostLoad prior;
	prior.Collections = {{"older", "old manager"}};
	prior.Source = Destination();
	const auto priorCollections = prior.Collections;
	const auto priorSourceGraph = prior.Source->GraphJson;
	const auto priorSourceBytes = prior.Source->OriginalBytes;
	engine::imagegraph::Diagnostic diagnostic;
	auto malformed = appended;
	malformed.MetadataJson = R"JSON({"description":"first","description":"second"})JSON";
	CHECK_FALSE(PreparePxcxAppendPostLoad(malformed, "new.pxcx", prior, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(prior.Collections == priorCollections);
	CHECK(prior.Source->GraphJson == priorSourceGraph);
	CHECK(prior.Source->OriginalBytes == priorSourceBytes);

	CHECK_FALSE(PreparePxcxAppendPostLoad(appended, "new.pxcx", prior, diagnostic, 1));
	CHECK(diagnostic.Code == engine::imagegraph::Status::LimitExceeded);
	CHECK(prior.Collections == priorCollections);
	CHECK(prior.Source->GraphJson == priorSourceGraph);
	CHECK(prior.Source->OriginalBytes == priorSourceBytes);

	auto stale = appended;
	stale.Nodes.front().NodeId = "missing-node";
	CHECK_FALSE(PreparePxcxAppendPostLoad(stale, "new.pxcx", prior, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(prior.Collections == priorCollections);
	CHECK(prior.Source->GraphJson == priorSourceGraph);
	CHECK(prior.Source->OriginalBytes == priorSourceBytes);
	CHECK_FALSE(PreparePxcxAppendPostLoad(appended, std::string_view("bad\0path", 8), prior, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	CHECK(prior.Collections == priorCollections);
	CHECK(prior.Source->GraphJson == priorSourceGraph);
	CHECK(prior.Source->OriginalBytes == priorSourceBytes);
}
