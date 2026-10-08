#include "../src/GroupInstances.hpp"

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_collection_instances")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::json;

	Json CollectionInstanceClasses() {
		return {
			{"nodes",
			 Json::array(
				 {{{"id", "outer"}, {"type", "Node_Group"}},
				  {{"id", "copy"}, {"type", "Node_Group"}},
				  {{"id", "collection"}, {"type", "Node_Collection"}},
				  {{"id", "local"}, {"type", "Node_Collection"}},
				  {{"id", "source-input"}, {"type", "Node_Group_Input"}},
				  {{"id", "local-input"}, {"type", "Node_Group_Input"}},
				  {{"id", "source-output"}, {"type", "Node_Group_Output"}},
				  {{"id", "local-output"}, {"type", "Node_Group_Output"}},
				  {{"id", "source-child"}, {"type", "Node_Number"}},
				  {{"id", "local-child"}, {"type", "Node_Number"}}}
			 )}
		};
	}

	Document NestedCollections(bool matched) {
		Document document;
		document.FormatVersion = 10;
		document.Groups = {{"outer", "Base"}, {"copy", "Copy"}, {"collection", "Source collection"}};
		document.Groups[1].InstanceBase = "outer";
		document.Groups[2].ParentId = "outer";
		document.Groups[2].ColorDepth = 5;
		document.Nodes.push_back({"source-child", "pc.number", "collection", {}, {{"value", 12.0}}});
		if (matched) {
			document.Groups.push_back({"local", "Local collection"});
			document.Groups.back().ParentId = "copy";
			document.Groups.back().ColorDepth = 3;
			document.Nodes.push_back({"local-child", "pc.number", "local", {}, {{"value", 99.0}}});
		}
		return document;
	}

	void AddSocket(Document &document, std::string groupId, std::string id, PortDirection direction) {
		const std::string type = direction == PortDirection::Input ? "pc.group_input" : "pc.group_output";
		document.Nodes.push_back({id, type, groupId, {}, {}});
		const auto group =
			std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &item) {
				return item.Id == groupId;
			});
		REQUIRE(group != document.Groups.end());
		group->Ports.push_back({id, id + "/parent", direction, id});
		document.Junctions.push_back({id + "/parent", groupId, ValueType::Any, 0.0});
	}

	Json Value(Json data) {
		return {{"r", {{"d", std::move(data)}}}};
	}
	Json Wire(std::string id) {
		return {{"from_node", std::move(id)}, {"from_index", 0}, {"from_tag", 0}};
	}
	Json SourceNode(std::string id, std::string type, Json inputs = Json::array()) {
		return {
			{"id", std::move(id)},
			{"type", std::move(type)},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)}
		};
	}
	Json Solid(std::string id, uint32_t packed, std::string group) {
		auto node = SourceNode(
			std::move(id),
			"Node_Solid",
			Json::array(
				{Json{{"r", {{"d", {1, 1}}}}, {"attri", {{"use_project_dimension", 1}}}},
				 Value(packed),
				 Value(false),
				 Json{{"r", {{"d", -4}}}, {"attri", {{"mask_alpha_only", false}}}},
				 Value(false),
				 Value(-4)}
			)
		);
		node["group"] = std::move(group);
		node["attri"] = {{"color_depth", 1}};
		return node;
	}
	Json GenericCollectionSource() {
		auto base = SourceNode("base", "Node_Collection");
		base["attri"] = {
			{"custom_input_list", Json::array()}, {"custom_output_list", {"base-output"}}, {"color_depth", 3}
		};
		auto local = SourceNode("local", "Node_Collection");
		local["instanceBase"] = "base";
		local["attri"] = {
			{"custom_input_list", Json::array()}, {"custom_output_list", {"local-output"}}, {"color_depth", 5}
		};
		auto baseOutput = SourceNode("base-output", "Node_Group_Output", Json::array({Wire("base-image")}));
		baseOutput["group"] = "base";
		auto localOutput =
			SourceNode("local-output", "Node_Group_Output", Json::array({Wire("local-image")}));
		localOutput["group"] = "local";
		return {
			{"attributes", {{"surface_dimension", {1, 1}}}},
			{"nodes",
			 Json::array(
				 {base,
				  local,
				  Solid("base-image", 0xff0000ffu, "base"),
				  baseOutput,
				  Solid("local-image", 0xffff0000u, "local"),
				  localOutput,
				  SourceNode("sink", "Node_Project_Output", Json::array({Wire("local")}))}
			 )}
		};
	}
	Json NestedCollectionSource() {
		auto outer = SourceNode("outer", "Node_Group");
		auto copy = SourceNode("copy", "Node_Group");
		copy["instanceBase"] = "outer";
		auto sourceCollection = SourceNode("collection", "Node_Collection", Json::array({Value(10.0)}));
		sourceCollection["group"] = "outer";
		sourceCollection["attri"] = {
			{"custom_input_list", {"source-input"}}, {"custom_output_list", Json::array()}
		};
		auto localCollection = SourceNode("local", "Node_Collection", Json::array({Value(99.0)}));
		localCollection["group"] = "copy";
		localCollection["attri"] = {
			{"custom_input_list", {"local-input"}}, {"custom_output_list", Json::array()}
		};
		auto sourceInput = SourceNode(
			"source-input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 1})), Value(1)})
		);
		sourceInput["group"] = "collection";
		auto localInput = SourceNode(
			"local-input", "Node_Group_Input", Json::array({Value(0), Value(Json::array({0, 2})), Value(1)})
		);
		localInput["group"] = "local";
		auto sourceNumber = SourceNode("source-child", "Node_Number", Json::array({Wire("source-input")}));
		sourceNumber["group"] = "collection";
		auto localNumber = SourceNode("local-child", "Node_Number", Json::array({Wire("local-input")}));
		localNumber["group"] = "local";
		return {
			{"nodes",
			 Json::array(
				 {outer,
				  copy,
				  sourceCollection,
				  localCollection,
				  sourceInput,
				  localInput,
				  sourceNumber,
				  localNumber}
			 )}
		};
	}

	Json PositionalCollectionSource() {
		auto outer = SourceNode("outer", "Node_Group");
		auto copy = SourceNode("copy", "Node_Group");
		copy["instanceBase"] = "outer";
		const auto key = [](int64_t tick, double value) {
			return Json::array(
				{Json::array({0, tick}), value, Json::array({0, 1}), Json::array({0, 0}), 0, 0, true, 0}
			);
		};
		Json secondParent = {
			{"anim", true}, {"r", Json::array({key(0, .625), key(4, .875)})}, {"on_end", 1}, {"loop_range", 0}
		};
		auto sourceCollection =
			SourceNode("collection", "Node_Collection", Json::array({Value(.125), secondParent}));
		sourceCollection["group"] = "outer";
		sourceCollection["attri"] = {
			{"custom_input_list", {"source-z", "source-a"}}, {"custom_output_list", Json::array()}
		};
		auto localCollection =
			SourceNode("local", "Node_Collection", Json::array({Value(.375), Value(.25), Value(.5)}));
		localCollection["group"] = "copy";
		localCollection["attri"] = {
			{"custom_input_list", {"local-a", "local-z", "local-extra"}},
			{"custom_output_list", Json::array()}
		};
		const auto input = [&](std::string id, std::string group, std::string name, double rangeEnd) {
			auto control = SourceNode(
				std::move(id),
				"Node_Group_Input",
				Json::array({Value(0), Value(Json::array({0, rangeEnd})), Value(1)})
			);
			control["group"] = std::move(group);
			control["name"] = std::move(name);
			return control;
		};
		// grug saved index, declaration order, ID suffix and display name all disagree.
		return {
			{"animator", {{"frames_total", 8}, {"playback", 0}, {"framerate", 24}}},
			{"nodes",
			 Json::array(
				 {outer,
				  copy,
				  sourceCollection,
				  localCollection,
				  input("local-extra", "local", "Surplus", 9),
				  input("source-a", "collection", "A", 1),
				  input("local-z", "local", "Z", 3),
				  input("source-z", "collection", "Z", 1),
				  input("local-a", "local", "A", 2)}
			 )}
		};
	}

	void CheckPositionalValues(const Document &authored, std::span<const GroupSubtypeBinding> bindings = {}) {
		const std::array<std::string_view, 3> sockets{"local-a", "local-z", "local-extra"};
		for (int64_t tick : {int64_t{0}, int64_t{4}}) {
			const std::array<double, 3> expected{.125, tick == 0 ? .625 : .875, .5};
			for (size_t index = 0; index < sockets.size(); ++index) {
				auto selected = authored;
				selected.Outputs = {{"sample", std::string(sockets[index]), "value"}};
				Plan plan;
				Diagnostic diagnostic;
				const auto compiled = Compile(selected, plan, diagnostic);
				INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
				REQUIRE(compiled == Status::Ok);
				EvaluatedValue value;
				EvaluationRequest request;
				request.Tick = tick;
				GroupReplayState empty, declarations, bound;
				if (!bindings.empty()) {
					REQUIRE(RebindGroupReplay(selected, empty, 1, declarations, diagnostic) == Status::Ok);
					REQUIRE(
						BindGroupReplay(selected, bindings, declarations, 1, bound, diagnostic) == Status::Ok
					);
					request.GroupReplay = &bound;
					request.GroupAuthoringRevision = 1;
				}
				const auto evaluated = EvaluateValue(selected, plan, "sample", request, value, diagnostic);
				INFO(
					diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port
									   << " tick=" << tick
				);
				REQUIRE(evaluated == Status::Ok);
				REQUIRE(std::holds_alternative<double>(value.Data));
				CHECK(std::get<double>(value.Data) == expected[index]);
			}
		}
	}

	engine::bake::PxcxArchive Archive(const Json &graph) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = graph.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
}

TEST_CASE(
	"Collection generic instances retain local descendants and native durability",
	"[imagegraphio][collection_instances]"
) {
	auto document = NestedCollections(true);
	document.Groups[1].InstanceBase.clear();
	document.Groups[3].InstanceBase = "collection";
	const auto before = document;
	std::string failure;
	REQUIRE(
		engine::imagegraphio::detail::ResolveImportedGroupInstances(
			CollectionInstanceClasses(), document, failure
		)
	);
	CHECK(document == before);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"Enclosing Group binds matched Collection sockets without cloning local children",
	"[imagegraphio][collection_instances]"
) {
	auto document = NestedCollections(true);
	AddSocket(document, "collection", "source-input", PortDirection::Input);
	AddSocket(document, "local", "local-input", PortDirection::Input);
	AddSocket(document, "collection", "source-output", PortDirection::Output);
	AddSocket(document, "local", "local-output", PortDirection::Output);
	AddSocket(document, "local", "extra-input", PortDirection::Input);
	AddSocket(document, "local", "extra-output", PortDirection::Output);
	document.Links = {
		{"source-input", "value", "source-child", "value"},
		{"source-child", "number", "source-output", "value"},
		{"local-input", "value", "local-child", "value"},
		{"local-child", "number", "local-output", "value"}
	};
	const auto localChild = document.Nodes[1];
	const auto localPorts = document.Groups[3].Ports;
	std::string failure;
	REQUIRE(
		engine::imagegraphio::detail::ResolveImportedGroupInstances(
			CollectionInstanceClasses(), document, failure
		)
	);
	CHECK(document.Groups[3].InstanceBase == "collection");
	CHECK(document.Groups[3].Ports == localPorts);
	CHECK(document.Nodes[1] == localChild);
	const auto input = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
		return node.Id == "local-input";
	});
	REQUIRE(input != document.Nodes.end());
	CHECK(input->InstanceBase.empty());
	CHECK(input->SourceParentInputBase == "source-input");
	CHECK(
		std::find(
			document.Links.begin(),
			document.Links.end(),
			Link{"local-child", "number", "local-output", "value"}
		) != document.Links.end()
	);
	CHECK(std::none_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
		return node.Id == "local/instance/source-child";
	}));
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"Missing Collection clone stays shallow and retains its attribute base",
	"[imagegraphio][collection_instances]"
) {
	auto document = NestedCollections(false);
	std::string failure;
	REQUIRE(
		engine::imagegraphio::detail::ResolveImportedGroupInstances(
			CollectionInstanceClasses(), document, failure
		)
	);
	REQUIRE(document.Groups.size() == 4);
	const auto &copied = document.Groups.back();
	CHECK(copied.Id == "copy/instance/collection");
	CHECK(copied.ParentId == "copy");
	CHECK(copied.InstanceBase == "collection");
	CHECK(copied.Ports.empty());
	CHECK(copied.ColorDepth == 5);
	CHECK(document.Nodes.size() == 1);
}

TEST_CASE(
	"Collection instance refusal preserves the published import",
	"[imagegraphio][collection_instances][atomic]"
) {
	using namespace engine::imagegraphio;
	const auto archive = Archive(GenericCollectionSource());
	PxcxImport published;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, published, failure));
	const auto prior = published.Graph;
	const auto priorBytes = published.Source.OriginalBytes;
	auto invalid = GenericCollectionSource();
	SECTION("missing base") {
		invalid["nodes"][1]["instanceBase"] = "missing";
	}
	SECTION("base cycle") {
		invalid["nodes"][0]["instanceBase"] = "local";
	}
	SECTION("low byte bound") {
		PxcxImportOptions options;
		options.MaximumOperationBytes = 1;
		CHECK_FALSE(ImportPxcxImageGraph(archive, published, failure, options));
		CHECK(published.Graph == prior);
		CHECK(published.Source.OriginalBytes == priorBytes);
		return;
	}
	CHECK_FALSE(ImportPxcxImageGraph(Archive(invalid), published, failure));
	CHECK_FALSE(failure.empty());
	CHECK(published.Graph == prior);
	CHECK(published.Source.OriginalBytes == priorBytes);
}

TEST_CASE(
	"Generic Collection instance source edits reopen with local pixels",
	"[imagegraphio][collection_instances]"
) {
	using namespace engine::imagegraphio;
	const auto archive = Archive(GenericCollectionSource());
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
	CHECK(imported.Source.OriginalBytes == archive.OriginalBytes);
	CHECK(imported.Source.GraphJson == archive.GraphJson);
	const auto local =
		std::find_if(imported.Graph.Groups.begin(), imported.Graph.Groups.end(), [](const Group &group) {
			return group.Id == "local";
		});
	REQUIRE(local != imported.Graph.Groups.end());
	CHECK(local->InstanceBase == "base");
	CHECK(std::none_of(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const Node &node) {
		return node.Id == "local/instance/base-image";
	}));
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(imported.Graph, plan, "sink", image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{0, 0, 255, 255});
	std::vector<std::byte> saved;
	const std::array<PxcxEdit, 1> edits{PxcxNodePositionEdit{"local-image", {7, 11}}};
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, saved, diagnostic));
	engine::bake::PxcxArchive reopenedArchive;
	REQUIRE(engine::bake::ReadPxcx(saved, reopenedArchive, failure));
	PxcxImport reopened;
	REQUIRE(ImportPxcxImageGraph(reopenedArchive, reopened, failure));
	auto expected = imported.Graph;
	const auto moved = std::find_if(expected.Nodes.begin(), expected.Nodes.end(), [](const Node &node) {
		return node.Id == "local-image";
	});
	REQUIRE(moved != expected.Nodes.end());
	moved->Position = {7, 11};
	CHECK(reopened.Graph == expected);
	REQUIRE(Compile(reopened.Graph, plan, diagnostic) == Status::Ok);
	Image after;
	REQUIRE(Evaluate(reopened.Graph, plan, "sink", after, diagnostic) == Status::Ok);
	CHECK(after.Pixels == image.Pixels);
}

TEST_CASE(
	"Imported nested Collection parent aliases preserve source controls and reopen",
	"[imagegraphio][collection_instances]"
) {
	using namespace engine::imagegraphio;
	const auto archive = Archive(NestedCollectionSource());
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
	CHECK(imported.Source.OriginalBytes == archive.OriginalBytes);
	const auto input =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const Node &node) {
			return node.Id == "local-input";
		});
	REQUIRE(input != imported.Graph.Nodes.end());
	CHECK(input->InstanceBase.empty());
	CHECK(input->SourceParentInputBase == "source-input");
	const auto range =
		std::find_if(input->Values.begin(), input->Values.end(), [](const AuthoredValue &value) {
			return value.Port == "range";
		});
	REQUIRE(range != input->Values.end());
	CHECK(range->Data == engine::imagegraph::Value{Vector2{0, 2}});
	CHECK(
		std::any_of(
			imported.GroupBindings.begin(),
			imported.GroupBindings.end(),
			[](const GroupSubtypeBinding &binding) {
				return binding.NodeId == "local-input" && binding.OwnerId == "source-input" &&
					   binding.Port == "parent_value";
			}
		)
	);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	std::vector<std::byte> saved;
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, {}, saved, diagnostic));
	CHECK(saved == archive.OriginalBytes);
	engine::bake::PxcxArchive reopenedArchive;
	REQUIRE(engine::bake::ReadPxcx(saved, reopenedArchive, failure));
	PxcxImport reopened;
	REQUIRE(ImportPxcxImageGraph(reopenedArchive, reopened, failure));
	CHECK(reopened.Graph == imported.Graph);
	CHECK(reopened.GroupBindings == imported.GroupBindings);
}

TEST_CASE(
	"Shallow Collection with absent parent sockets refuses without inventing children",
	"[imagegraphio][collection_instances]"
) {
	auto document = NestedCollections(false);
	AddSocket(document, "collection", "source-input", PortDirection::Input);
	std::string failure;
	CHECK_FALSE(
		engine::imagegraphio::detail::ResolveImportedGroupInstances(
			CollectionInstanceClasses(), document, failure
		)
	);
	CHECK(failure.find("no child sockets") != std::string::npos);
	CHECK(document.Groups.size() == 3);
	CHECK(std::none_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
		return node.GroupId == "copy/instance/collection";
	}));
}

TEST_CASE(
	"Imported Collection parent aliases follow saved socket indices through every reopen",
	"[imagegraphio][collection_instances]"
) {
	using namespace engine::imagegraphio;
	const auto archive = Archive(PositionalCollectionSource());
	PxcxImport imported;
	std::string failure;
	const bool accepted = ImportPxcxImageGraph(archive, imported, failure);
	INFO(failure);
	REQUIRE(accepted);
	CHECK(imported.Source.OriginalBytes == archive.OriginalBytes);
	const auto local =
		std::find_if(imported.Graph.Groups.begin(), imported.Graph.Groups.end(), [](const Group &group) {
			return group.Id == "local";
		});
	REQUIRE(local != imported.Graph.Groups.end());
	REQUIRE(local->Ports.size() == 3);
	CHECK(local->Ports[0].ControlNodeId == "local-a");
	CHECK(local->Ports[1].ControlNodeId == "local-z");
	CHECK(local->Ports[2].ControlNodeId == "local-extra");
	const std::array<std::pair<std::string_view, std::string_view>, 3> aliases{
		{{"local-a", "source-z"}, {"local-z", "source-a"}, {"local-extra", ""}}
	};
	for (const auto &[target, owner] : aliases) {
		const auto input =
			std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [&](const Node &node) {
				return node.Id == target;
			});
		REQUIRE(input != imported.Graph.Nodes.end());
		CHECK(input->InstanceBase.empty());
		CHECK(input->SourceParentInputBase == owner);
	}
	CheckPositionalValues(imported.Graph);
	CheckPositionalValues(imported.Graph, imported.GroupBindings);
	Diagnostic diagnostic;
	Document nativeReopened;
	REQUIRE(Read(Write(imported.Graph), nativeReopened, diagnostic) == Status::Ok);
	CHECK(nativeReopened == imported.Graph);
	CheckPositionalValues(nativeReopened);
	std::vector<std::byte> saved;
	const std::array<PxcxEdit, 1> edits{PxcxNodePositionEdit{"local-a", {17, 23}}};
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, saved, diagnostic));
	engine::bake::PxcxArchive reopenedArchive;
	REQUIRE(engine::bake::ReadPxcx(saved, reopenedArchive, failure));
	PxcxImport reopened;
	REQUIRE(ImportPxcxImageGraph(reopenedArchive, reopened, failure));
	const auto savedGraph = Json::parse(reopenedArchive.GraphJson.c_str());
	CHECK(
		savedGraph["nodes"][3]["attri"]["custom_input_list"] ==
		Json::array({"local-a", "local-z", "local-extra"})
	);
	CHECK(savedGraph["nodes"][2]["inputs"][0]["r"]["d"] == .125);
	CHECK(savedGraph["nodes"][2]["inputs"][1]["r"].size() == 2);
	CheckPositionalValues(reopened.Graph);
	CheckPositionalValues(reopened.Graph, reopened.GroupBindings);
	const auto first =
		std::find_if(reopened.Graph.Nodes.begin(), reopened.Graph.Nodes.end(), [](const Node &node) {
			return node.Id == "local-a";
		});
	REQUIRE(first != reopened.Graph.Nodes.end());
	CHECK(first->Position == Vector2{17, 23});
	CHECK(first->SourceParentInputBase == "source-z");
	Document secondNativeReopened;
	REQUIRE(Read(Write(reopened.Graph), secondNativeReopened, diagnostic) == Status::Ok);
	CHECK(secondNativeReopened == reopened.Graph);
	CheckPositionalValues(secondNativeReopened);
}

TEST_CASE(
	"Missing nested Collection clones preserve scheduling and purity through native and PXC saves",
	"[imagegraphio][collection_instances][clone_flags]"
) {
	auto outer = SourceNode("outer", "Node_Group");
	auto copy = SourceNode("copy", "Node_Group");
	copy["instanceBase"] = "outer";
	auto collection = SourceNode("collection", "Node_Collection");
	collection["group"] = "outer";
	collection["render"] = false;
	collection["attri"] = {
		{"pure_function", false},
		{"custom_input_list", Json::array()},
		{"custom_output_list", Json::array()},
		{"input_display_list", Json::array({Json{{"future_display", {7, "opaque"}}}})}
	};
	collection["attriTool"] = {{"future_tool", {{"value", "retained"}}}};
	const Json source = {{"nodes", Json::array({outer, copy, collection})}};
	const auto archive = Archive(source);
	PxcxImport imported;
	std::string failure;
	INFO(failure);
	REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
	const auto check = [](const Document &document) {
		const auto group = std::find_if(document.Groups.begin(), document.Groups.end(), [](const Group &g) {
			return g.Id == "copy/instance/collection";
		});
		REQUIRE(group != document.Groups.end());
		CHECK(group->ParentId == "copy");
		CHECK(group->InstanceBase == "collection");
		CHECK(group->Ports.empty());
		CHECK_FALSE(group->RenderActive);
		CHECK_FALSE(group->PureFunction);
	};
	check(imported.Graph);
	Document native;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Read(Write(imported.Graph), native, diagnostic) == Status::Ok);
	CHECK(native == imported.Graph);
	check(native);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic));
	CHECK(bytes == archive.OriginalBytes);
	engine::bake::PxcxArchive saved;
	REQUIRE(engine::bake::ReadPxcx(bytes, saved, failure));
	PxcxImport reopened;
	REQUIRE(ImportPxcxImageGraph(saved, reopened, failure));
	check(reopened.Graph);
	CHECK(Json::parse(saved.GraphJson.c_str()) == source);
	CHECK(reopened.Source.OriginalBytes == archive.OriginalBytes);
}

TEST_CASE(
	"Matched nested Collections retain local scheduling and purity flags",
	"[imagegraphio][collection_instances][clone_flags]"
) {
	auto document = NestedCollections(true);
	document.Groups[2].RenderActive = false;
	document.Groups[2].PureFunction = false;
	const auto local = document.Groups[3];
	std::string failure;
	INFO(failure);
	REQUIRE(
		engine::imagegraphio::detail::ResolveImportedGroupInstances(
			CollectionInstanceClasses(), document, failure
		)
	);
	CHECK(document.Groups[3].RenderActive == local.RenderActive);
	CHECK(document.Groups[3].PureFunction == local.PureFunction);
	CHECK(document.Groups[3].InstanceBase == "collection");
}

TEST_CASE(
	"Missing Pixel Builder owned scopes copy authored scheduling and purity flags",
	"[imagegraphio][collection_instances][pixel_builder][clone_flags]"
) {
	Document document;
	document.FormatVersion = 10;
	document.Nodes = {{"builder", "pc.pixel_builder", "outer", {}, {}}};
	Group outer{"outer", "Outer"}, copy{"copy", "Copy"}, builder{"builder", "Builder"};
	copy.InstanceBase = "outer";
	builder.ParentId = "outer";
	builder.OwnerNodeId = "builder";
	builder.RenderActive = false;
	builder.PureFunction = false;
	document.Groups = {outer, copy, builder};
	Json root = {
		{"nodes",
		 Json::array(
			 {SourceNode("outer", "Node_Group"),
			  SourceNode("copy", "Node_Group"),
			  SourceNode("builder", "Node_Pixel_Builder")}
		 )}
	};
	std::string failure;
	INFO(failure);
	REQUIRE(engine::imagegraphio::detail::ResolveImportedGroupInstances(root, document, failure));
	const auto scope = std::find_if(document.Groups.begin(), document.Groups.end(), [](const Group &g) {
		return g.OwnerNodeId == "copy/instance/builder";
	});
	REQUIRE(scope != document.Groups.end());
	CHECK(scope->ParentId == "copy");
	CHECK(scope->InstanceBase.empty());
	CHECK(scope->Ports.empty());
	CHECK_FALSE(scope->RenderActive);
	CHECK_FALSE(scope->PureFunction);
	Document restored;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}
