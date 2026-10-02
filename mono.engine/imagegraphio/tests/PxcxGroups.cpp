#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_groups")
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::json;
	std::vector<engine::imagegraph::Keyframe> KeyframesFor(
		const engine::imagegraph::Document &document, std::string_view nodeId, std::string_view port
	) {
		std::vector<engine::imagegraph::Keyframe> result;
		for (const auto &key : document.Keyframes)
			if (key.NodeId == nodeId && key.Port == port) result.push_back(key);
		return result;
	}
	bool HasCanonicalStaticInput(
		const engine::imagegraph::Document &document, std::string_view nodeId, std::string_view port
	) {
		using namespace engine::imagegraph;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end() ||
			std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), port) ==
				node->SourceStaticInputs.end())
			return false;
		const auto keys = KeyframesFor(document, nodeId, port);
		const auto tracks =
			std::count_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
				return track.NodeId == nodeId && track.Port == port;
			});
		return keys.size() == 1 && GetFrameTime(keys.front()) == FrameTime{} &&
			   keys.front().Kind == KeyframeKind::Normal && tracks == 1;
	}
	Json Value(Json data) {
		return Json{{"r", {{"d", std::move(data)}}}};
	}
	Json Wire(std::string id, uint32_t index = 0) {
		return Json{{"from_node", std::move(id)}, {"from_index", index}, {"from_tag", 0}};
	}
	Json Node(std::string id, std::string type, Json inputs = Json::array()) {
		return Json{
			{"id", std::move(id)},
			{"type", std::move(type)},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)}
		};
	}
	Json Solid(std::string id, uint32_t packed) {
		Json inputs = Json::array();
		inputs.push_back(Json{{"r", {{"d", {1, 1}}}}, {"attri", {{"use_project_dimension", 1}}}});
		inputs.push_back(Value(packed));
		inputs.push_back(Value(false));
		inputs.push_back(Json{{"r", {{"d", -4}}}, {"attri", {{"mask_alpha_only", false}}}});
		inputs.push_back(Value(false));
		inputs.push_back(Value(-4));
		auto node = Node(std::move(id), "Node_Solid", std::move(inputs));
		node["attri"] = {{"color_depth", 3}};
		return node;
	}
	Json Invert(std::string id, std::string source) {
		Json inputs = Json::array(
			{Wire(std::move(source)),
			 Value(-4),
			 Value(1),
			 Value(true),
			 Value(15),
			 Value(false),
			 Value(0),
			 Value(false)}
		);
		auto node = Node(std::move(id), "Node_Invert", std::move(inputs));
		node["attri"] = Json::object();
		return node;
	}
	void DiagnoseInvert(const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan, const engine::imagegraph::Image &originalImage,
		std::string_view label) {
		const auto printImage = [](std::string_view name, int status, std::string_view message,
			const engine::imagegraph::Image &image) {
			std::cerr << name << " status=" << status << " message=" << message << " width=" << image.Width
				<< " height=" << image.Height << " pixels=";
			for (size_t index = 0; index < image.Pixels.size(); index++)
				std::cerr << (index == 0 ? "" : ",") << unsigned(image.Pixels[index]);
			std::cerr << "\n";
		};
		printImage("ORIGINAL_SINK", int(engine::imagegraph::Status::Ok), {}, originalImage);
		engine::imagegraph::Diagnostic explicitRequestDiagnostic;
		engine::imagegraph::Diagnostic overloadDiagnostic;
		engine::imagegraph::Image explicitRequestImage;
		const auto explicitRequestStatus = engine::imagegraph::Evaluate(
			document, plan, "sink", engine::imagegraph::EvaluationRequest{}, explicitRequestImage,
			explicitRequestDiagnostic);
		engine::imagegraph::Image overloadImage;
		const auto overloadStatus = engine::imagegraph::Evaluate(
			document, plan, "sink", overloadImage, overloadDiagnostic);
		printImage("REPEAT_SINK_EXPLICIT_REQUEST", int(explicitRequestStatus), explicitRequestDiagnostic.Message,
			explicitRequestImage);
		printImage("REPEAT_SINK_DEFAULT_OVERLOAD", int(overloadStatus), overloadDiagnostic.Message, overloadImage);
		engine::imagegraph::EvaluationSnapshot snapshot;
		engine::imagegraph::Diagnostic diagnostic;
		const auto status = engine::imagegraph::EvaluateNodeInputs(
			document, plan, "filter", engine::imagegraph::EvaluationRequest{}, snapshot, diagnostic);
		std::cerr << "INVERT_DIAGNOSTIC_BEGIN " << label << " status=" << int(status)
			<< " message=" << diagnostic.Message << "\n";
		for (const auto &value : snapshot.Values()) {
			std::cerr << value.Port << " variant=" << value.Data.index();
			if (const auto *v = std::get_if<bool>(&value.Data)) std::cerr << " bool=" << *v;
			if (const auto *v = std::get_if<double>(&value.Data)) std::cerr << " scalar=" << *v;
			if (const auto *v = std::get_if<int64_t>(&value.Data)) std::cerr << " integer=" << *v;
			std::cerr << "\n";
		}
		for (const auto &link : plan.EffectiveLinks)
			std::cerr << "EFFECTIVE_LINK " << link.FromNode << ":" << link.FromPort
				<< " -> " << link.ToNode << ":" << link.ToPort << "\n";
		for (const size_t index : plan.NodeOrder)
			std::cerr << "ORDER " << document.Nodes[index].Id << "\n";
		auto direct = document;
		direct.Outputs.push_back({"filter-diagnostic", "filter", "image"});
		engine::imagegraph::Plan directPlan;
		auto directStatus = engine::imagegraph::Compile(direct, directPlan, diagnostic);
		engine::imagegraph::Image directImage;
		if (directStatus == engine::imagegraph::Status::Ok)
			directStatus = engine::imagegraph::Evaluate(direct, directPlan, "filter-diagnostic", directImage, diagnostic);
		std::cerr << "DIRECT_FILTER status=" << int(directStatus) << " message=" << diagnostic.Message;
		for (size_t index = 0; index < std::min(size_t{4}, directImage.Pixels.size()); ++index)
			std::cerr << " channel" << index << "=" << unsigned(directImage.Pixels[index]);
		std::cerr << "\n";
		for (const std::string_view nodeId : {"output", "sink"}) {
			engine::imagegraph::EvaluationSnapshot incoming;
			const auto incomingStatus = engine::imagegraph::EvaluateNodeInputs(
				document, plan, nodeId, engine::imagegraph::EvaluationRequest{}, incoming, diagnostic);
			std::cerr << "HANDOFF_INPUT node=" << nodeId << " status=" << int(incomingStatus)
				<< " message=" << diagnostic.Message << "\n";
			for (const auto &inputImage : incoming.Images()) {
				std::cerr << "HANDOFF_IMAGE node=" << nodeId << " port=" << inputImage.Port;
				for (size_t index = 0; index < std::min(size_t{4}, inputImage.Data.Pixels.size()); ++index)
					std::cerr << " channel" << index << "=" << unsigned(inputImage.Data.Pixels[index]);
				std::cerr << "\n";
			}
		}
		for (const auto &selected : {engine::imagegraph::Output{"input-diagnostic", "input", "value"},
			engine::imagegraph::Output{"output-diagnostic", "output", "value"}}) {
			auto selectedDocument = document;
			selectedDocument.Outputs.push_back(selected);
			engine::imagegraph::Plan selectedPlan;
			auto selectedStatus = engine::imagegraph::Compile(selectedDocument, selectedPlan, diagnostic);
			engine::imagegraph::Image selectedImage;
			if (selectedStatus == engine::imagegraph::Status::Ok)
				selectedStatus = engine::imagegraph::Evaluate(selectedDocument, selectedPlan, selected.Id,
					selectedImage, diagnostic);
			std::cerr << "HANDOFF_OUTPUT node=" << selected.NodeId << " status=" << int(selectedStatus)
				<< " message=" << diagnostic.Message;
			for (size_t index = 0; index < std::min(size_t{4}, selectedImage.Pixels.size()); ++index)
				std::cerr << " channel" << index << "=" << unsigned(selectedImage.Pixels[index]);
			std::cerr << "\n";
		}
		engine::imagegraph::Image sinkImage;
		const auto sinkStatus = engine::imagegraph::Evaluate(document, plan, "sink", sinkImage, diagnostic);
		std::cerr << "HANDOFF_SINK status=" << int(sinkStatus) << " message=" << diagnostic.Message;
		for (size_t index = 0; index < std::min(size_t{4}, sinkImage.Pixels.size()); ++index)
			std::cerr << " channel" << index << "=" << unsigned(sinkImage.Pixels[index]);
		std::cerr << "\n";
		std::cerr << engine::imagegraph::Write(document) << "INVERT_DIAGNOSTIC_END\n";
	}
	void CheckPixels(const engine::imagegraph::Image &image, std::array<uint8_t, 4> expected) {
		REQUIRE(image.Width == 2);
		REQUIRE(image.Height == 2);
		REQUIRE(image.Pixels.size() == 16);
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4)
			for (size_t channel = 0; channel < 4; ++channel)
				CHECK(image.Pixels[offset + channel] == expected[channel]);
	}
	Json Graph() {
		auto root = Solid("root", 0xFF402010u);
		auto group = Node("group", "Node_Group", Json::array({Wire("root")}));
		group["attri"] = {{"custom_input_list", {"input"}}, {"custom_output_list", {"output"}}};
		auto input =
			Node("input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(4)}));
		input["group"] = "group";
		auto filter = Invert("filter", "input");
		filter["group"] = "group";
		filter["attri"] = Json::object();
		auto output = Node("output", "Node_Group_Output", Json::array({Wire("filter")}));
		output["group"] = "group";
		auto sink = Node("sink", "Node_Project_Output", Json::array({Wire("group")}));
		return Json{
			{"attributes", {{"surface_dimension", {2, 2}}}},
			{"nodes", Json::array({root, group, input, output, sink, filter})}
		};
	}
	engine::bake::PxcxArchive Archive(const Json &graph) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = graph.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		const bool written = engine::bake::WritePxcx(source, bytes, failure);
		INFO(failure);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
} // namespace
TEST_CASE("Saved group socket IDs project to persisted native boundary junctions", "[imagegraphio][groups]") {
	const auto archive = Archive(Graph());
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	CHECK(result.Source.OriginalBytes == archive.OriginalBytes);
	CHECK(result.Source.GraphJson == archive.GraphJson);
	REQUIRE(result.Graph.Groups.size() == 1);
	const auto &group = result.Graph.Groups.front();
	CHECK(group.Id == "group");
	CHECK(group.ColorDepth == 3);
	REQUIRE(group.Ports.size() == 2);
	CHECK(group.Ports[0].Id == "input");
	CHECK(group.Ports[1].Id == "output");
	REQUIRE(result.Graph.Junctions.size() == 2);
	CHECK(result.Graph.Junctions[0].GroupId == "group");
	CHECK(result.Graph.Junctions[1].GroupId == "group");
	REQUIRE(result.Graph.Outputs.size() == 1);
	CHECK(result.Graph.Outputs.front().NodeId == "output");
	CHECK_FALSE(result.Graph.Outputs.front().Port.empty());
	engine::imagegraph::Document restored;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(result.Graph), restored, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(restored == result.Graph);
	engine::imagegraph::Plan plan;
	{
		const auto status = engine::imagegraph::Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == engine::imagegraph::Status::Ok);
	}
	engine::imagegraph::EvaluationSnapshot filterInputs;
	REQUIRE(
		engine::imagegraph::EvaluateNodeInputs(
			restored, plan, "filter", engine::imagegraph::EvaluationRequest{}, filterInputs, diagnostic
		) == engine::imagegraph::Status::Ok
	);
	const auto mix =
		std::find_if(filterInputs.Values().begin(), filterInputs.Values().end(), [](const auto &value) {
			return value.Port == "mix";
		});
	REQUIRE(mix != filterInputs.Values().end());
	CHECK(mix->Data == engine::imagegraph::Value{1.0});
	const auto channel =
		std::find_if(filterInputs.Values().begin(), filterInputs.Values().end(), [](const auto &value) {
			return value.Port == "channel";
		});
	REQUIRE(channel != filterInputs.Values().end());
	CHECK(channel->Data == engine::imagegraph::Value{std::int64_t{15}});
	engine::imagegraph::Image image;
	REQUIRE(
		engine::imagegraph::Evaluate(restored, plan, "sink", image, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	DiagnoseInvert(restored, plan, image, "saved-boundary");
	CheckPixels(image, {239, 223, 191, 255});
}
TEST_CASE(
	"Invalid saved group socket membership and parents preserve the "
	"previous projection",
	"[imagegraphio][groups]"
) {
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(Graph()), result, failure));
	const auto previous = engine::imagegraph::Write(result.Graph);
	const auto bytes = result.Source.OriginalBytes;
	for (int mode = 0; mode < 13; ++mode) {
		CAPTURE(mode);
		auto graph = Graph();
		if (mode == 0) graph["nodes"][1]["attri"]["custom_input_list"] = {"missing"};
		if (mode == 1) graph["nodes"][1]["attri"]["custom_input_list"] = {"input", "input"};
		if (mode == 2) graph["nodes"][2]["group"] = "missing";
		if (mode == 3) graph["nodes"][1]["group"] = "group";
		if (mode == 4) {
			auto sibling = Node("sibling", "Node_Group");
			sibling["attri"] = Json::object();
			graph["nodes"].push_back(sibling);
			graph["nodes"][5]["group"] = "sibling";
		}
		if (mode == 5) graph["nodes"][1]["type"] = "Unknown_Collection";
		if (mode == 6) graph["nodes"][1]["attri"]["oversample"] = 14;
		if (mode == 7) graph["nodes"][1]["attri"]["interpolate"] = 8;
		if (mode == 10) graph["nodes"][1]["attri"]["unknown_render_control"] = 1;
		if (mode == 11) graph["nodes"][2]["inputs"][2] = Wire("missing");
		if (mode == 12)
			graph["nodes"][2]["inputs"][2] = {
				{"anim", true}, {"r", Json::array({Json::array({0, 33}), Json::array({1, 33})})}
			};
		if (mode == 8) graph["nodes"][1]["inputs"][0]["from_tag"] = 1;
		if (mode == 9) {
			auto sibling = Node("sibling", "Node_Group");
			sibling["attri"] = Json::object();
			graph["nodes"].push_back(sibling);
			graph["nodes"][5]["group"] = "sibling";
			graph["nodes"].erase(4);
		}
		if (mode == 11) {
			// Archive validation rejects a missing producer before native projection.
			engine::bake::PxcxArchive invalid;
			invalid.MetadataNumber = 121092;
			invalid.MetadataText = "1.22.10.201";
			invalid.GraphJson = graph.dump() + '\0';
			std::vector<std::byte> rejected;
			CHECK_FALSE(engine::bake::WritePxcx(invalid, rejected, failure));
			CHECK(failure == "pxcx: graph connection names a missing source node: missing");
		} else {
			CHECK_FALSE(ImportPxcxImageGraph(Archive(graph), result, failure));
		}
		CHECK(engine::imagegraph::Write(result.Graph) == previous);
		CHECK(result.Source.OriginalBytes == bytes);
	}
}
TEST_CASE(
	"Saved stripped processor depth is RGBA8 rather than its new-node "
	"inherited default",
	"[imagegraphio][groups][color_depth]"
) {
	auto graph = Graph();
	graph["nodes"] = Json::array({Node("solid", "Node_Solid")});
	graph["nodes"][0]["attri"] = Json::object();
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), result, failure));
	REQUIRE(result.Graph.Nodes.size() == 1);
	const auto &values = result.Graph.Nodes.front().Values;
	const auto selected = std::find_if(values.begin(), values.end(), [](const auto &value) {
		return value.Port == "attribute_color_depth";
	});
	REQUIRE(selected != values.end());
	CHECK(selected->Data == engine::imagegraph::Value{engine::imagegraph::EnumValue{3}});
}

TEST_CASE(
	"Nested groups route the first saved socket rather than the first "
	"display row",
	"[imagegraphio][groups]"
) {
	auto graph = Graph();
	graph["nodes"][0] = Solid("root", 0xFF402010u);
	auto blue = Solid("blue", 0xFF804020u);
	graph["nodes"].push_back(blue);
	graph["nodes"][1]["inputs"] = Json::array({Wire("blue"), Wire("root")});
	graph["nodes"][1]["attri"]["custom_input_list"] = {"input", "red_input"};
	graph["nodes"][1]["attri"]["input_display_list"] = {1, 0};
	auto red =
		Node("red_input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(4)}));
	red["group"] = "group";
	graph["nodes"].push_back(red);
	auto nested = Node("nested", "Node_Group", Json::array({Wire("input")}));
	nested["group"] = "group";
	nested["attri"] = {{"custom_input_list", {"nested_input"}}, {"custom_output_list", {"nested_output"}}};
	graph["nodes"].push_back(nested);
	auto nestedInput = Node(
		"nested_input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(4)})
	);
	nestedInput["group"] = "nested";
	graph["nodes"].push_back(nestedInput);
	graph["nodes"][5]["group"] = "nested";
	graph["nodes"][5]["inputs"][0] = Wire("nested_input");
	auto nestedOutput = Node("nested_output", "Node_Group_Output", Json::array({Wire("filter")}));
	nestedOutput["group"] = "nested";
	graph["nodes"].push_back(nestedOutput);
	graph["nodes"][3]["inputs"][0] = Wire("nested");
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), result, failure));
	REQUIRE(result.Graph.Groups.size() == 2);
	CHECK(result.Graph.Groups[0].Ports[0].Id == "input");
	CHECK(result.Graph.Groups[0].Ports[1].Id == "red_input");
	CHECK(result.Graph.Groups[1].ParentId == "group");
	engine::imagegraph::Diagnostic diagnostic;
	engine::imagegraph::Document restored;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(result.Graph), restored, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	engine::imagegraph::Plan plan;
	{
		const auto status = engine::imagegraph::Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == engine::imagegraph::Status::Ok);
	}
	engine::imagegraph::Image image;
	REQUIRE(
		engine::imagegraph::Evaluate(restored, plan, "sink", image, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CheckPixels(image, {223, 191, 127, 255});
}
TEST_CASE("A group does not change unrelated source tag admission", "[imagegraphio][groups]") {
	auto graph = Graph();
	auto unrelated = Invert("unrelated", "root");
	unrelated["inputs"][0]["from_tag"] = 1;
	graph["nodes"].push_back(unrelated);
	PxcxImport grouped;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), grouped, failure));
	graph["nodes"] = Json::array({graph["nodes"][0], unrelated});
	PxcxImport ordinary;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), ordinary, failure));
	const auto edge =
		std::find_if(grouped.Graph.Links.begin(), grouped.Graph.Links.end(), [](const auto &link) {
			return link.ToNode == "unrelated";
		});
	REQUIRE(edge != grouped.Graph.Links.end());
	REQUIRE(ordinary.Graph.Links.size() == 1);
	CHECK(*edge == ordinary.Graph.Links.front());
}

TEST_CASE(
	"Unrelated opaque source nodes remain visible beside native group "
	"boundaries",
	"[imagegraphio][groups]"
) {
	auto graph = Graph();
	graph["nodes"].push_back(Node("foreign", "Unknown_Source_Node"));
	const auto archive = Archive(graph);
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	REQUIRE(result.Graph.Groups.size() == 1);
	const auto foreign =
		std::find_if(result.Graph.Nodes.begin(), result.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "foreign";
		});
	REQUIRE(foreign != result.Graph.Nodes.end());
	CHECK(foreign->Type == "pxcx.opaque/Unknown_Source_Node");
	CHECK(result.Source.OriginalBytes == archive.OriginalBytes);
	CHECK(result.Source.GraphJson == archive.GraphJson);
	CHECK(std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(), [](const auto &diagnostic) {
		return diagnostic.NodeId == "foreign";
	}));
}

TEST_CASE(
	"Opaque dangling source membership is preserved without inventing a "
	"native group",
	"[imagegraphio][groups]"
) {
	auto foreign = Node("foreign", "Unknown_Future_Node");
	foreign["group"] = "future-group";
	auto graph = Graph();
	graph["nodes"] = Json::array({foreign});
	const auto archive = Archive(graph);
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, result, failure));
	REQUIRE(result.Graph.Nodes.size() == 1);
	CHECK(result.Graph.Nodes.front().GroupId.empty());
	CHECK(result.Graph.Groups.empty());
	CHECK(result.Source.GraphJson == archive.GraphJson);
	CHECK(result.Source.OriginalBytes == archive.OriginalBytes);
	CHECK(std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(), [](const auto &diagnostic) {
		return diagnostic.NodeId == "foreign" && diagnostic.Port == "group";
	}));
	graph["nodes"] = Json::array({Solid("native", 0xFF402010u)});
	graph["nodes"][0]["group"] = "future-group";
	const auto previous = engine::imagegraph::Write(result.Graph);
	CHECK_FALSE(ImportPxcxImageGraph(Archive(graph), result, failure));
	CHECK(engine::imagegraph::Write(result.Graph) == previous);
	CHECK(result.Source.OriginalBytes == archive.OriginalBytes);
}
TEST_CASE(
	"Constructor inherited and input depth keep proven RGBA8 handwritten "
	"routes",
	"[imagegraphio][groups][color_depth]"
) {
	auto graph = Graph();
	graph["nodes"][0].erase("attri");
	graph["nodes"][5].erase("attri");
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), result, failure));
	for (const auto &[id, expected] :
		 {std::pair{"root", "image.solid"}, std::pair{"filter", "image.invert"}}) {
		const auto found =
			std::find_if(result.Graph.Nodes.begin(), result.Graph.Nodes.end(), [&](const auto &node) {
				return node.Id == id;
			});
		REQUIRE(found != result.Graph.Nodes.end());
		CHECK(found->Type == expected);
	}
	engine::imagegraph::Plan plan;
	engine::imagegraph::Diagnostic diagnostic;
	{
		const auto status = engine::imagegraph::Compile(result.Graph, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == engine::imagegraph::Status::Ok);
	}
	engine::imagegraph::Image image;
	REQUIRE(
		engine::imagegraph::Evaluate(result.Graph, plan, "sink", image, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	DiagnoseInvert(result.Graph, plan, image, "inherited-depth");
	CheckPixels(image, {239, 223, 191, 255});
}
TEST_CASE(
	"HDR input producers prevent lossy handwritten routes even for "
	"explicit RGBA8 output",
	"[imagegraphio][groups][color_depth]"
) {
	for (bool strippedOutput : {false, true}) {
		auto graph = Graph();
		graph["nodes"][0]["attri"]["color_depth"] = 4;
		if (!strippedOutput) graph["nodes"][5].erase("attri");
		PxcxImport result;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(Archive(graph), result, failure));
		for (const auto &[id, expected] : {std::pair{"root", "pc.solid"}, std::pair{"filter", "pc.invert"}}) {
			const auto found =
				std::find_if(result.Graph.Nodes.begin(), result.Graph.Nodes.end(), [&](const auto &node) {
					return node.Id == id;
				});
			REQUIRE(found != result.Graph.Nodes.end());
			CHECK(found->Type == expected);
		}
		engine::imagegraph::Plan plan;
		engine::imagegraph::Diagnostic diagnostic;
		REQUIRE(
			engine::imagegraph::Compile(result.Graph, plan, diagnostic) == engine::imagegraph::Status::Ok
		);
		engine::imagegraph::Image image;
		REQUIRE(
			engine::imagegraph::Evaluate(result.Graph, plan, "sink", image, diagnostic) ==
			engine::imagegraph::Status::Ok
		);
		CHECK(
			image.Format == (strippedOutput ? engine::imagegraph::SurfaceFormat::RGBA8Unorm
											: engine::imagegraph::SurfaceFormat::RGBA16Float)
		);
		std::array<double, 4> pixel;
		REQUIRE(engine::imagegraph::LoadSurfacePixel(image, 0, 0, pixel));
		CHECK(std::abs(pixel[0] - (1.0 - 16.0 / 255.0)) < .002);
		CHECK(std::abs(pixel[1] - (1.0 - 32.0 / 255.0)) < .002);
		CHECK(std::abs(pixel[2] - (1.0 - 64.0 / 255.0)) < .002);
		CHECK(pixel[3] == 1.0);
	}
}

namespace {
	engine::imagegraph::Document Persisted(const Json &source) {
		const auto archive = Archive(source);
		PxcxImport imported;
		std::string failure;
		INFO(failure);
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		CHECK(imported.Source.OriginalBytes == archive.OriginalBytes);
		CHECK(imported.Source.GraphJson == archive.GraphJson);
		engine::imagegraph::Document result;
		engine::imagegraph::Diagnostic diagnostic;
		REQUIRE(
			engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), result, diagnostic) ==
			engine::imagegraph::Status::Ok
		);
		CHECK(result == imported.Graph);
		return result;
	}
	engine::imagegraph::EvaluatedValue SampleValue(const engine::imagegraph::Document &document) {
		engine::imagegraph::Diagnostic diagnostic;
		engine::imagegraph::Plan plan;
		{
			const auto status = engine::imagegraph::Compile(document, plan, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == engine::imagegraph::Status::Ok);
		}
		engine::imagegraph::EvaluatedValue result;
		const auto status = engine::imagegraph::EvaluateValue(document, plan, "sink", {}, result, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == engine::imagegraph::Status::Ok);
		return result;
	}
	Json NumericBoundary(double value, bool boolean) {
		auto group = Node("group", "Node_Group", Json::array({Value(value)}));
		group["attri"] = {{"custom_input_list", {"input"}}, {"custom_output_list", {"output"}}};
		auto input = Node(
			"input",
			"Node_Group_Input",
			Json::array({Value(0), Value(Json::array({0, 1})), Value(boolean ? 2 : 0)})
		);
		input["group"] = "group";
		auto consumer = boolean ? Node("consumer", "Node_Number", Json::array({Value(10.7), Wire("input")}))
								: Node(
									  "consumer",
									  "Node_String_Get_Char",
									  Json::array({Value("ABCD"), Wire("input"), Value(1)})
								  );
		consumer["group"] = "group";
		auto output = Node("output", "Node_Group_Output", Json::array({Wire("consumer")}));
		output["group"] = "group";
		auto sink = Node("sink", "Node_Project_Output", Json::array({Wire("group")}));
		return Json{
			{"attributes", {{"surface_dimension", {2, 2}}}},
			{"nodes", Json::array({group, input, consumer, output, sink})}
		};
	}
} // namespace
TEST_CASE(
	"Saved generic group numeric boundaries preserve raw values for "
	"actual source consumers",
	"[imagegraphio][groups][group_boundary]"
) {
	for (double raw : {.5, .500001}) {
		const auto document = Persisted(NumericBoundary(raw, true));
		CHECK(std::get<double>(SampleValue(document).Data) == (raw > .5 ? 11.0 : 10.7));
		const auto input = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
			return node.Id == "input";
		});
		REQUIRE(input != document.Nodes.end());
		const auto value = std::find_if(input->Values.begin(), input->Values.end(), [](const auto &value) {
			return value.Port == "parent_value";
		});
		REQUIRE(value != input->Values.end());
		CHECK(std::get<double>(value->Data) == raw);
	}
	for (double raw : {2.5, 3.5}) {
		const auto document = Persisted(NumericBoundary(raw, false));
		CHECK(std::get<std::string>(SampleValue(document).Data) == (raw == 2.5 ? "B" : "D"));
	}
}
TEST_CASE(
	"Input-derived group depth schedules an otherwise disconnected HDR parent",
	"[imagegraphio][groups][color_depth]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	graph["nodes"][0]["attri"]["color_depth"] = 4;
	graph["nodes"][1]["attri"]["color_depth"] = 0;
	auto generator = Solid("filter", 0xFF402010u);
	generator["group"] = "group";
	generator["attri"]["color_depth"] = 1;
	graph["nodes"][5] = generator;
	auto document = Persisted(graph);
	Plan plan;
	Diagnostic diagnostic;
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	REQUIRE(plan.GroupSurfaceDependencies.size() == 1);
	const auto &route = plan.GroupSurfaceDependencies.front();
	CHECK(document.Nodes[route.Consumer].Id == "filter");
	CHECK(document.Nodes[route.Producer].Id == "input");
	Image image;
	{
		const auto status = Evaluate(document, plan, "sink", image, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	CHECK(image.Format == SurfaceFormat::RGBA16Float);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
	CHECK(std::abs(pixel[0] - 16.0 / 255.0) < .001);
	CHECK(std::abs(pixel[1] - 32.0 / 255.0) < .001);
	CHECK(std::abs(pixel[2] - 64.0 / 255.0) < .001);
	CHECK(pixel[3] == 1.0);
	auto second = Solid("second", 0xFF804020u);
	second["attri"]["color_depth"] = 5;
	graph["nodes"].push_back(second);
	auto secondInput = ::Node(
		"second-input",
		"Node_Group_Input",
		Json::array({::Value(0), ::Value(Json::array({0, 1})), ::Value(4)})
	);
	secondInput["group"] = "group";
	graph["nodes"].push_back(secondInput);
	graph["nodes"][1]["attri"]["custom_input_list"] = {"second-input", "input"};
	graph["nodes"][1]["attri"]["input_display_list"] = {1, 0};
	graph["nodes"][1]["inputs"] = Json::array({Wire("second"), Wire("root")});
	document = Persisted(graph);
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	{
		const auto status = Evaluate(document, plan, "sink", image, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	CHECK(image.Format == SurfaceFormat::RGBA32Float);
	REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
	CHECK(std::abs(pixel[0] - 16.0 / 255.0) < .000001);
}
TEST_CASE(
	"Ordinary source instances reconcile children by class and retain "
	"local parent sockets",
	"[imagegraphio][groups][group_boundary]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	auto copy = ::Node("copy", "Node_Group", Json::array({Wire("root")}));
	copy["instanceBase"] = "group";
	copy["attri"] = {{"custom_input_list", {"copy-input"}}, {"custom_output_list", {"copy-output"}}};
	auto input = ::Node(
		"copy-input", "Node_Group_Input", Json::array({::Value(0), ::Value(Json::array({0, 1})), ::Value(4)})
	);
	input["group"] = "copy";
	auto filter = Invert("unrelated-name", "copy-input");
	filter["group"] = "copy";
	filter["inputs"][2] = ::Value(0);
	auto output = ::Node("copy-output", "Node_Group_Output", Json::array({Wire("unrelated-name")}));
	output["group"] = "copy";
	graph["nodes"].push_back(copy);
	graph["nodes"].push_back(output);
	graph["nodes"].push_back(filter);
	graph["nodes"].push_back(input);
	graph["nodes"][4]["inputs"][0] = Wire("copy");
	auto document = Persisted(graph);
	const auto projected = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
		return node.Id == "unrelated-name";
	});
	REQUIRE(projected != document.Nodes.end());
	CHECK(projected->InstanceBase == "filter");
	const auto local =
		std::find_if(projected->Values.begin(), projected->Values.end(), [](const auto &value) {
			return value.Port == "mix";
		});
	REQUIRE(local != projected->Values.end());
	CHECK(local->Data == engine::imagegraph::Value{0.0});
	Plan plan;
	Diagnostic diagnostic;
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	Image image;
	{
		const auto status = Evaluate(document, plan, "sink", image, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	CheckPixels(image, {239, 223, 191, 255});
	PxcxImport previous;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), previous, failure));
	REQUIRE(previous.GroupPrebinding.has_value());
	const auto subtypeBinding =
		std::find_if(previous.GroupBindings.begin(), previous.GroupBindings.end(), [](const auto &binding) {
			return binding.NodeId == "copy-input" && binding.Port == "subtype";
		});
	REQUIRE(subtypeBinding != previous.GroupBindings.end());
	CHECK(subtypeBinding->OwnerId == "input");
	CHECK(subtypeBinding->Getter == GroupSubtypeAnimator::Static);
	CHECK(subtypeBinding->Writer == GroupSubtypeAnimator::Static);
	CHECK(std::any_of(previous.GroupBindings.begin(), previous.GroupBindings.end(), [](const auto &binding) {
		return binding.NodeId == "unrelated-name" && binding.OwnerId == "filter" && binding.Port == "mix";
	}));
	const auto saved = std::find_if(
		previous.GroupPrebinding->Nodes.begin(), previous.GroupPrebinding->Nodes.end(), [](const auto &node) {
			return node.Id == "unrelated-name";
		}
	);
	REQUIRE(saved != previous.GroupPrebinding->Nodes.end());
	CHECK(saved->InstanceBase.empty());
	Plan savedPlan;
	REQUIRE(Compile(*previous.GroupPrebinding, savedPlan, diagnostic) == Status::Ok);
	Image savedImage;
	REQUIRE(Evaluate(*previous.GroupPrebinding, savedPlan, "sink", savedImage, diagnostic) == Status::Ok);
	CheckPixels(savedImage, {16, 32, 64, 255});
	GroupReplayState emptyReplay, localReplay, boundReplay;
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Static}, {"copy-input", GroupSubtypeAnimator::Static}
	};
	EvaluationRequest clock;
	clock.MaximumImageDimension = 128;
	REQUIRE(
		ReplayGroupBootstrap(
			*previous.GroupPrebinding, savedPlan, order, clock, emptyReplay, 1, localReplay, diagnostic
		) == Status::Ok
	);
	REQUIRE(
		BindGroupReplay(previous.Graph, previous.GroupBindings, localReplay, 1, boundReplay, diagnostic) ==
		Status::Ok
	);
	CHECK(boundReplay.InstancesBound());
	CHECK_FALSE(localReplay.InstancesBound());
	const auto savedBefore = Write(*previous.GroupPrebinding);
	const auto before = Write(previous.Graph);
	const auto sourceBytes = previous.Source.OriginalBytes;
	graph["nodes"][6]["instanceBase"] = "copy";
	CHECK_FALSE(ImportPxcxImageGraph(Archive(graph), previous, failure));
	CHECK(Write(previous.Graph) == before);
	CHECK(Write(*previous.GroupPrebinding) == savedBefore);
	CHECK(previous.Source.OriginalBytes == sourceBytes);
}
TEST_CASE(
	"Instance closure canonicalizes mixed source class mappings without "
	"losing local animation",
	"[imagegraphio][groups][group_instances]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	auto copy = ::Node("copy", "Node_Group", Json::array({Wire("root")}));
	copy["attri"] = {{"custom_input_list", {"copy-input"}}, {"custom_output_list", {"copy-output"}}};
	auto input = ::Node(
		"copy-input", "Node_Group_Input", Json::array({::Value(0), ::Value(Json::array({0, 1})), ::Value(4)})
	);
	input["group"] = "copy";
	auto filter = Invert("different-name", "copy-input");
	filter["group"] = "copy";
	filter["attri"] = {{"color_depth", 5}};
	filter["inputs"][2] = Json{
		{"anim", true},
		{"on_end", 0},
		{"r",
		 Json::array(
			 {Json::array(
				  {Json::array({1, -1.5}), 0.0, Json::array({.2, .3}), Json::array({.7, .8}), 1, 1, 0, 0}
			  ),
			  Json::array(
				  {Json::array({0, 2.25}), 0.0, Json::array({0, 1}), Json::array({0, 0}), 0, 0, 0, 0}
			  )}
		 )}
	};
	auto output = ::Node("copy-output", "Node_Group_Output", Json::array({Wire("different-name")}));
	output["group"] = "copy";
	graph["nodes"].push_back(copy);
	graph["nodes"].push_back(output);
	graph["nodes"].push_back(filter);
	graph["nodes"].push_back(input);
	graph["nodes"][4]["inputs"][0] = Wire("copy");
	const auto find = [](const Document &document, std::string_view id) -> const engine::imagegraph::Node & {
		const auto it = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == id;
		});
		REQUIRE(it != document.Nodes.end());
		return *it;
	};
	PxcxImport independent;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), independent, failure));
	CHECK(find(independent.Graph, "filter").Type == "image.invert");
	CHECK(find(independent.Graph, "different-name").Type == "pc.invert");
	const auto originalKeys = KeyframesFor(independent.Graph, "different-name", "mix");
	REQUIRE(originalKeys.size() == 2);
	CHECK(HasCanonicalStaticInput(independent.Graph, "copy-input", "range"));
	graph["nodes"][6]["instanceBase"] = "group";
	const auto source = Archive(graph);
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(find(imported.Graph, "filter").Type == "pc.invert");
	const auto &projected = find(imported.Graph, "different-name");
	CHECK(projected.Type == "pc.invert");
	CHECK(projected.InstanceBase == "filter");
	const auto projectedKeys = KeyframesFor(imported.Graph, "different-name", "mix");
	CHECK(projectedKeys == originalKeys);
	const auto adder = std::find_if(projectedKeys.begin(), projectedKeys.end(), [](const auto &key) {
		return key.Kind == KeyframeKind::Adder;
	});
	REQUIRE(adder != projectedKeys.end());
	CHECK(GetFrameTime(*adder) == FrameTime{1, .5, true});
	REQUIRE(adder->Ease);
	CHECK(adder->Ease->In == Vector2{.2, .3});
	CHECK(HasCanonicalStaticInput(imported.Graph, "copy-input", "range"));
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	Plan plan;
	{
		const auto status = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	EvaluationRequest request;
	request.Tick = 4;
	Image image;
	{
		const auto status = Evaluate(restored, plan, "sink", request, image, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
	CheckPixels(image, {239, 223, 191, 255});
}
TEST_CASE(
	"Imported bootstrap records preserve subtype animation mode and "
	"authoritative source bytes",
	"[imagegraphio][groups][group_bootstrap]"
) {
	auto graph = Graph();
	const auto original = Archive(graph);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(original, imported, failure));
	REQUIRE(imported.GroupBootstrap.size() == 1);
	CHECK(imported.GroupBootstrap[0].NodeId == "input");
	CHECK(imported.GroupBootstrap[0].SubtypeAnimator == engine::imagegraph::GroupSubtypeAnimator::Static);
	CHECK(imported.Source.OriginalBytes == original.OriginalBytes);
	graph["nodes"][2]["inputs"][0] = Json{
		{"anim", true},
		{"on_end", 0},
		{"r",
		 Json::array(
			 {Json::array({Json::array({0, -1.5}), 0, Json::array({0, 1}), Json::array({0, 0}), 0, 0, 0, 0})}
		 )}
	};
	const auto animated = Archive(graph);
	PxcxImport changed;
	REQUIRE(ImportPxcxImageGraph(animated, changed, failure));
	REQUIRE(changed.GroupBootstrap.size() == 1);
	CHECK(changed.GroupBootstrap[0].SubtypeAnimator == engine::imagegraph::GroupSubtypeAnimator::Animated);
	CHECK(changed.Source.OriginalBytes == animated.OriginalBytes);
	CHECK(imported.GroupBootstrap[0].SubtypeAnimator == engine::imagegraph::GroupSubtypeAnimator::Static);
}
TEST_CASE(
	"Saved nested instance boundaries reconcile by source class and "
	"preserve exact pixels",
	"[imagegraphio][groups][group_instances]"
) {
	auto graph = Graph();
	auto outer = Node("outer", "Node_Group", Json::array({Wire("root")}));
	outer["attri"] = {{"custom_input_list", {"outer-input"}}, {"custom_output_list", {"outer-output"}}};
	auto outerInput = Node(
		"outer-input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(4)})
	);
	outerInput["group"] = "outer";
	auto outerOutput = Node("outer-output", "Node_Group_Output", Json::array({Wire("group")}));
	outerOutput["group"] = "outer";
	graph["nodes"][1]["group"] = "outer";
	graph["nodes"][1]["inputs"][0] = Wire("outer-input");
	auto copyOuter = outer;
	copyOuter["id"] = "copy-outer";
	copyOuter["instanceBase"] = "outer";
	copyOuter["attri"] = {
		{"custom_input_list", {"copy-outer-input"}}, {"custom_output_list", {"copy-outer-output"}}
	};
	auto copyOuterInput = outerInput;
	copyOuterInput["id"] = "copy-outer-input";
	copyOuterInput["group"] = "copy-outer";
	auto copyOuterOutput = outerOutput;
	copyOuterOutput["id"] = "copy-outer-output";
	copyOuterOutput["group"] = "copy-outer";
	copyOuterOutput["inputs"][0] = Wire("copy-inner");
	auto copyInner = graph["nodes"][1];
	copyInner["id"] = "copy-inner";
	copyInner["group"] = "copy-outer";
	copyInner["inputs"][0] = Wire("copy-outer-input");
	copyInner["attri"] = {
		{"custom_input_list", {"copy-inner-input"}}, {"custom_output_list", {"copy-inner-output"}}
	};
	auto copyInnerInput = graph["nodes"][2];
	copyInnerInput["id"] = "copy-inner-input";
	copyInnerInput["group"] = "copy-inner";
	auto copyInnerOutput = graph["nodes"][3];
	copyInnerOutput["id"] = "copy-inner-output";
	copyInnerOutput["group"] = "copy-inner";
	copyInnerOutput["inputs"][0] = Wire("copy-filter");
	auto copyFilter = Invert("copy-filter", "copy-inner-input");
	copyFilter["group"] = "copy-inner";
	copyFilter["inputs"][2] = Value(0);
	graph["nodes"][4]["inputs"][0] = Wire("copy-outer");
	for (const auto &record :
		 {outer,
		  outerInput,
		  outerOutput,
		  copyOuter,
		  copyOuterInput,
		  copyOuterOutput,
		  copyInner,
		  copyInnerInput,
		  copyInnerOutput,
		  copyFilter})
		graph["nodes"].push_back(record);
	const auto archive = Archive(graph);
	PxcxImport imported;
	std::string failure;
	const bool accepted = ImportPxcxImageGraph(archive, imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	CHECK(imported.Source.OriginalBytes == archive.OriginalBytes);
	const auto nested =
		std::find_if(imported.Graph.Groups.begin(), imported.Graph.Groups.end(), [](const auto &group) {
			return group.Id == "copy-inner";
		});
	REQUIRE(nested != imported.Graph.Groups.end());
	CHECK(nested->InstanceBase == "group");
	CHECK(nested->ParentId == "copy-outer");
	engine::imagegraph::Document persisted;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), persisted, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(persisted == imported.Graph);
	engine::imagegraph::Plan plan;
	const auto compiled = engine::imagegraph::Compile(persisted, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(compiled == engine::imagegraph::Status::Ok);
	engine::imagegraph::Image image;
	const auto evaluated = engine::imagegraph::Evaluate(persisted, plan, "sink", image, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(evaluated == engine::imagegraph::Status::Ok);
	CheckPixels(image, {239, 223, 191, 255});
}
TEST_CASE(
	"Omitted generic parent retains the source minus-one constructor value",
	"[imagegraphio][groups][group_bootstrap]"
) {
	auto group = Node("group", "Node_Group", Json::array());
	group["attri"] = {{"custom_input_list", {"input"}}, {"custom_output_list", {"output"}}};
	auto input =
		Node("input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(1)}));
	input["group"] = "group";
	auto output = Node("output", "Node_Group_Output", Json::array({Wire("input")}));
	output["group"] = "group";
	auto sink = Node("sink", "Node_Project_Output", Json::array({Wire("group")}));
	const auto source = Archive(Json{{"nodes", Json::array({group, input, output, sink})}});
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	engine::imagegraph::Document persisted;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), persisted, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	const auto sampled = SampleValue(persisted);
	CHECK(std::get<double>(sampled.Data) == -1.0);
	REQUIRE(sampled.Domain);
	CHECK(sampled.Domain->Kind == engine::imagegraph::SourceSocketKind::Float);
}

TEST_CASE(
	"Compressed animated group controls retain animation with source "
	"frame-zero defaults",
	"[imagegraphio][groups][group_bootstrap]"
) {
	auto graph = Graph();
	graph["nodes"][2]["inputs"][0] = Value(0.5);
	graph["nodes"][2]["inputs"][0]["anim"] = true;
	const auto source = Archive(graph);
	PxcxImport imported;
	std::string failure;
	const bool accepted = ImportPxcxImageGraph(source, imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	REQUIRE(imported.GroupBootstrap.size() == 1);
	CHECK(
		imported.GroupBootstrap.front().SubtypeAnimator == engine::imagegraph::GroupSubtypeAnimator::Animated
	);
	const auto subtypeKeys = KeyframesFor(imported.Graph, "input", "subtype");
	REQUIRE(subtypeKeys.size() == 1);
	CHECK(std::get<double>(subtypeKeys.front().Data) == 0.5);
	CHECK(engine::imagegraph::GetFrameTime(subtypeKeys.front()) == engine::imagegraph::FrameTime{});
	CHECK(subtypeKeys.front().Kind == engine::imagegraph::KeyframeKind::Normal);
	REQUIRE(subtypeKeys.front().Ease);
	CHECK(HasCanonicalStaticInput(imported.Graph, "input", "range"));
	CHECK(subtypeKeys.front().Ease->InType == "linear");
	CHECK(subtypeKeys.front().Ease->OutType == "linear");
	CHECK(subtypeKeys.front().Ease->In == engine::imagegraph::Vector2{0, 1});
	CHECK(subtypeKeys.front().Ease->Out == engine::imagegraph::Vector2{0, 0});
	CHECK_FALSE(subtypeKeys.front().SourceDriver);
	const auto subtypeTrack =
		std::find_if(imported.Graph.Tracks.begin(), imported.Graph.Tracks.end(), [](const auto &track) {
			return track.NodeId == "input" && track.Port == "subtype";
		});
	REQUIRE(subtypeTrack != imported.Graph.Tracks.end());
	CHECK(subtypeTrack->End == "hold");
	engine::imagegraph::Document restored;
	engine::imagegraph::Diagnostic diagnostic;
	const auto read =
		engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), restored, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(read == engine::imagegraph::Status::Ok);
	CHECK(restored == imported.Graph);
}

TEST_CASE(
	"Import replacement charges retained callback records and preserves "
	"them below admission",
	"[imagegraphio][groups][import_budget]"
) {
	const auto archive = Archive(Graph());
	const auto prior = [&]() {
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		REQUIRE(imported.GroupBootstrap.size() == 1);
		imported.GroupBootstrap.reserve(32);
		imported.GroupBootstrap.front().NodeId.assign(1024, 'x');
		return imported;
	};
	const auto admits = [&](uint64_t limit) {
		auto destination = prior();
		auto options = destination.Options;
		options.MaximumOperationBytes = limit;
		std::string failure;
		return ImportPxcxImageGraph(archive, destination, failure, options);
	};
	uint64_t low = 1, high = engine::imagegraph::Limits::MaximumEvaluationBytes;
	REQUIRE(admits(high));
	while (low < high) {
		const uint64_t middle = low + (high - low) / 2;
		if (admits(middle))
			high = middle;
		else
			low = middle + 1;
	}
	REQUIRE(low > 1);
	auto destination = prior();
	const auto native = engine::imagegraph::Write(destination.Graph);
	const auto original = destination.Source.OriginalBytes;
	const auto capacity = destination.GroupBootstrap.capacity();
	auto options = destination.Options;
	options.MaximumOperationBytes = low - 1;
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(archive, destination, failure, options));
	CHECK_FALSE(failure.empty());
	CHECK(engine::imagegraph::Write(destination.Graph) == native);
	CHECK(destination.Source.OriginalBytes == original);
	CHECK(destination.GroupBootstrap.capacity() == capacity);
	CHECK(destination.GroupBootstrap.front().NodeId == std::string(1024, 'x'));
	options.MaximumOperationBytes = low;
	REQUIRE(ImportPxcxImageGraph(archive, destination, failure, options));
	CHECK(destination.GroupBootstrap.front().NodeId == "input");
	options.MaximumOperationBytes = 0;
	CHECK_FALSE(ImportPxcxImageGraph(archive, destination, failure, options));
	CHECK(destination.GroupBootstrap.front().NodeId == "input");
	options.MaximumOperationBytes = engine::imagegraph::Limits::MaximumEvaluationBytes + 1;
	CHECK_FALSE(ImportPxcxImageGraph(archive, destination, failure, options));
	CHECK(destination.Source.OriginalBytes == original);
}

TEST_CASE(
	"Imported chained bindings retain nearest override mode and ultimate "
	"animator owner",
	"[imagegraphio][groups][group_binding]"
) {
	using engine::imagegraph::GroupSubtypeAnimator;
	for (size_t length = 16; length <= 29; ++length) {
		INFO(length);
		auto graph = Graph();
		const std::string owner(length, 'o'), middle(length, 'm'), target(length, 't');
		graph["nodes"][2]["id"] = owner;
		graph["nodes"][1]["attri"]["custom_input_list"] = Json::array({owner});
		graph["nodes"][5]["inputs"][0] = Wire(owner);
		graph["nodes"][2]["inputs"][0]["anim"] = true;
		for (const auto &pair : {std::pair{"middle", "group"}, std::pair{"outer", "middle"}}) {
			const std::string groupId = pair.first;
			const std::string inputId = groupId == "middle" ? middle : target;
			auto group = Node(groupId, "Node_Group", Json::array({Wire("root")}));
			group["instanceBase"] = pair.second;
			group["attri"] = {
				{"custom_input_list", {inputId}}, {"custom_output_list", {groupId + "-output"}}
			};
			auto input = Node(
				inputId, "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(4)})
			);
			input["group"] = groupId;
			if (groupId == "middle") input["inputs"][0]["attri"]["override_instance"] = true;
			auto filter = Invert(groupId + "-filter", inputId);
			filter["group"] = groupId;
			auto output =
				Node(groupId + "-output", "Node_Group_Output", Json::array({Wire(groupId + "-filter")}));
			output["group"] = groupId;
			graph["nodes"].push_back(group);
			graph["nodes"].push_back(input);
			graph["nodes"].push_back(filter);
			graph["nodes"].push_back(output);
		}
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(Archive(graph), imported, failure));
		INFO(failure);
		const auto binding = std::find_if(
			imported.GroupBindings.begin(), imported.GroupBindings.end(), [&](const auto &binding) {
				return binding.NodeId == target && binding.Port == "subtype";
			}
		);
		REQUIRE(binding != imported.GroupBindings.end());
		CHECK(binding->OwnerId == owner);
		CHECK(binding->Getter == GroupSubtypeAnimator::Static);
		CHECK(binding->Writer == GroupSubtypeAnimator::Animated);
		CHECK(binding->NodeId.capacity() == std::string(target).capacity());
		CHECK(binding->OwnerId.capacity() == std::string(owner).capacity());
	}
}

TEST_CASE(
	"Imported empty Group animator retains durable source mode", "[imagegraphio][groups][group_bootstrap]"
) {
	auto graph = Graph();
	graph["nodes"][2]["inputs"][0] =
		Json{{"anim", true}, {"r", Json::array()}, {"on_end", 3}, {"loop_range", 7}};
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	const auto node =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &candidate) {
			return candidate.Id == "input";
		});
	REQUIRE(node != imported.Graph.Nodes.end());
	CHECK(node->SourceAnimatedInputs == std::vector<std::string>{"subtype"});
	CHECK(KeyframesFor(imported.Graph, "input", "subtype").empty());
	const auto subtypeTrack =
		std::find_if(imported.Graph.Tracks.begin(), imported.Graph.Tracks.end(), [](const auto &track) {
			return track.NodeId == "input" && track.Port == "subtype";
		});
	REQUIRE(subtypeTrack != imported.Graph.Tracks.end());
	CHECK(subtypeTrack->End == "wrap");
	CHECK(subtypeTrack->LoopRange == 7);
	CHECK(HasCanonicalStaticInput(imported.Graph, "input", "range"));
	engine::imagegraph::Document restored;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), restored, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(restored == imported.Graph);
	engine::imagegraph::Plan plan;
	const auto compiled = engine::imagegraph::Compile(restored, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == engine::imagegraph::Status::Ok);
}

TEST_CASE(
	"Empty Group control animators retain zero and raw empty range storage",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	graph["nodes"][2]["inputs"][2] = Json{{"anim", true}, {"r", Json::array()}};
	graph["nodes"][2]["inputs"][1] = Json{{"anim", true}, {"r", Json::array()}};
	graph["nodes"][2]["inputs"][0] = Json{{"r", Json::array()}};
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	Document restored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	const auto compiled = Compile(restored, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(restored, plan, "input", {}, snapshot, diagnostic) == Status::Ok);
	const auto inputType =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "input_type";
		});
	REQUIRE(inputType != snapshot.Values().end());
	CHECK(std::get<double>(inputType->Data) == 0.0);
	const auto range =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "range";
		});
	REQUIRE(range != snapshot.Values().end());
	REQUIRE(std::holds_alternative<Vector2>(range->Data));
	CHECK(std::get<Vector2>(range->Data) == Vector2{0, 0});
	const auto authored =
		std::find_if(restored.Nodes.begin(), restored.Nodes.end(), [](const engine::imagegraph::Node &node) {
			return node.Id == "input";
		});
	REQUIRE(authored != restored.Nodes.end());
	const auto storedRange =
		std::find_if(authored->Values.begin(), authored->Values.end(), [](const AuthoredValue &value) {
			return value.Port == "range";
		});
	REQUIRE(storedRange != authored->Values.end());
	REQUIRE(std::holds_alternative<ArrayValue>(storedRange->Data));
	CHECK(std::get<ArrayValue>(storedRange->Data).ElementType == ValueType::Scalar);
	CHECK(std::get<ArrayValue>(storedRange->Data).Elements.empty());
	CHECK(std::get<ArrayValue>(storedRange->Data).Nested.empty());
	GroupReplayState previous, replay;
	const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
	const auto bootstrapped =
		ReplayGroupBootstrap(restored, plan, order, {}, previous, 1, replay, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(bootstrapped == Status::Ok);
	REQUIRE(replay.Entries().size() == 1);
	CHECK(replay.Entries().front().InputType == 0);
	CHECK(replay.Entries().front().Domain.Type == ValueType::Integer);
}

TEST_CASE(
	"Static empty Group Range and Vec2 animators retain raw zero and "
	"resolve zero vectors",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	for (bool explicitStatic : {false, true}) {
		auto graph = Graph();
		Json empty = {{"r", Json::array()}};
		if (explicitStatic) empty["anim"] = false;
		auto &inputs = graph["nodes"][2]["inputs"];
		while (inputs.size() < 13)
			inputs.push_back(Json::object());
		inputs[3] = ::Value("");
		inputs[4] = ::Value(0);
		inputs[5] = ::Value(0);
		inputs[6] = ::Value(true);
		inputs[7] = ::Value(.01);
		inputs[8] = ::Value("Trigger");
		inputs[9] = ::Value(0);
		inputs[10] = ::Value(0);
		inputs[11] = ::Value(0);
		inputs[1] = empty;
		inputs[12] = empty;
		PxcxImport imported;
		std::string failure;
		const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		Document restored;
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		const auto compiled = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		const auto node = std::find_if(
			restored.Nodes.begin(), restored.Nodes.end(), [](const engine::imagegraph::Node &candidate) {
				return candidate.Id == "input";
			}
		);
		REQUIRE(node != restored.Nodes.end());
		CHECK(node->SourceAnimatedInputs.empty());
		for (const std::string port : {"range", "gizmo_position"}) {
			const auto track =
				std::find_if(restored.Tracks.begin(), restored.Tracks.end(), [&](const auto &track) {
					return track.NodeId == "input" && track.Port == port;
				});
			REQUIRE(track != restored.Tracks.end());
			CHECK(track->End == "hold");
			CHECK(track->LoopRange == -1);
		}
		for (const std::string port : {"range", "gizmo_position"}) {
			CHECK(KeyframesFor(restored, "input", port).empty());
			const auto staticPort =
				std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), port);
			CHECK(staticPort != node->SourceStaticInputs.end());
			CHECK(std::count_if(restored.Tracks.begin(), restored.Tracks.end(), [&](const auto &track) {
					  return track.NodeId == "input" && track.Port == port;
				  }) == 1);
		}
		GroupReplayState previous, replay;
		const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
		const auto bootstrapped =
			ReplayGroupBootstrap(restored, plan, order, {}, previous, 1, replay, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(bootstrapped == Status::Ok);
		REQUIRE(replay.Entries().size() == 1);
		CHECK(replay.Entries().front().InputType == 4);
		CHECK(replay.Entries().front().Subtype == 0);
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(restored, plan, "input", {}, snapshot, diagnostic) == Status::Ok);
		std::vector<AuthoredValue> resolved;
		REQUIRE(ResolveNodeValues(restored, plan, "sink", "input", {}, resolved, diagnostic) == Status::Ok);
		for (const std::string port : {"range", "gizmo_position"}) {
			const auto raw =
				std::find_if(node->Values.begin(), node->Values.end(), [&](const AuthoredValue &value) {
					return value.Port == port;
				});
			REQUIRE(raw != node->Values.end());
			CHECK(std::get<double>(raw->Data) == 0.0);
			const auto input =
				std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &value) {
					return value.Port == port;
				});
			REQUIRE(input != snapshot.Values().end());
			CHECK(std::get<Vector2>(input->Data) == Vector2{0, 0});
			const auto authored =
				std::find_if(resolved.begin(), resolved.end(), [&](const AuthoredValue &value) {
					return value.Port == port;
				});
			REQUIRE(authored != resolved.end());
			CHECK(std::get<Vector2>(authored->Data) == Vector2{0, 0});
		}
	}
}

TEST_CASE(
	"Importer aliases every cloned child source input with independent "
	"getter and writer modes",
	"[imagegraphio][groups][group_binding]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	const auto key = [](Json data, Json driver = 0) {
		return Json::array(
			{Json::array({0, 0}),
			 std::move(data),
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 std::move(driver)}
		);
	};
	graph["nodes"][5]["inputs"][2] =
		Json{{"anim", false}, {"r", Json::array({key(.25, Json{{"typ", "linear"}, {"spd", .125}})})}};

	auto copy = ::Node("copy", "Node_Group", Json::array({Wire("root")}));
	copy["instanceBase"] = "group";
	copy["attri"] = {{"custom_input_list", {"copy-input"}}, {"custom_output_list", {"copy-output"}}};
	auto input = ::Node(
		"copy-input", "Node_Group_Input", Json::array({::Value(0), ::Value(Json::array({0, 1})), ::Value(4)})
	);
	input["group"] = "copy";
	input["inputs"][1] = Json{
		{"anim", true},
		{"r", Json::array({key(Json::array({2, 6}))})},
		{"attri", {{"override_instance", true}}}
	};
	auto filter = Invert("copy-filter", "copy-input");
	filter["group"] = "copy";
	filter["inputs"][2] =
		Json{{"anim", true}, {"r", Json::array({key(.25)})}, {"attri", {{"override_instance", true}}}};
	auto output = ::Node("copy-output", "Node_Group_Output", Json::array({Wire("copy-filter")}));
	output["group"] = "copy";
	graph["nodes"].push_back(copy);
	graph["nodes"].push_back(input);
	graph["nodes"].push_back(filter);
	graph["nodes"].push_back(output);
	graph["nodes"][4]["inputs"][0] = Wire("copy");
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	size_t expectedCount = 0;
	for (const auto &node : imported.Graph.Nodes) {
		if (node.InstanceBase.empty()) continue;
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		for (const auto &input : entry->Inputs) {
			if (input.SourceIndex < 0 || input.Id == "parent_value") continue;
			++expectedCount;
			const auto binding = std::find_if(
				imported.GroupBindings.begin(), imported.GroupBindings.end(), [&](const auto &binding) {
					return binding.NodeId == node.Id && binding.Port == input.Id;
				}
			);
			REQUIRE(binding != imported.GroupBindings.end());
		}
	}
	CHECK(imported.GroupBindings.size() == expectedCount);
	for (const auto port : {std::string_view("range"), std::string_view("mix")}) {
		const auto binding = std::find_if(
			imported.GroupBindings.begin(), imported.GroupBindings.end(), [&](const auto &binding) {
				return binding.Port == port;
			}
		);
		REQUIRE(binding != imported.GroupBindings.end());
		CHECK(binding->Getter == GroupSubtypeAnimator::Animated);
		CHECK(binding->Writer == GroupSubtypeAnimator::Static);
	}
	CHECK(std::none_of(imported.GroupBindings.begin(), imported.GroupBindings.end(), [](const auto &binding) {
		return binding.Port == "parent_value" || binding.Port.starts_with("attribute_");
	}));
	const auto node =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "copy-filter";
		});
	REQUIRE(node != imported.Graph.Nodes.end());
	CHECK(node->SourceAnimatedInputs == std::vector<std::string>{"mix"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	const auto ownerKey =
		std::find_if(imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "filter" && key.Port == "mix";
		});
	REQUIRE(ownerKey != imported.Graph.Keyframes.end());
	REQUIRE(ownerKey->SourceDriver);
	CHECK(ownerKey->SourceDriver == KeyframeSourceDriver{KeyframeLinearDriver{.125}});
	const auto ownerNode =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "filter";
		});
	REQUIRE(ownerNode != imported.Graph.Nodes.end());
	CHECK(
		std::find(ownerNode->SourceStaticInputs.begin(), ownerNode->SourceStaticInputs.end(), "mix") !=
		ownerNode->SourceStaticInputs.end()
	);
	REQUIRE(imported.GroupPrebinding);
	Plan localPlan, plan;
	REQUIRE(Compile(*imported.GroupPrebinding, localPlan, diagnostic) == Status::Ok);
	REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
	std::vector<GroupBootstrapTarget> order;
	for (const auto &record : imported.GroupBootstrap)
		order.push_back({record.NodeId, record.SubtypeAnimator});
	EvaluationRequest clock;
	clock.Tick = 3;
	GroupReplayState empty, loaded, bound;
	REQUIRE(
		ReplayGroupBootstrap(
			*imported.GroupPrebinding, localPlan, order, clock, empty, 1, loaded, diagnostic
		) == Status::Ok
	);
	REQUIRE(
		BindGroupReplay(imported.Graph, imported.GroupBindings, loaded, 1, bound, diagnostic) == Status::Ok
	);
	clock.GroupReplay = &bound;
	clock.GroupAuthoringRevision = 1;
	for (const auto id : {std::string_view("filter"), std::string_view("copy-filter")}) {
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(imported.Graph, plan, id, clock, snapshot, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		const auto mix =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "mix";
			});
		REQUIRE(mix != snapshot.Values().end());
		CHECK(mix->Data == engine::imagegraph::Value{id == "filter" ? .25 : .625});
	}
}

TEST_CASE(
	"Imported Group parent mode drives signed-clock edits and survives native restore",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	auto graph = NumericBoundary(.25, true);
	graph["nodes"][0]["inputs"][0]["anim"] = true;
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	Document authored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(imported.Graph), authored, diagnostic) == Status::Ok);
	REQUIRE(Compile(authored, plan, diagnostic) == Status::Ok);
	const auto originalWholeKeys = authored.Keyframes;
	const auto originalKeys = KeyframesFor(authored, "input", "parent_value");
	REQUIRE(originalKeys.size() == 1);
	CHECK(HasCanonicalStaticInput(authored, "input", "range"));
	const auto node =
		std::find_if(authored.Nodes.begin(), authored.Nodes.end(), [](const engine::imagegraph::Node &n) {
			return n.Id == "input";
		});
	REQUIRE(node != authored.Nodes.end());
	CHECK(
		std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), "parent_value") !=
		node->SourceAnimatedInputs.end()
	);
	const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
	GroupReplayState empty, loaded, edited;
	EvaluationRequest clock;
	REQUIRE(SetFrameTime(clock, {1, .375, true}));
	REQUIRE(
		RestoreGroupDeclarations(authored, plan, order, clock, empty, 1, loaded, diagnostic) == Status::Ok
	);
	engine::imagegraph::Value replacement = .75;
	GroupRefreshEvent event;
	event.NodeId = "input";
	event.Reason = GroupRefreshReason::ParentEdit;
	event.At = clock;
	event.SubtypeAnimator = GroupSubtypeAnimator::Static;
	event.EditedPort = "parent_value";
	event.LocalValue = &replacement;
	event.LocalAnimated =
		std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), event.EditedPort) !=
		node->SourceAnimatedInputs.end();
	REQUIRE(ReplayGroupRefresh(authored, plan, {&event, 1}, loaded, 1, edited, diagnostic) == Status::Ok);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Nodes == authored.Nodes);
	CHECK(projected.Keyframes.size() == originalWholeKeys.size() + 1);
	for (const auto &key : originalWholeKeys) {
		if (key.NodeId == "input" && key.Port == "parent_value") continue;
		CHECK(
			std::find(projected.Keyframes.begin(), projected.Keyframes.end(), key) !=
			projected.Keyframes.end()
		);
	}
	const auto projectedParentKeys = KeyframesFor(projected, "input", "parent_value");
	REQUIRE(projectedParentKeys.size() == 2);
	CHECK(
		std::find(projectedParentKeys.begin(), projectedParentKeys.end(), originalKeys.front()) !=
		projectedParentKeys.end()
	);
	const auto added =
		std::find_if(projected.Keyframes.begin(), projected.Keyframes.end(), [&](const Keyframe &key) {
			return key.NodeId == "input" && key.Port == "parent_value" &&
				   GetFrameTime(key) == GetFrameTime(clock);
		});
	REQUIRE(added != projected.Keyframes.end());
	CHECK(added->Data == replacement);
	Document restored;
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
	Plan savedPlan;
	REQUIRE(Compile(restored, savedPlan, diagnostic) == Status::Ok);
	EvaluationRequest later;
	REQUIRE(SetFrameTime(later, {40, .125, false}));
	GroupReplayState restoredState;
	REQUIRE(
		RestoreGroupDeclarations(restored, savedPlan, order, later, empty, 2, restoredState, diagnostic) ==
		Status::Ok
	);
	Document again;
	REQUIRE(ProjectGroupReplay(restored, restoredState, 2, again, diagnostic) == Status::Ok);
	CHECK(Write(again) == Write(restored));

	// Native keys alone never change the separately imported static mode declaration.
	graph["nodes"][0]["inputs"][0]["anim"] = false;
	PxcxImport staticImport;
	REQUIRE(ImportPxcxImageGraph(Archive(graph), staticImport, failure));
	Document staticAuthored = staticImport.Graph;
	staticAuthored.Keyframes = originalWholeKeys;
	staticAuthored.Tracks = authored.Tracks;
	REQUIRE(Compile(staticAuthored, savedPlan, diagnostic) == Status::Ok);
	const auto staticNode = std::find_if(
		staticAuthored.Nodes.begin(), staticAuthored.Nodes.end(), [](const engine::imagegraph::Node &n) {
			return n.Id == "input";
		}
	);
	REQUIRE(staticNode != staticAuthored.Nodes.end());
	event.LocalAnimated =
		std::find(
			staticNode->SourceAnimatedInputs.begin(), staticNode->SourceAnimatedInputs.end(), event.EditedPort
		) != staticNode->SourceAnimatedInputs.end();
	CHECK_FALSE(event.LocalAnimated);
	GroupReplayState staticLoaded, staticEdited;
	REQUIRE(
		RestoreGroupDeclarations(
			staticAuthored, savedPlan, order, clock, empty, 1, staticLoaded, diagnostic
		) == Status::Ok
	);
	REQUIRE(
		ReplayGroupRefresh(
			staticAuthored, savedPlan, {&event, 1}, staticLoaded, 1, staticEdited, diagnostic
		) == Status::Ok
	);
	Document staticProjected;
	REQUIRE(ProjectGroupReplay(staticAuthored, staticEdited, 1, staticProjected, diagnostic) == Status::Ok);
	CHECK(staticProjected.Keyframes.size() == originalWholeKeys.size());
	for (const auto &key : originalWholeKeys) {
		if (key.NodeId == "input" && key.Port == "parent_value") continue;
		CHECK(
			std::find(staticProjected.Keyframes.begin(), staticProjected.Keyframes.end(), key) !=
			staticProjected.Keyframes.end()
		);
	}
	const auto staticParentKeys = KeyframesFor(staticProjected, "input", "parent_value");
	REQUIRE(staticParentKeys.size() == 1);
	CHECK(GetFrameTime(staticParentKeys.front()) == GetFrameTime(originalKeys.front()));
	CHECK(staticParentKeys.front().Data == replacement);
}

TEST_CASE(
	"Empty animated Group parent retains mode and track settings even without keys",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	auto graph = NumericBoundary(.25, true);
	graph["nodes"][0]["inputs"][0] =
		Json{{"anim", true}, {"r", Json::array()}, {"on_end", 3}, {"loop_range", 7}};
	graph["nodes"][1]["inputs"][0]["anim"] = true;
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	const auto node = std::find_if(
		imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const engine::imagegraph::Node &n) {
			return n.Id == "input";
		}
	);
	REQUIRE(node != imported.Graph.Nodes.end());
	CHECK(
		std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), "parent_value") !=
		node->SourceAnimatedInputs.end()
	);
	CHECK(
		std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), "subtype") !=
		node->SourceAnimatedInputs.end()
	);
	const auto track =
		std::find_if(imported.Graph.Tracks.begin(), imported.Graph.Tracks.end(), [](const AnimationTrack &t) {
			return t.NodeId == "input" && t.Port == "parent_value";
		});
	REQUIRE(track != imported.Graph.Tracks.end());
	CHECK(track->End == "wrap");
	CHECK(track->LoopRange == 7);
	Document restored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	graph = Graph();
	graph["nodes"][1]["inputs"][0]["anim"] = "true";
	CHECK_FALSE(ImportPxcxImageGraph(Archive(graph), imported, failure));
	CHECK(failure == "group parent animator mode is malformed");
}

TEST_CASE(
	"Linked Group parent animator mode and empty settings remain locally persisted",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	graph["nodes"][1]["inputs"][0]["anim"] = true;
	graph["nodes"][1]["inputs"][0]["r"] = Json::array();
	graph["nodes"][1]["inputs"][0]["on_end"] = 3;
	graph["nodes"][1]["inputs"][0]["loop_range"] = 7;
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	const auto node = std::find_if(
		imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const engine::imagegraph::Node &n) {
			return n.Id == "input";
		}
	);
	REQUIRE(node != imported.Graph.Nodes.end());
	CHECK(
		std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), "parent_value") !=
		node->SourceAnimatedInputs.end()
	);
	const auto track =
		std::find_if(imported.Graph.Tracks.begin(), imported.Graph.Tracks.end(), [](const AnimationTrack &t) {
			return t.NodeId == "input" && t.Port == "parent_value";
		});
	REQUIRE(track != imported.Graph.Tracks.end());
	CHECK(track->End == "wrap");
	CHECK(track->LoopRange == 7);
	Document restored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(restored, plan, "input", {}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	CHECK(snapshot.Images().front().Port == "parent_value");
}

TEST_CASE(
	"Linked Group parent retains inactive local source keys without replacing its link",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	auto graph = Graph();
	graph["nodes"][1]["inputs"][0]["anim"] = true;
	graph["nodes"][1]["inputs"][0]["r"] = Json{{"d", .25}};
	PxcxImport imported;
	std::string failure;
	const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	const auto parentKeys = KeyframesFor(imported.Graph, "input", "parent_value");
	REQUIRE(parentKeys.size() == 1);
	CHECK(std::get<double>(parentKeys.front().Data) == .25);
	CHECK(HasCanonicalStaticInput(imported.Graph, "input", "range"));
	Document restored;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(restored, plan, "input", {}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	CHECK(snapshot.Images().front().Port == "parent_value");
}

TEST_CASE(
	"Static Group parent source provenance retains keyed settings "
	"without sampling its local driver",
	"[imagegraphio][groups][group_bootstrap]"
) {
	using namespace engine::imagegraph;
	for (bool explicitStatic : {false, true}) {
		auto graph = NumericBoundary(.25, true);
		const auto key = [](double time, double data, Json driver = 0) {
			return Json::array(
				{Json::array({0, time}),
				 data,
				 Json::array({0, 1}),
				 Json::array({0, 0}),
				 0,
				 0,
				 true,
				 std::move(driver)}
			);
		};
		Json parent = {
			{"r", Json::array({key(0, .25, Json{{"typ", "linear"}, {"spd", .125}}), key(4, .75)})},
			{"on_end", 1},
			{"loop_range", 0}
		};
		if (explicitStatic) parent["anim"] = false;
		graph["nodes"][0]["inputs"][0] = parent;
		PxcxImport imported;
		std::string failure;
		const auto accepted = ImportPxcxImageGraph(Archive(graph), imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		Document restored;
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		const auto node =
			std::find_if(restored.Nodes.begin(), restored.Nodes.end(), [](const engine::imagegraph::Node &n) {
				return n.Id == "input";
			});
		REQUIRE(node != restored.Nodes.end());
		CHECK(
			std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), "parent_value") !=
			node->SourceStaticInputs.end()
		);
		CHECK(
			std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), "parent_value") ==
			node->SourceAnimatedInputs.end()
		);
		const auto parentKeys = KeyframesFor(restored, "input", "parent_value");
		REQUIRE(parentKeys.size() == 2);
		const auto drivenKey = std::find_if(parentKeys.begin(), parentKeys.end(), [](const auto &key) {
			return key.SourceDriver.has_value();
		});
		REQUIRE(drivenKey != parentKeys.end());
		CHECK(drivenKey->SourceDriver == KeyframeSourceDriver{KeyframeLinearDriver{.125}});
		const auto parentTrack =
			std::find_if(restored.Tracks.begin(), restored.Tracks.end(), [](const auto &track) {
				return track.NodeId == "input" && track.Port == "parent_value";
			});
		REQUIRE(parentTrack != restored.Tracks.end());
		CHECK(parentTrack->End == "loop");
		CHECK(parentTrack->LoopRange == 0);
		CHECK(HasCanonicalStaticInput(restored, "input", "range"));
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		EvaluationRequest clock;
		clock.Tick = 8;
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(restored, plan, "input", clock, snapshot, diagnostic) == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &entry) {
				return entry.Port == "parent_value";
			});
		REQUIRE(value != snapshot.Values().end());
		CHECK(std::get<double>(value->Data) == .25);
	}
}

TEST_CASE(
	"Source early-return animators retain unused loop and wrap settings", "[imagegraphio][timeline_v9]"
) {
	using namespace engine::imagegraph;
	for (const bool animated : {false, true}) {
		for (const int64_t loopRange : {int64_t(999), int64_t(-7)}) {
			const auto key = [](int64_t frame, double data) {
				return Json::array(
					{Json::array({0, frame}),
					 data,
					 Json::array({0, 1}),
					 Json::array({0, 0}),
					 0,
					 0,
					 true,
					 Json{{"typ", "linear"}, {"spd", .5}}}
				);
			};
			Json keys = Json::array({key(0, 2)});
			if (!animated) keys.push_back(key(9, 7));
			Json graph{
				{"nodes",
				 Json::array({::Node(
					 "number",
					 "Node_Number_Simple",
					 Json::array(
						 {Json{{"anim", animated}, {"r", keys}, {"on_end", 3}, {"loop_range", loopRange}}}
					 )
				 )})}
			};
			PxcxImport imported;
			std::string failure;
			REQUIRE(ImportPxcxImageGraph(Archive(graph), imported, failure));
			REQUIRE(imported.Graph.Nodes.front().Type == "pc.number_simple");
			REQUIRE(imported.Graph.Tracks.size() == 1);
			CHECK(imported.Graph.Tracks.front().End == "wrap");
			CHECK(imported.Graph.Tracks.front().LoopRange == loopRange);
			CHECK_FALSE(imported.Graph.Timeline);
			imported.Graph.Outputs = {{"number", "number", "number"}};
			Document restored;
			Diagnostic diagnostic;
			REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
			CHECK(restored == imported.Graph);
			Plan plan;
			const auto status = Compile(restored, plan, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			EvaluationRequest request;
			request.Tick = 4;
			request.Subframe = .5;
			EvaluatedValue value;
			REQUIRE(EvaluateValue(restored, plan, "number", request, value, diagnostic) == Status::Ok);
			CHECK(value.Data == engine::imagegraph::Value{animated ? 4.25 : 2.0});
			request.NegativeFrame = true;
			REQUIRE(EvaluateValue(restored, plan, "number", request, value, diagnostic) == Status::Ok);
			CHECK(value.Data == engine::imagegraph::Value{2.0});
			auto unmarked = restored;
			unmarked.Nodes.front().SourceAnimatedInputs.clear();
			unmarked.Nodes.front().SourceStaticInputs.clear();
			CHECK(Compile(unmarked, plan, diagnostic) == Status::InvalidValue);
		}
	}
}

TEST_CASE(
	"Static animator Matrix construction admits expanded native payload before allocation",
	"[imagegraphio][timeline_v9]"
) {
	using namespace engine::imagegraph;
	const Json matrix{{"size", Json::array({64, 64})}, {"raw", Json::array()}};
	const Json key =
		Json::array({Json::array({0, 0}), matrix, Json::array({0, 1}), Json::array({0, 0}), 0, 0, true, 0});
	const Json graph{
		{"nodes",
		 Json::array({::Node(
			 "matrix", "Node_Matrix_Invert", Json::array({Json{{"anim", false}, {"r", Json::array({key})}}})
		 )})}
	};
	const auto archive = Archive(graph);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
	REQUIRE(imported.Graph.Keyframes.size() == 1);
	const auto &data = std::get<MatrixValue>(imported.Graph.Keyframes.front().Data);
	CHECK(data.Values.size() == 4096);
	PxcxImport sentinel;
	sentinel.Graph.Nodes = {{"kept", "value.number", "", {}, {{"value", 42.0}}}};
	const auto before = sentinel.Graph;
	auto smallGraph = graph;
	smallGraph["nodes"][0]["inputs"][0]["r"][0][1]["size"] = Json::array({1, 1});
	const auto smallArchive = Archive(smallGraph);
	PxcxImportOptions options;
	uint64_t rejected = 0, accepted = Limits::MaximumEvaluationBytes;
	while (accepted - rejected > 1) {
		const uint64_t candidate = rejected + (accepted - rejected) / 2;
		options.MaximumOperationBytes = candidate;
		auto probe = sentinel;
		if (ImportPxcxImageGraph(smallArchive, probe, failure, options))
			accepted = candidate;
		else
			rejected = candidate;
	}
	options.MaximumOperationBytes = accepted;
	auto small = sentinel;
	REQUIRE(ImportPxcxImageGraph(smallArchive, small, failure, options));
	CHECK_FALSE(ImportPxcxImageGraph(archive, sentinel, failure, options));
	CHECK(failure.find("operation bounds") != std::string::npos);
	CHECK(sentinel.Graph == before);
}

TEST_CASE(
	"Long dynamic source input names use admitted exact string construction", "[imagegraphio][timeline_v9]"
) {
	using namespace engine::imagegraph;
	Json inputs = Json::array();
	for (size_t index = 0; index < 17; ++index)
		inputs.push_back(Json::object());
	inputs[16] = Json{{"r", {{"d", .75}}}};
	const auto archive = Archive(Json{{"nodes", Json::array({::Node("light", "Node_2D_light", inputs)})}});
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.2_d_light");
	const auto &node = imported.Graph.Nodes.front();
	const auto input =
		std::find_if(node.DynamicInputs.begin(), node.DynamicInputs.end(), [](const auto &input) {
			return input.Id == "radial_band_ratio_0";
		});
	REQUIRE(input != node.DynamicInputs.end());
	CHECK(input->Id.capacity() == input->Id.size());
	REQUIRE(input->Default);
	CHECK(*input->Default == engine::imagegraph::Value{.75});
	PxcxImport sentinel;
	sentinel.Graph.Nodes = {{"kept", "value.number", "", {}, {{"value", 42.0}}}};
	const auto before = sentinel.Graph;
	PxcxImportOptions options;
	uint64_t rejected = 0, accepted = Limits::MaximumEvaluationBytes;
	while (accepted - rejected > 1) {
		options.MaximumOperationBytes = rejected + (accepted - rejected) / 2;
		auto probe = sentinel;
		if (ImportPxcxImageGraph(archive, probe, failure, options))
			accepted = options.MaximumOperationBytes;
		else
			rejected = options.MaximumOperationBytes;
	}
	options.MaximumOperationBytes = accepted;
	auto admitted = sentinel;
	REQUIRE(ImportPxcxImageGraph(archive, admitted, failure, options));
	options.MaximumOperationBytes = accepted - 1;
	CHECK_FALSE(ImportPxcxImageGraph(archive, sentinel, failure, options));
	CHECK(sentinel.Graph == before);
}

TEST_CASE(
	"Static compact input animator settings persist with their implicit source key",
	"[imagegraphio][timeline_v9]"
) {
	using namespace engine::imagegraph;
	for (const bool parent : {false, true}) {
		Json record{{"anim", false}, {"r", {{"d", 2.0}}}, {"on_end", 3}, {"loop_range", 999}};
		Json graph;
		if (parent) {
			graph = NumericBoundary(.25, true);
			graph["nodes"][0]["inputs"][0] = record;
		} else
			graph =
				Json{{"nodes", Json::array({::Node("number", "Node_Number_Simple", Json::array({record}))})}};
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(Archive(graph), imported, failure));
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		const std::string id = parent ? "input" : "number", port = parent ? "parent_value" : "value";
		const auto track =
			std::find_if(restored.Tracks.begin(), restored.Tracks.end(), [&](const auto &track) {
				return track.NodeId == id && track.Port == port;
			});
		REQUIRE(track != restored.Tracks.end());
		CHECK(track->End == "wrap");
		CHECK(track->LoopRange == 999);
		const auto key =
			std::find_if(restored.Keyframes.begin(), restored.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == id && key.Port == port;
			});
		REQUIRE(key != restored.Keyframes.end());
		CHECK(GetFrameTime(*key) == FrameTime{});
		CHECK(key->Kind == KeyframeKind::Normal);
		CHECK(key->Interpolation == "source");
		CHECK(key->Ease == KeyframeEase{"linear", "linear", {0, 1}, {0, 0}});
		CHECK(key->Data == engine::imagegraph::Value{2.0});
		if (!parent) restored.Outputs = {{"out", "number", "number"}};
		Plan plan;
		const auto compiled = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.Tick = 7;
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(restored, plan, id, request, snapshot, diagnostic) == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &value) {
				return value.Port == port;
			});
		REQUIRE(value != snapshot.Values().end());
		CHECK(value->Data == engine::imagegraph::Value{2.0});
	}
}

TEST_CASE(
	"Mandatory static compact keys reject an exhausted native key count atomically",
	"[imagegraphio][timeline_v9]"
) {
	using namespace engine::imagegraph;
	Json keys = Json::array();
	for (size_t time = 0; time < Limits::MaximumKeyframes; ++time)
		keys.push_back(
			Json::array(
				{Json::array({0, time}), 1.0, Json::array({0, 1}), Json::array({0, 0}), 0, 0, true, 0}
			)
		);
	const Json graph{
		{"nodes",
		 Json::array({::Node(
			 "math",
			 "Node_Math",
			 Json::array(
				 {Json::object(), Json{{"anim", true}, {"r", std::move(keys)}}, Json{{"r", {{"d", 2.0}}}}}
			 )
		 )})}
	};
	PxcxImport sentinel;
	sentinel.Graph.Nodes = {{"kept", "value.number", "", {}, {{"value", 42.0}}}};
	const auto before = sentinel.Graph;
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(Archive(graph), sentinel, failure));
	CHECK(failure.find("static compact source keyframe count") != std::string::npos);
	CHECK(sentinel.Graph == before);
}

TEST_CASE(
	"Compact and signed sole empty Group vector keys retain exact source provenance",
	"[imagegraphio][timeline_v9]"
) {
	using namespace engine::imagegraph;
	for (bool animated : {false, true})
		for (bool compact : {false, true})
			for (bool range : {false, true}) {
				auto graph = NumericBoundary(.25, true);
				auto &inputs = graph["nodes"][1]["inputs"];
				while (inputs.size() <= 12)
					inputs.push_back(Json::object());
				const size_t index = range ? 1 : 12;
				const std::string port = range ? "range" : "gizmo_position";
				Json raw;
				if (compact)
					raw = Json{{"d", Json::array()}};
				else
					raw = Json::array({Json::array(
						{Json::array({0, -2.5}),
						 Json::array(),
						 Json::array({0, 1}),
						 Json::array({0, 0}),
						 0,
						 0,
						 true,
						 0}
					)});
				inputs[index] = Json{{"anim", animated}, {"r", raw}, {"on_end", 3}, {"loop_range", 999}};
				PxcxImport imported;
				std::string failure;
				REQUIRE(ImportPxcxImageGraph(Archive(graph), imported, failure));
				const auto key = std::find_if(
					imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [&](const auto &key) {
						return key.NodeId == "input" && key.Port == port;
					}
				);
				REQUIRE(key != imported.Graph.Keyframes.end());
				CHECK(key->Data == engine::imagegraph::Value{ArrayValue{ValueType::Scalar, {}}});
				CHECK(key->Kind == KeyframeKind::Normal);
				CHECK_FALSE(key->SourceDriver);
				CHECK(GetFrameTime(*key) == (compact ? FrameTime{} : FrameTime{2, .5, true}));
				Document restored;
				Diagnostic diagnostic;
				REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
				CHECK(restored == imported.Graph);
				Plan plan;
				const auto status = Compile(restored, plan, diagnostic);
				INFO(diagnostic.Message);
				REQUIRE(status == Status::Ok);
				EvaluationRequest clock;
				clock.Tick = 5;
				clock.Subframe = .25;
				clock.NegativeFrame = true;
				EvaluationSnapshot snapshot;
				REQUIRE(
					EvaluateNodeInputs(restored, plan, "input", clock, snapshot, diagnostic) == Status::Ok
				);
				const auto input =
					std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &input) {
						return input.Port == port;
					});
				REQUIRE(input != snapshot.Values().end());
				CHECK(
					input->Data == (range ? engine::imagegraph::Value{Vector2{0, 0}}
										  : engine::imagegraph::Value{ArrayValue{ValueType::Scalar, {}}})
				);
			}
}

TEST_CASE("Linked source quaternion metadata survives import and native save", "[imagegraphio][groups]") {
	using namespace engine::imagegraph;
	for (const int64_t mode : {0, 1}) {
		auto linked = Wire("producer");
		linked["attri"] = {{"angle_display", mode}};
		linked["anim"] = false;
		linked["r"] = {{"d", Json::array({0, 0, 180, 0})}};
		const auto graph = Json{
			{"nodes",
			 Json::array(
				 {::Node(
					  "producer",
					  "Node_Quarternion_From_Euler",
					  Json::array({::Value(Json::array({90, 0, 0}))})
				  ),
				  ::Node("target", "Node_Quarternion_To_Euler", Json::array({linked}))}
			 )}
		};
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(Archive(graph), imported, failure));
		const auto track =
			std::find_if(imported.Graph.Tracks.begin(), imported.Graph.Tracks.end(), [](const auto &track) {
				return track.NodeId == "target" && track.Port == "rotation";
			});
		REQUIRE(track != imported.Graph.Tracks.end());
		CHECK(track->QuaternionMode == std::optional<int64_t>{mode});
		CHECK(
			std::none_of(
				imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
					return key.NodeId == "target" && key.Port == "rotation";
				}
			)
		);
		imported.Graph.Outputs = {{"producer", "producer", "rotation"}, {"target", "target", "euler_angles"}};
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		EvaluationRequest clock;
		REQUIRE(SetFrameTime(clock, {2, .5, true}));
		EvaluatedValue produced;
		REQUIRE(EvaluateValue(restored, plan, "producer", clock, produced, diagnostic) == Status::Ok);
		REQUIRE(std::holds_alternative<Vector4>(produced.Data));
		const auto vector = std::get<Vector4>(produced.Data);
		Quaternion expected;
		REQUIRE(ConvertSourceQuaternion({vector.X, vector.Y, vector.Z, vector.W}, mode, expected));
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(restored, plan, "target", clock, snapshot, diagnostic) == Status::Ok);
		const auto input =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "rotation";
			});
		REQUIRE(input != snapshot.Values().end());
		CHECK(input->Data == engine::imagegraph::Value{expected});
	}
}
