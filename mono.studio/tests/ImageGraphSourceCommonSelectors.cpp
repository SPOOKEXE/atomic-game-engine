#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nodegraph/Graph.hpp>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph.source_common_selectors")
TEST_DEPENDS("studio.imagegraph")

TEST_CASE(
	"Opaque source common sockets remain distinct through canvas save "
	"and reopen",
	"[studio][imagegraph]"
) {
	using namespace engine::imagegraph;
	engine::bake::PxcxArchive archive;
	archive.Nodes = {{"source", "vendor.unknown", 0, 0}, {"sink", "vendor.unknown", 16, 16}};
	archive.Links = {
		{"source", 0, "sink", 0},
		{"source", 0, "sink", 1, -2, true},
		{"source", 0, "sink", 2, -3, true},
		{"source", 0, "sink", 3, -4},
		{"source", 1, "sink", 4, -4},
		{"source", 7, "sink", 5, -3},
		{"source", 0, "sink", 6, -5},
		{"source", 0, "sink", 2, {}, false, true}
	};
	studio::PxcxImageGraphProjection projection;
	std::string error;
	REQUIRE(studio::ProjectPxcxImageGraph(archive, projection, error));
	REQUIRE(projection.Graph.Links.size() == archive.Links.size());
	const std::vector<std::string> ports = {
		"output-0",
		"pxcx.update_in_trigger",
		"pxcx.updated_out_trigger",
		"pxcx.metadata.0",
		"pxcx.metadata.1",
		"pxcx.updated_out_trigger",
		"output-0"
	};
	for (size_t index = 0; index < ports.size(); ++index)
		CHECK(projection.Graph.Links[index].FromPort == ports[index]);
	CHECK(projection.Graph.Links.back().FromPort == "output-0");
	CHECK(projection.Graph.Links.back().ToPort == "pxcx.update_in_trigger");
	CHECK(projection.Graph.Links[2].ToPort == "input-2");
	projection.Graph.Outputs = {{"result", "source", "output-0"}};

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	REQUIRE(studio::LoadImageGraphCanvas(projection.Graph, canvas, ids, error));
	CHECK(canvas.Links().size() == archive.Links.size());
	CHECK(ids.UnmappedLinks.empty());
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, projection.Graph, ids, saved, error));
	CHECK(saved.Links == projection.Graph.Links);
	CHECK(saved.Nodes == projection.Graph.Nodes);
	Document reopened;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(saved), reopened, diagnostic) == Status::Ok);
	CHECK(reopened.Links == saved.Links);
	Plan plan;
	CHECK(Compile(reopened, plan, diagnostic) == Status::UnknownNode);
}

TEST_CASE("Opaque source projection refuses mismatched trigger sentinel atomically", "[studio][imagegraph]") {
	engine::bake::PxcxArchive archive;
	archive.Nodes = {{"source", "vendor.unknown", 0, 0}, {"sink", "vendor.unknown", 16, 16}};
	archive.Links = {{"source", 0, "sink", 0}};
	studio::PxcxImageGraphProjection projection;
	std::string error;
	REQUIRE(studio::ProjectPxcxImageGraph(archive, projection, error));
	const auto before = projection.Graph;
	archive.Links.front().SourceTriggerIndexMinusOne = true;
	SECTION("Absent tag") {}
	SECTION("Metadata tag") {
		archive.Links.front().FromTag = -4;
	}
	SECTION("Nonzero positional index") {
		archive.Links.front().FromTag = -3;
		archive.Links.front().FromIndex = 1;
	}
	CHECK_FALSE(studio::ProjectPxcxImageGraph(archive, projection, error));
	CHECK(error == "PXCX common trigger index has no matching source selector");
	CHECK(projection.Graph == before);
}

TEST_CASE(
	"Native source common ports project on nodes and group views "
	"independently of visibility",
	"[studio][imagegraph]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 11;
	Node native;
	native.Id = "native-number";
	native.Type = "value.number";
	native.Position = {20.0, 30.0};
	document.Nodes.push_back(native);
	Node collisionNative = native;
	collisionNative.Id = "collision-native";
	document.Nodes.push_back(collisionNative);
	Group group;
	group.Id = "source-group";
	group.Name = "Collection";
	group.SourceInternalName = "Node_Collection";
	group.SourcePosition = {80.0, 90.0};
	document.Groups.push_back(group);
	Group collisionGroup = group;
	collisionGroup.Id = "second-group";
	document.Groups.push_back(collisionGroup);
	SourceCommonOwnerRecord nodeOwner;
	nodeOwner.SourceOwnerId = "source-number";
	nodeOwner.SourceType = "Node_Number_Simple";
	nodeOwner.NativeOwnerKind = SourceCommonNativeOwnerKind::Node;
	nodeOwner.NativeOwnerId = native.Id;
	SourceCommonOwnerRecord groupOwner;
	groupOwner.SourceOwnerId = collisionGroup.Id;
	groupOwner.SourceType = "Node_Collection";
	groupOwner.NativeOwnerKind = SourceCommonNativeOwnerKind::Group;
	groupOwner.NativeOwnerId = group.Id;
	SourceCommonOwnerRecord collisionGroupOwner = groupOwner;
	collisionGroupOwner.SourceOwnerId = "source-collection";
	collisionGroupOwner.NativeOwnerId = collisionGroup.Id;
	SourceCommonOwnerRecord collisionOwner = nodeOwner;
	collisionOwner.SourceOwnerId = native.Id;
	collisionOwner.NativeOwnerId = collisionNative.Id;
	nodeOwner.UpdateAnimatorOwnerId = nodeOwner.SourceOwnerId;
	nodeOwner.UpdateAnimatorPort = "native:animator:1";
	groupOwner.UpdateAnimatorOwnerId = groupOwner.SourceOwnerId;
	groupOwner.UpdateAnimatorPort = "native:animator:2";
	collisionGroupOwner.UpdateAnimatorOwnerId = collisionGroupOwner.SourceOwnerId;
	collisionGroupOwner.UpdateAnimatorPort = "native:animator:4";
	collisionOwner.UpdateAnimatorOwnerId = collisionOwner.SourceOwnerId;
	collisionOwner.UpdateAnimatorPort = "native:animator:3";
	document.SourceCommonOwners = {nodeOwner, groupOwner, collisionOwner, collisionGroupOwner};
	document.SourceAnimators.emplace();
	const auto addAnimator = [&](const SourceCommonOwnerRecord &owner) {
		DetachedSourceAnimator animator;
		animator.OwnerId = owner.UpdateAnimatorOwnerId;
		animator.Id = owner.UpdateAnimatorPort;
		animator.OriginalPort = "pxcx.update_in_trigger";
		animator.Type = ValueType::Boolean;
		document.SourceAnimators->Detached.push_back(animator);
		GroupSubtypeOverlay payload;
		payload.NodeId = owner.UpdateAnimatorOwnerId;
		payload.Port = owner.UpdateAnimatorPort;
		payload.Fixed = false;
		document.SourceAnimators->DetachedValues.push_back(payload);
	};
	addAnimator(nodeOwner);
	addAnimator(groupOwner);
	addAnimator(collisionOwner);
	addAnimator(collisionGroupOwner);
	document.Links.push_back(
		{collisionGroupOwner.NativeOwnerId,
		 "pxcx.updated_out_trigger",
		 nodeOwner.NativeOwnerId,
		 "pxcx.update_in_trigger"}
	);
	Plan commonPlan;
	Diagnostic commonDiagnostic;
	REQUIRE(CompileSourceCommonRuntime(document, commonPlan, commonDiagnostic) == Status::Ok);

	studio::RegisterImageGraphNodeTypes();
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	CHECK(canvas.Links().size() == 1);
	CHECK(ids.UnmappedLinks.empty());
	const auto hasPort = [&](nodegraph::NodeId id, std::string_view port, bool output) {
		const auto node = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [&](const auto &item) {
			return item.Id == id;
		});
		if (node == canvas.Nodes().end()) return false;
		const auto *type = nodegraph::NodeTypes::Find(node->Type);
		const auto ports = output ? (node->OutputPorts ? &*node->OutputPorts
									 : type			   ? &type->Outputs
													   : nullptr)
								  : (node->InputPorts ? &*node->InputPorts
									 : type			  ? &type->Inputs
													  : nullptr);
		return ports &&
			   std::any_of(ports->begin(), ports->end(), [&](const auto &item) { return item.Name == port; });
	};
	const auto portVisible = [&](nodegraph::NodeId id, std::string_view port, bool output) {
		const auto node = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [&](const auto &item) {
			return item.Id == id;
		});
		if (node == canvas.Nodes().end()) return false;
		const auto &ports = output ? node->OutputPorts : node->InputPorts;
		if (!ports) return false;
		const auto found =
			std::find_if(ports->begin(), ports->end(), [&](const auto &item) { return item.Name == port; });
		return found != ports->end() && found->Visible;
	};
	const auto nativeCanvas = ids.ToCanvas.at(native.Id);
	const auto groupView = ids.SourceCommonGroupViews.at(groupOwner.SourceOwnerId);
	for (const auto id : {nativeCanvas, groupView}) {
		CHECK(hasPort(id, "pxcx.update_in_trigger", false));
		CHECK(hasPort(id, "pxcx.updated_out_trigger", true));
		CHECK(hasPort(id, "pxcx.metadata.0", true));
		CHECK(hasPort(id, "pxcx.metadata.1", true));
		CHECK_FALSE(portVisible(id, "pxcx.update_in_trigger", false));
		CHECK_FALSE(portVisible(id, "pxcx.updated_out_trigger", true));
		CHECK_FALSE(portVisible(id, "pxcx.metadata.0", true));
		CHECK_FALSE(portVisible(id, "pxcx.metadata.1", true));
	}
	const auto hiddenUpdate =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [&](const auto &item) {
			return item.Id == nativeCanvas;
		});
	REQUIRE(hiddenUpdate != canvas.Nodes().end());
	REQUIRE(hiddenUpdate->InputPorts.has_value());
	const auto updatePort = std::find_if(
		hiddenUpdate->InputPorts->begin(), hiddenUpdate->InputPorts->end(), [](const auto &item) {
			return item.Name == "pxcx.update_in_trigger";
		}
	);
	REQUIRE(updatePort != hiddenUpdate->InputPorts->end());
	CHECK_FALSE(updatePort->Visible);
	CHECK_FALSE(document.SourceCommonOwners.front().ShowUpdateTrigger);
	CHECK_FALSE(document.SourceCommonOwners.front().OutMeta);
	CHECK_FALSE(document.SourceCommonOwners.back().ShowUpdateTrigger);
	CHECK_FALSE(document.SourceCommonOwners.back().OutMeta);

	CHECK(canvas.GroupOf(groupView) == ids.GroupsToCanvas.at(group.Id));
	const auto moved = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [&](const auto &item) {
		return item.Id == groupView;
	});
	REQUIRE(moved != canvas.Nodes().end());
	moved->X = 111.5f;
	moved->Y = 222.25f;
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, saved, error));
	CHECK(saved.Nodes.size() == 2);
	CHECK(saved.Nodes.front().Id == native.Id);
	CHECK(saved.Links == document.Links);
	CHECK(saved.Groups.front().SourcePosition.X == 111.5);
	CHECK(saved.Groups.front().SourcePosition.Y == 222.25);
	CHECK(saved.SourceCommonOwners == document.SourceCommonOwners);
	Plan savedPlan;
	REQUIRE(CompileSourceCommonRuntime(saved, savedPlan, commonDiagnostic) == Status::Ok);
	Document reopened;
	REQUIRE(Read(Write(saved), reopened, commonDiagnostic) == Status::Ok);
	Plan reopenedPlan;
	REQUIRE(CompileSourceCommonRuntime(reopened, reopenedPlan, commonDiagnostic) == Status::Ok);
}

TEST_CASE("Deleting a native source owner retires its dependent owners and local animator payloads") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 11;
	for (const char *id : {"base-native-node", "dependent-node", "base-node", "survivor-two-node"}) {
		Node node;
		node.Id = id;
		node.Type = "value.number";
		document.Nodes.push_back(node);
	}
	size_t writerIndex = 0;
	auto addOwner = [&](std::string id, std::string nativeId, std::string base = {}) {
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = id;
		owner.SourceType = "Node_Number_Simple";
		owner.NativeOwnerId = nativeId;
		owner.InstanceBase = std::move(base);
		owner.UpdateAnimatorOwnerId = owner.SourceOwnerId;
		owner.UpdateAnimatorPort = "native:animator:" + std::to_string(++writerIndex);
		document.SourceCommonOwners.push_back(owner);
		if (!document.SourceAnimators) document.SourceAnimators.emplace();
		DetachedSourceAnimator writer;
		writer.OwnerId = owner.SourceOwnerId;
		writer.Id = owner.UpdateAnimatorPort;
		writer.OriginalPort = "pxcx.update_in_trigger";
		writer.Type = ValueType::Boolean;
		document.SourceAnimators->Detached.push_back(writer);
		GroupSubtypeOverlay payload;
		payload.NodeId = writer.OwnerId;
		payload.Port = writer.Id;
		payload.Fixed = false;
		document.SourceAnimators->DetachedValues.push_back(payload);
	};
	addOwner("base-node", "base-native-node");
	addOwner("dependent", "dependent-node", "base-node");
	addOwner("survivor", "base-node");
	addOwner("another-survivor", "survivor-two-node");
	document.Links.push_back(
		{"base-native-node", "pxcx.updated_out_trigger", "dependent-node", "pxcx.update_in_trigger"}
	);
	document.Links.push_back(
		{"base-node", "pxcx.updated_out_trigger", "survivor-two-node", "pxcx.update_in_trigger"}
	);

	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(CompileSourceCommonRuntime(document, plan, diagnostic) == Status::Ok);
	studio::RegisterImageGraphNodeTypes();
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	REQUIRE(canvas.Remove(ids.ToCanvas.at("base-native-node")));
	Document saved = document;
	saved.Nodes.front().Position.X = 444.0;
	const auto savedBeforeRefusal = Write(saved);
	CHECK_FALSE(studio::SaveImageGraphCanvas(canvas, document, ids, saved, error));
	CHECK(error.find("detach or delete dependent owners first") != std::string::npos);
	CHECK(Write(saved) == savedBeforeRefusal);
	REQUIRE(canvas.Remove(ids.ToCanvas.at("dependent-node")));
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, saved, error));
	REQUIRE(saved.SourceCommonOwners.size() == 2);
	CHECK(saved.SourceCommonOwners.front().SourceOwnerId == "survivor");
	CHECK(saved.SourceCommonOwners.back().SourceOwnerId == "another-survivor");
	REQUIRE(saved.Links.size() == 1);
	CHECK(saved.Links.front().FromNode == "base-node");
	CHECK(saved.Links.front().ToNode == "survivor-two-node");
	REQUIRE(static_cast<bool>(saved.SourceAnimators));
	REQUIRE(saved.SourceAnimators->Detached.size() == 2);
	CHECK(saved.SourceAnimators->Detached.front().OwnerId == "survivor");
	CHECK(saved.SourceAnimators->Detached.back().OwnerId == "another-survivor");
	REQUIRE(saved.SourceAnimators->DetachedValues.size() == 2);
	CHECK(saved.SourceAnimators->DetachedValues.front().NodeId == "survivor");
	CHECK(saved.SourceAnimators->DetachedValues.back().NodeId == "another-survivor");
	Plan savedPlan;
	REQUIRE(CompileSourceCommonRuntime(saved, savedPlan, diagnostic) == Status::Ok);
	Document reloaded;
	REQUIRE(Read(Write(saved), reloaded, diagnostic) == Status::Ok);
	Plan reloadedPlan;
	REQUIRE(CompileSourceCommonRuntime(reloaded, reloadedPlan, diagnostic) == Status::Ok);
	REQUIRE(reloaded.SourceCommonOwners.size() == 2);
	CHECK(reloaded.SourceCommonOwners.front().SourceOwnerId == "survivor");
	CHECK(reloaded.SourceCommonOwners.back().SourceOwnerId == "another-survivor");
}

TEST_CASE(
	"Opaque common Update destination refuses invalid fact indices atomically", "[studio][imagegraph]"
) {
	engine::bake::PxcxArchive archive;
	archive.Nodes = {{"source", "vendor.unknown", 0, 0}, {"sink", "vendor.unknown", 16, 16}};
	archive.Links = {{"source", 0, "sink", 0}};
	studio::PxcxImageGraphProjection projection;
	std::string error;
	REQUIRE(studio::ProjectPxcxImageGraph(archive, projection, error));
	const auto before = projection.Graph;
	archive.Links.front().DestinationUpdateTrigger = true;
	CHECK_FALSE(studio::ProjectPxcxImageGraph(archive, projection, error));
	CHECK(error == "PXCX common Update destination has no matching inspector index");
	CHECK(projection.Graph == before);
}
