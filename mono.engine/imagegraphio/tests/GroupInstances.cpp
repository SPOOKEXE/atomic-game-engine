#include "GroupInstances.hpp"

#include "ImportBudget.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_ID("engine.imagegraphio.group-instances")

using namespace engine::imagegraph;
using namespace engine::imagegraphio::detail;

namespace {
	Document InstanceSource() {
		Document document;
		document.FormatVersion = 9;
		Group base;
		base.Id = "base";
		Group instance;
		instance.Id = "instance";
		instance.InstanceBase = base.Id;
		document.Groups = {base, instance};
		document.Nodes.push_back({"number", "pc.number", "base", {}, {{"value", 10.7}}});
		Keyframe key;
		key.NodeId = "number";
		key.Port = "value";
		key.Data = 12.5;
		key.Kind = KeyframeKind::Adder;
		key.Interpolation = "source";
		key.Ease = KeyframeEase{};
		key.SourceDriver = KeyframeLinearDriver{2.0};
		document.Keyframes.push_back(key);
		document.Tracks.push_back({"number", "value", "loop"});
		return document;
	}
	const nlohmann::json Source = {{"nodes", {{{"id", "number"}, {"type", "Node_Number"}}}}};
}

TEST_CASE(
	"Instance reconciliation clones local animator metadata for later overrides",
	"[imagegraphio][group_instances]"
) {
	auto document = InstanceSource();
	document.Nodes.front().SourceAnimatedInputs = {"value"};
	const auto original = document.Keyframes.front();
	std::string failure;
	REQUIRE(ResolveImportedGroupInstances(Source, document, failure));
	REQUIRE(failure.empty());
	REQUIRE(document.Nodes.size() == 2);
	const auto &copied = document.Nodes.back();
	CHECK(copied.Id == "instance/instance/number");
	CHECK(copied.GroupId == "instance");
	CHECK(copied.InstanceBase == "number");
	CHECK(copied.Values == document.Nodes.front().Values);
	CHECK(copied.SourceAnimatedInputs == document.Nodes.front().SourceAnimatedInputs);
	REQUIRE(document.Keyframes.size() == 2);
	auto expected = original;
	expected.NodeId = copied.Id;
	CHECK(document.Keyframes.back() == expected);
	CHECK(document.Keyframes.front() == original);
	REQUIRE(document.Tracks.size() == 2);
	CHECK(document.Tracks.back().NodeId == copied.Id);
	CHECK(document.Tracks.back().End == document.Tracks.front().End);
}

TEST_CASE(
	"Instance map allocations admit previous live document before any reconciliation mutation",
	"[imagegraphio][group_instances]"
) {
	auto document = InstanceSource();
	const auto lastGood = document;
	const auto retained = DocumentRetainedPayloadBytes(document);
	REQUIRE(retained.has_value());
	REQUIRE(*retained < Limits::MaximumEvaluationBytes - 1);
	std::string failure;
	// Candidate and retained prior result occupy the entire operation allowance.
	// Even the first source-class map node must refuse before cloning or replacing anything.
	CHECK_FALSE(ResolveImportedGroupInstances(Source, document, failure, 1, *retained + 1));
	CHECK(document == lastGood);
	CHECK(failure == "instance operation exceeds allocation budget");
}

TEST_CASE("Import allocator charges actual rebound map storage", "[imagegraphio][group_instances]") {
	using Pair = std::pair<const std::string_view, std::string_view>;
	ImportBudget budget(sizeof(Pair));
	std::map<std::string_view, std::string_view, std::less<>, ImportAllocator<Pair>> map(
		std::less<>{}, ImportAllocator<Pair>{budget}
	);
	CHECK_THROWS_AS(map.emplace("number", "Node_Number"), std::bad_alloc);
	CHECK(map.empty());
	CHECK(budget.Available() == sizeof(Pair));
}

TEST_CASE(
	"Surplus instance boundary controls remove their animation references only",
	"[imagegraphio][group_instances]"
) {
	auto document = InstanceSource();
	// Base has no custom sockets. Its target has surplus animated sockets in both directions.
	for (const auto direction : {PortDirection::Input, PortDirection::Output}) {
		const std::string id = direction == PortDirection::Input ? "extra-input" : "extra-output";
		const std::string type = direction == PortDirection::Input ? "pc.group_input" : "pc.group_output";
		document.Nodes.push_back({id, type, "instance", {}, {}});
		document.Groups.back().Ports.push_back({id, id + "/parent", direction, id});
		document.Junctions.push_back({id + "/parent", "instance", ValueType::Any, 0.0});
		Keyframe key;
		key.NodeId = id;
		key.Port = "name";
		key.Data = std::string("Animated label");
		document.Keyframes.push_back(key);
		document.Tracks.push_back({id, "name", "hold"});
	}
	const auto originalKey = document.Keyframes.front();
	const auto originalTrack = document.Tracks.front();
	std::string failure;
	REQUIRE(ResolveImportedGroupInstances(Source, document, failure));
	CHECK(document.Groups.back().Ports.empty());
	CHECK(document.Junctions.empty());
	REQUIRE(document.Nodes.size() == 2);
	REQUIRE(document.Keyframes.size() == 2);
	REQUIRE(document.Tracks.size() == 2);
	CHECK(document.Keyframes.front() == originalKey);
	CHECK(document.Tracks.front() == originalTrack);
	CHECK(document.Keyframes.back().NodeId == "instance/instance/number");
	CHECK(document.Tracks.back().NodeId == "instance/instance/number");
	document.Outputs = {{"result", "instance/instance/number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	{
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		CHECK(status == Status::Ok);
	}
}

TEST_CASE(
	"Missing instance boundary admits both long group-name copies before publication",
	"[imagegraphio][group_instances]"
) {
	constexpr size_t nameLength = 262144;
	Document document;
	document.FormatVersion = 9;
	Group base;
	base.Id = "b";
	base.Ports = {{"i", "parent", PortDirection::Input, "i"}};
	Group instance;
	instance.Id.assign(nameLength, 'g');
	instance.InstanceBase = "b";
	document.Groups = {base, instance};
	document.Nodes = {{"i", "pc.group_input", "b", {}, {}}};
	document.Junctions = {{"parent", "b", ValueType::Any, 0.0}};
	const auto unchanged = document;
	const auto retained = DocumentRetainedPayloadBytes(document);
	REQUIRE(retained.has_value());
	const nlohmann::json source = {{"nodes", {{{"id", "i"}, {"type", "Node_Group_Input"}}}}};
	std::string failure;
	// This allowance fits the old pair of temp names and its incomplete boundary-name charge,
	// but cannot also retain both complete GroupId copies and the ordered socket copies.
	REQUIRE_FALSE(ResolveImportedGroupInstances(source, document, failure, 0, *retained + 7 * nameLength));
	CHECK(document == unchanged);
	CHECK(document.Nodes.size() == 1);
	CHECK(document.Junctions.size() == 1);
	CHECK(document.Groups.back().Ports.empty());
	CHECK(failure == "instance boundary clone exceeds document bounds before copying");
}

TEST_CASE(
	"Surplus instance collections deactivate the complete nested executable subtree",
	"[imagegraphio][group_instances]"
) {
	auto document = InstanceSource();
	Group surplus;
	surplus.Id = "surplus";
	surplus.ParentId = "instance";
	Group nested;
	nested.Id = "nested";
	nested.ParentId = "surplus";
	document.Groups.push_back(surplus);
	document.Groups.push_back(nested);
	document.Nodes.push_back({"extra", "pc.number", "nested", {}, {{"value", 44.0}}});
	document.Nodes.push_back({"extra-input", "pc.group_input", "surplus", {}, {}});
	document.Groups[2].Ports.push_back({"extra-input", "extra/parent", PortDirection::Input, "extra-input"});
	document.Junctions.push_back({"extra/parent", "surplus", ValueType::Any, -1.0});
	document.Links.push_back({"extra/parent", "value", "extra-input", "parent_value"});
	Keyframe key;
	key.NodeId = "extra";
	key.Port = "value";
	key.Data = 55.0;
	document.Keyframes.push_back(key);
	document.Tracks.push_back({"extra", "value", "hold"});
	key.NodeId = "extra-input";
	key.Port = "name";
	key.Data = std::string("Unused input");
	document.Keyframes.push_back(key);
	document.Tracks.push_back({"extra-input", "name", "hold"});
	document.Outputs = {{"inactive-result", "extra", "number"}};
	auto source = Source;
	source["nodes"].push_back({{"id", "surplus"}, {"type", "Node_Group"}});
	source["nodes"].push_back({{"id", "nested"}, {"type", "Node_Group"}});
	source["nodes"].push_back({{"id", "extra"}, {"type", "Node_Number"}});
	source["nodes"].push_back({{"id", "extra-input"}, {"type", "Node_Group_Input"}});
	const auto originalSource = source;
	std::string failure;
	const bool reconciled = ResolveImportedGroupInstances(source, document, failure);
	INFO(failure);
	REQUIRE(reconciled);
	CHECK(source == originalSource);
	REQUIRE(document.Groups.size() == 2);
	CHECK(document.Junctions.empty());
	CHECK(document.Links.empty());
	CHECK(document.Outputs.empty());
	REQUIRE(document.Nodes.size() == 2);
	REQUIRE(document.Keyframes.size() == 2);
	REQUIRE(document.Tracks.size() == 2);
	CHECK(document.Keyframes.back().NodeId == "instance/instance/number");
	document.Outputs = {{"result", "instance/instance/number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"Prebinding projection retains saved local topology and clears "
	"execution aliases",
	"[imagegraphio][group_instances]"
) {
	auto document = InstanceSource();
	document.Nodes.push_back({"saved", "pc.number", "instance", {}, {{"value", 3.0}}});
	document.Nodes.back().InstanceBase = "number";
	const auto original = document;
	const auto bytes = DocumentRetainedPayloadBytes(document);
	REQUIRE(bytes.has_value());
	ImportBudget budget(Limits::MaximumEvaluationBytes);
	std::optional<Document> snapshot;
	std::string failure;
	REQUIRE(CaptureGroupPrebinding(document, snapshot, *bytes, budget, failure));
	REQUIRE(snapshot.has_value());
	CHECK(document == original);
	CHECK(snapshot->Groups.back().InstanceBase.empty());
	CHECK(snapshot->Nodes.back().InstanceBase.empty());
	CHECK(snapshot->Nodes.back().Values == original.Nodes.back().Values);
	CHECK(snapshot->Keyframes == original.Keyframes);
	CHECK(budget.Available() == Limits::MaximumEvaluationBytes - *bytes);
}
TEST_CASE(
	"Prebinding clone admits candidate prior and saved projection before "
	"copying",
	"[imagegraphio][group_instances]"
) {
	const auto document = InstanceSource();
	const auto bytes = DocumentRetainedPayloadBytes(document);
	REQUIRE(bytes.has_value());
	std::string failure;
	std::optional<Document> snapshot;
	ImportBudget exact(3 * *bytes);
	REQUIRE(CaptureGroupPrebinding(document, snapshot, *bytes, exact, failure));
	REQUIRE(snapshot.has_value());
	const auto retained = *snapshot;
	ImportBudget below(3 * *bytes - 1);
	CHECK_FALSE(CaptureGroupPrebinding(document, snapshot, *bytes, below, failure));
	CHECK(*snapshot == retained);
	CHECK(below.Available() == 3 * *bytes - 1);
	CHECK(failure == "saved local Group projection exceeds overlapping operation bounds");
}

TEST_CASE(
	"Missing instance Group control inherits empty animator mode with long identities",
	"[imagegraphio][group_instances]"
) {
	Document document;
	document.FormatVersion = 9;
	Group base{"base", "Base"};
	base.Ports = {{"input", "parent", PortDirection::Input, "input"}};
	const std::string instanceId(4096, 'g');
	Group instance{instanceId, "Instance"};
	instance.InstanceBase = base.Id;
	document.Groups = {base, instance};
	document.Nodes = {{"input", "pc.group_input", "base", {}, {}}};
	document.Nodes.front().SourceAnimatedInputs = {"subtype"};
	document.Junctions = {{"parent", "base", ValueType::Any, 0.0}};
	const nlohmann::json source = {{"nodes", {{{"id", "input"}, {"type", "Node_Group_Input"}}}}};
	std::string failure;
	const bool accepted = ResolveImportedGroupInstances(source, document, failure);
	INFO(failure);
	REQUIRE(accepted);
	REQUIRE(document.Nodes.size() == 2);
	const auto &copy = document.Nodes.back();
	CHECK(copy.SourceAnimatedInputs == std::vector<std::string>{"subtype"});
	CHECK(copy.InstanceBase == "input");
	CHECK(copy.GroupId == instanceId);
	CHECK(copy.GroupId.capacity() == std::string(instanceId).capacity());
	CHECK(copy.Id.capacity() == std::string(copy.Id).capacity());
	CHECK(document.Keyframes.empty());
	CHECK(document.Tracks.empty());
}
