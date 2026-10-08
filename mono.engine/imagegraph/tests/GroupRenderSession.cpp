#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.group_render_active")
using namespace engine::imagegraph;

namespace {
	Document GroupGraph(bool active = true) {
		Document document;
		document.FormatVersion = 10;
		document.Nodes = {
			{"source", "pc.number_simple", "group", {}, {{"value", 3.0}}},
			{"control", "pc.group_output", "group", {}, {}},
			{"consumer", "pc.number_simple", "", {}, {{"value", 99.0}}}
		};
		Group group{"group", "Group"};
		group.RenderActive = active;
		group.Ports = {{"result", "parent-value", PortDirection::Output, "control"}};
		document.Groups.push_back(std::move(group));
		document.Junctions = {{"parent-value", "group", ValueType::Any, std::nullopt}};
		document.Links = {
			{"source", "number", "control", "value"},
			{"control", "value", "parent-value", "value"},
			{"parent-value", "value", "consumer", "value"}
		};
		document.Outputs = {{"group-socket", "control", "value"}, {"result", "consumer", "number"}};
		return document;
	}
	Plan CompileGraph(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	void Process(
		const Document &document,
		GroupRenderSession &session,
		GroupRenderMode mode = GroupRenderMode::AutomaticFull,
		std::string_view groupId = {}
	) {
		const auto plan = CompileGraph(document);
		Diagnostic diagnostic;
		const auto status = ProcessGroupRender(document, plan, {}, {mode, groupId}, session, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	void Refresh(const Document &document, GroupRenderSession &session, EvaluationRequest request = {}) {
		Diagnostic diagnostic;
		const auto status = RefreshSourceGroupPurity(
			document,
			CompileGraph(document),
			request,
			{SourcePurityRefreshEvent::LoadTopology},
			session,
			diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
	SourceFrameActivity Activity(const GroupRenderSession &session, std::string_view id) {
		const auto row = std::find_if(session.Nodes.begin(), session.Nodes.end(), [&](const auto &node) {
			return node.NodeId == id;
		});
		REQUIRE(row != session.Nodes.end());
		return row->FrameActivity;
	}
	SourceGroupPurity Purity(const GroupRenderSession &session, std::string_view id = "group") {
		const auto row =
			std::find_if(session.Purities.begin(), session.Purities.end(), [&](const auto &group) {
				return group.GroupId == id;
			});
		REQUIRE(row != session.Purities.end());
		return row->State;
	}
	double Held(const Document &document, const GroupRenderSession &session, std::string_view id) {
		CacheGroupReplayOutput output;
		Diagnostic diagnostic;
		const auto status = ReadGroupRenderOutput(document, id, session, output, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		REQUIRE(output.Data.has_value());
		REQUIRE(std::holds_alternative<double>(*output.Data));
		return std::get<double>(*output.Data);
	}
}

TEST_CASE(
	"Group render flags persist without serializing process history", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	Diagnostic diagnostic;
	Document restored;
	const auto encoded = Write(document);
	REQUIRE_FALSE(encoded.empty());
	REQUIRE(Read(encoded, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK_FALSE(restored.Groups.front().RenderActive);
	CHECK(Write(GroupGraph()).find("group_render") == std::string::npos);

	const Document before = restored;
	for (const std::string_view suffix :
		 {"group_render \"group\" 0\n",
		  "group_render \"group\" 2\n",
		  "group_render \"missing\" 0\n",
		  "group_render \"group\" false\n"}) {
		CHECK(Read(encoded + std::string(suffix), restored, diagnostic) == Status::Malformed);
		CHECK(restored == before);
	}
	document.FormatVersion = 9;
	CHECK(Write(document).empty());
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) != Status::Ok);
	auto legacy = Write(GroupGraph());
	legacy.replace(0, std::string("imagegraph 10").size(), "imagegraph 9");
	CHECK(Read(legacy + "group_render \"group\" 0\n", restored, diagnostic) == Status::Malformed);
	CHECK(restored == before);
}

TEST_CASE(
	"Cold disabled group sockets do not schedule downstream consumers", "[imagegraph][groups][group_render]"
) {
	const auto document = GroupGraph(false);
	GroupRenderSession session;
	Process(document, session);
	CHECK(Held(document, session, "group-socket") == -1.0);
	CHECK(Held(document, session, "result") == 0.0);
	CHECK_FALSE(session.Ready("source"));
	CHECK_FALSE(session.Ready("control"));
	CHECK_FALSE(session.Ready("consumer"));
}

TEST_CASE(
	"Disabled warm values survive partial and full readiness resets", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	GroupRenderSession session;
	Process(document, session);
	CHECK(Held(document, session, "result") == 3.0);
	CHECK(session.Ready("control"));
	CHECK(session.Ready("consumer"));

	document.Groups.front().RenderActive = false;
	document.Nodes.front().Values.front().Data = 7.0;
	Process(document, session, GroupRenderMode::AutomaticPartial);
	CHECK(Held(document, session, "group-socket") == 3.0);
	CHECK(Held(document, session, "result") == 3.0);
	CHECK(session.Ready("control"));
	CHECK(session.Ready("consumer"));

	Process(document, session);
	CHECK(Held(document, session, "group-socket") == 3.0);
	CHECK(Held(document, session, "result") == 3.0);
	CHECK_FALSE(session.Ready("control"));
	CHECK_FALSE(session.Ready("consumer"));

	Document reopened;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), reopened, diagnostic) == Status::Ok);
	GroupRenderSession fresh;
	Process(reopened, fresh);
	CHECK(Held(reopened, fresh, "group-socket") == -1.0);
	CHECK(Held(reopened, fresh, "result") == 0.0);

	document.Groups.front().RenderActive = true;
	Process(document, session);
	CHECK(Held(document, session, "result") == 7.0);
	CHECK(session.Ready("control"));
	CHECK(session.Ready("consumer"));
}

TEST_CASE("Forced pure group updates bypass the automatic flag", "[imagegraph][groups][group_render]") {
	const auto document = GroupGraph(false);
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	CHECK(Held(document, session, "group-socket") == 3.0);
	CHECK(Held(document, session, "result") == 0.0);
	CHECK(session.Ready("source"));
	CHECK(session.Ready("control"));
	CHECK_FALSE(session.Ready("consumer"));
}

TEST_CASE(
	"Forced nonpure group updates leave children awaiting scheduling", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	document.Keyframes = {{"source", "value", 0, 3.0, "linear"}, {"source", "value", 1, 7.0, "linear"}};
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	CHECK(Held(document, session, "group-socket") == -1.0);
	CHECK(Held(document, session, "result") == 0.0);
	CHECK_FALSE(session.Ready("source"));
	CHECK_FALSE(session.Ready("control"));
	CHECK_FALSE(session.Ready("consumer"));
}

TEST_CASE("Group render flags remain local to instances", "[imagegraph][groups][group_render]") {
	Document document;
	document.FormatVersion = 10;
	document.Groups = {{"base", "Base"}, {"instance", "Instance", "", {}, 1, "base"}};
	document.Groups.front().RenderActive = false;
	document.Nodes = {
		{"original", "pc.number_simple", "base", {}, {{"value", 3.0}}},
		{"copy", "pc.number_simple", "instance", {}, {{"value", 99.0}}, {}, "original"}
	};
	document.Outputs = {{"base-result", "original", "number"}, {"instance-result", "copy", "number"}};
	GroupRenderSession session;
	Process(document, session);
	CHECK(Held(document, session, "base-result") == 0.0);
	CHECK(Held(document, session, "instance-result") == 3.0);
	CHECK_FALSE(session.Ready("original"));
	CHECK(session.Ready("copy"));
}

TEST_CASE(
	"Process refusal preserves held sockets and readiness atomically", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	GroupRenderSession session;
	Process(document, session);
	const auto outputs = session.Outputs;
	const auto readiness = session.Nodes;
	const auto retainedBytes = RetainedGroupRenderSessionBytes(session);
	Diagnostic diagnostic;
	const auto plan = CompileGraph(document);
	CHECK(
		ProcessGroupRender(document, plan, {}, {}, session, diagnostic, retainedBytes) ==
		Status::LimitExceeded
	);
	CHECK(session.Outputs == outputs);
	CHECK(session.Nodes == readiness);
	CHECK(Held(document, session, "result") == 3.0);
	CHECK(
		ProcessGroupRender(
			document, plan, {}, {GroupRenderMode::ForceGroup, "absent"}, session, diagnostic
		) != Status::Ok
	);
	CHECK(session.Outputs == outputs);
	CHECK(session.Nodes == readiness);
}

TEST_CASE(
	"Held socket reads reject replaced node types and removed dynamic ports",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	GroupRenderSession session;
	Process(document, session);
	CacheGroupReplayOutput destination{"sentinel", Value{-97.0}};
	const auto before = destination;
	Diagnostic diagnostic;
	document.Nodes.back().Type = "pc.number";
	CHECK(
		ReadGroupRenderOutput(document, "result", session, destination, diagnostic) == Status::InvalidOutput
	);
	CHECK(destination == before);

	Document dynamic;
	dynamic.FormatVersion = 10;
	dynamic.Nodes = {{"split", "pc.array_split", "", {}, {}}};
	dynamic.Nodes.front().DynamicOutputs = {{"held", ValueType::Any}};
	dynamic.Outputs = {{"dynamic-result", "split", "held"}};
	GroupRenderSession retained;
	const CacheGroupReplayOutput held{"held", Value{3.0}};
	const auto change = RetainCacheGroupReplayNode(
		retained.Outputs, "split", "pc.array_split", std::span(&held, 1), Limits::MaximumEvaluationBytes
	);
	REQUIRE(change.Code == Status::Ok);
	REQUIRE(
		ReadGroupRenderOutput(dynamic, "dynamic-result", retained, destination, diagnostic) == Status::Ok
	);
	CHECK(destination == held);
	dynamic.Nodes.front().DynamicOutputs.clear();
	destination = before;
	CHECK(
		ReadGroupRenderOutput(dynamic, "dynamic-result", retained, destination, diagnostic) ==
		Status::InvalidOutput
	);
	CHECK(destination == before);
}

TEST_CASE(
	"Group process byte accounting includes every replay ledger", "[imagegraph][groups][group_render]"
) {
	GroupRenderSession session;
	const auto checkGrowth = [&](uint64_t previous, uint64_t ledgerBefore, uint64_t ledgerAfter) {
		CHECK(ledgerAfter > ledgerBefore);
		CHECK(RetainedGroupRenderSessionBytes(session) == previous + ledgerAfter - ledgerBefore);
	};
	auto previous = RetainedGroupRenderSessionBytes(session);
	auto ledgerBefore = RetainedSimulationReplayBytes(session.Replay.Simulation);
	session.Replay.Simulation.Entries.push_back({});
	session.Replay.Simulation.Entries.back().NodeId = "simulation";
	checkGrowth(previous, ledgerBefore, RetainedSimulationReplayBytes(session.Replay.Simulation));

	previous = RetainedGroupRenderSessionBytes(session);
	ledgerBefore = RetainedSurfaceFrameReplayBytes(session.Replay.Surfaces);
	session.Replay.Surfaces.Entries.push_back({});
	session.Replay.Surfaces.Entries.back().NodeId = "surface";
	checkGrowth(previous, ledgerBefore, RetainedSurfaceFrameReplayBytes(session.Replay.Surfaces));

	previous = RetainedGroupRenderSessionBytes(session);
	ledgerBefore = RetainedRandomReplayBytes(session.Replay.Random);
	session.Replay.Random.Entries.push_back({});
	session.Replay.Random.Entries.back().NodeId = "random";
	session.Replay.Random.Entries.back().Kernel.assign(8, .5);
	checkGrowth(previous, ledgerBefore, RetainedRandomReplayBytes(session.Replay.Random));

	previous = RetainedGroupRenderSessionBytes(session);
	ledgerBefore = RetainedDataReplayBytes(session.Replay.Data);
	session.Replay.Data.Entries.push_back({});
	session.Replay.Data.Entries.back().NodeId = "data";
	session.Replay.Data.Entries.back().Values = {{0, std::string(128, 'x')}};
	checkGrowth(previous, ledgerBefore, RetainedDataReplayBytes(session.Replay.Data));

	previous = RetainedGroupRenderSessionBytes(session);
	ledgerBefore = RetainedRigidReplayBytes(session.Replay.Rigid);
	session.Replay.Rigid.Owners.push_back({});
	checkGrowth(previous, ledgerBefore, RetainedRigidReplayBytes(session.Replay.Rigid));
}

TEST_CASE(
	"Process admission charges borrowed authoring beside prior and candidate history",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	GroupRenderSession session;
	Process(document, session);
	document.Groups.front().Name.assign(128 * 1024, 'g');
	const auto documentBytes = DocumentRetainedPayloadBytes(document);
	REQUIRE(documentBytes.has_value());
	const auto cap = RetainedGroupRenderSessionBytes(session) * 2 + 64 * 1024;
	REQUIRE(*documentBytes > cap);
	const auto outputs = session.Outputs;
	const auto readiness = session.Nodes;
	Diagnostic diagnostic;
	const auto plan = CompileGraph(document);
	CHECK(ProcessGroupRender(document, plan, {}, {}, session, diagnostic, cap) == Status::LimitExceeded);
	CHECK(session.Outputs == outputs);
	CHECK(session.Nodes == readiness);
}

TEST_CASE(
	"Read-only evaluation observes disabled held sockets without scheduling",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	GroupRenderSession session;
	Process(document, session);
	document.Groups.front().RenderActive = false;
	document.Nodes.front().Values.front().Data = 7.0;
	Process(document, session);
	REQUIRE_FALSE(session.Ready("control"));
	const auto plan = CompileGraph(document);
	EvaluationRequest request;
	request.GroupRender = &session;
	Diagnostic diagnostic;
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "group-socket", request, result, diagnostic) == Status::Ok);
	CHECK(result.Data == Value{3.0});
	EvaluationSnapshot inputs;
	REQUIRE(EvaluateNodeInputs(document, plan, "consumer", request, inputs, diagnostic) == Status::Ok);
	const auto input = std::find_if(inputs.Values().begin(), inputs.Values().end(), [](const auto &value) {
		return value.Port == "value";
	});
	REQUIRE(input != inputs.Values().end());
	CHECK(input->Data == Value{3.0});
	CHECK_FALSE(session.Ready("control"));
	CHECK(Held(document, session, "group-socket") == 3.0);
}

TEST_CASE(
	"Partial processing retains ready outputs without explicit dirty seeds",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	document.Outputs.push_back({"source-socket", "source", "number"});
	GroupRenderSession session;
	Process(document, session);
	document.Nodes.front().Values.front().Data = 7.0;
	Process(document, session, GroupRenderMode::AutomaticPartial);
	CHECK(Held(document, session, "source-socket") == 3.0);
	CHECK(Held(document, session, "group-socket") == 3.0);
	CHECK(Held(document, session, "result") == 3.0);
	CHECK(session.Ready("source"));
	CHECK(session.Ready("control"));
	CHECK(session.Ready("consumer"));
}

TEST_CASE(
	"Explicit partial seeds update their ready downstream chain", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	document.Outputs.push_back({"source-socket", "source", "number"});
	GroupRenderSession session;
	Process(document, session);
	document.Nodes.front().Values.front().Data = 7.0;
	const auto plan = CompileGraph(document);
	const std::string_view affected = "source";
	Diagnostic diagnostic;
	const auto status = ProcessGroupRender(
		document,
		plan,
		{},
		{GroupRenderMode::AutomaticPartial, {}, std::span(&affected, 1)},
		session,
		diagnostic
	);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	CHECK(Held(document, session, "source-socket") == 7.0);
	CHECK(Held(document, session, "group-socket") == 7.0);
	CHECK(Held(document, session, "result") == 7.0);
	CHECK(session.Ready("source"));
	CHECK(session.Ready("control"));
	CHECK(session.Ready("consumer"));
}

TEST_CASE(
	"Type replacement resets only that producer during disabled partial processing",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	document.Outputs.push_back({"source-socket", "source", "number"});
	GroupRenderSession session;
	Process(document, session);
	REQUIRE(Held(document, session, "source-socket") == 3.0);
	document.Groups.front().RenderActive = false;
	document.Nodes.front().Type = "pc.number";
	document.Nodes.front().Values.front().Data = 7.0;
	Process(document, session, GroupRenderMode::AutomaticPartial);
	CHECK_FALSE(session.Ready("source"));
	CHECK(Held(document, session, "source-socket") == 0.0);
	CHECK(session.Ready("control"));
	CHECK(Held(document, session, "group-socket") == 3.0);
	CHECK(Held(document, session, "result") == 3.0);
}

TEST_CASE(
	"Changed dynamic socket types retire held payloads and readiness", "[imagegraph][groups][group_render]"
) {
	Document document;
	document.FormatVersion = 10;
	document.Groups = {{"group", "Group"}};
	document.Groups.front().RenderActive = false;
	document.Nodes = {{"split", "pc.array_split", "group", {}, {}}};
	document.Nodes.front().DynamicOutputs = {{"held", ValueType::Any}};
	document.Outputs = {{"result", "split", "held"}};
	GroupRenderSession session;
	const CacheGroupReplayOutput held{"held", Value{3.0}};
	const auto change = RetainCacheGroupReplayNode(
		session.Outputs, "split", "pc.array_split", std::span(&held, 1), Limits::MaximumEvaluationBytes
	);
	REQUIRE(change.Code == Status::Ok);
	session.Nodes = {{"split", true}};
	CacheGroupReplayOutput destination{"sentinel", Value{-97.0}};
	Diagnostic diagnostic;
	REQUIRE(ReadGroupRenderOutput(document, "result", session, destination, diagnostic) == Status::Ok);
	CHECK(destination == held);

	document.Nodes.front().DynamicOutputs.front().Type = ValueType::Image;
	destination = {"sentinel", Value{-97.0}};
	const auto before = destination;
	CHECK(
		ReadGroupRenderOutput(document, "result", session, destination, diagnostic) == Status::InvalidOutput
	);
	CHECK(destination == before);
	Process(document, session, GroupRenderMode::AutomaticPartial);
	CHECK_FALSE(session.Ready("split"));
	CHECK(
		ReadGroupRenderOutput(document, "result", session, destination, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(destination == before);
	CHECK(diagnostic.NodeId == "split");
	CHECK(diagnostic.Port == "held");
}

TEST_CASE(
	"Static source getters retain keys without making the group animated",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	document.Nodes.front().SourceStaticInputs = {"value"};
	document.Keyframes = {{"source", "value", 0, 13.0, "linear"}, {"source", "value", 1, 17.0, "linear"}};
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	// Static source getters read the retained first animator value.
	CHECK(Held(document, session, "group-socket") == 13.0);
	CHECK(Held(document, session, "result") == 0.0);
	CHECK(session.Ready("source"));
	CHECK(session.Ready("control"));
	CHECK_FALSE(session.Ready("consumer"));
}

TEST_CASE("Animated source mode makes an empty key map nonpure", "[imagegraph][groups][group_render]") {
	auto document = GroupGraph(false);
	document.Nodes.front().SourceAnimatedInputs = {"value"};
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	CHECK(Held(document, session, "group-socket") == -1.0);
	CHECK_FALSE(session.Ready("source"));
	CHECK_FALSE(session.Ready("control"));
}

TEST_CASE(
	"Forced group purity follows base animation before an instance getter override",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	auto owner = document.Nodes.front();
	owner.Id = "owner";
	owner.GroupId.clear();
	document.Nodes.front().InstanceBase = "owner";
	document.Nodes.front().InstanceOverrides = {"value"};
	bool animatedBase = false;
	SECTION("Static base remains pure with an animated replay getter on its instance") {
		owner.SourceStaticInputs = {"value"};
		document.Nodes.front().SourceAnimatedInputs = {"value"};
	}
	SECTION("Animated base remains nonpure with a static replay getter on its instance") {
		animatedBase = true;
		owner.SourceAnimatedInputs = {"value"};
		document.Nodes.front().SourceStaticInputs = {"value"};
	}
	document.Nodes.push_back(std::move(owner));
	const auto plan = CompileGraph(document);
	Diagnostic diagnostic;
	GroupReplayState empty, local, bound;
	REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
	const GroupSubtypeBinding binding{
		"source",
		"owner",
		animatedBase ? GroupSubtypeAnimator::Static : GroupSubtypeAnimator::Animated,
		animatedBase ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static,
		"value"
	};
	REQUIRE(BindGroupReplay(document, {&binding, 1}, local, 1, bound, diagnostic) == Status::Ok);
	CHECK(bound.Binding("source", "value")->Getter != bound.Binding("source", "value")->Writer);
	EvaluationRequest request;
	request.GroupReplay = &bound;
	request.GroupAuthoringRevision = 1;
	GroupRenderSession session;
	REQUIRE(ProcessGroupRender(document, plan, request, {}, session, diagnostic) == Status::Ok);
	Refresh(document, session, request);
	REQUIRE(
		ProcessGroupRender(
			document, plan, request, {GroupRenderMode::ForceGroup, "group"}, session, diagnostic
		) == Status::Ok
	);
	CHECK(session.Ready("source") == !animatedBase);
	CHECK(session.Ready("control") == !animatedBase);
	CHECK_FALSE(session.Ready("consumer"));
	CHECK(Held(document, session, "group-socket") == (animatedBase ? -1.0 : 3.0));
}

TEST_CASE(
	"Authored group purity persists independently of render activity", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph();
	document.Groups.front().PureFunction = false;
	const auto encoded = Write(document);
	REQUIRE_FALSE(encoded.empty());
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(encoded, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK_FALSE(restored.Groups.front().PureFunction);
	CHECK(restored.Groups.front().RenderActive);
	const auto prior = restored;
	for (const std::string_view row :
		 {"group_pure \"group\" 0\n",
		  "group_pure \"group\" 2\n",
		  "group_pure \"group\" false\n",
		  "group_pure \"missing\" 0\n",
		  "group_pure \"group\" 1 trailing\n"}) {
		CHECK(Read(encoded + std::string(row), restored, diagnostic) == Status::Malformed);
		CHECK(restored == prior);
	}
	CHECK(Write(GroupGraph()).find("group_pure") == std::string::npos);
	REQUIRE(Read(Write(GroupGraph()) + "group_pure \"group\" 1\n", restored, diagnostic) == Status::Ok);
	CHECK(restored.Groups.front().PureFunction);
	document.FormatVersion = 9;
	CHECK(Write(document).empty());
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) == Status::UnsupportedVersion);
}

TEST_CASE(
	"Authored nonpure groups wait for automatic scheduling after a forced update",
	"[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	document.Groups.front().PureFunction = false;
	GroupRenderSession session;
	Process(document, session);
	{
		Diagnostic diagnostic;
		REQUIRE(
			RefreshSourceGroupPurity(
				document,
				CompileGraph(document),
				{},
				{SourcePurityRefreshEvent::LoadTopology},
				session,
				diagnostic
			) == Status::Ok
		);
	}
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	CHECK_FALSE(session.Ready("source"));
	CHECK_FALSE(session.Ready("control"));
	CHECK(Held(document, session, "group-socket") == -1.0);
	document.Groups.front().RenderActive = true;
	Process(document, session);
	CHECK(session.Ready("source"));
	CHECK(session.Ready("control"));
	CHECK(Held(document, session, "result") == 3.0);
}

TEST_CASE(
	"Authored nonpure flag prevents forced inspection of unknown child frame activity",
	"[imagegraph][groups][group_render][atomic]"
) {
	auto document = GroupGraph(false);
	document.Groups.front().PureFunction = false;
	document.Nodes.push_back({"unknown-activity", "pc.ase_layer", "group", {}, {}});
	GroupRenderSession session;
	Process(document, session);
	{
		Diagnostic diagnostic;
		REQUIRE(
			RefreshSourceGroupPurity(
				document,
				CompileGraph(document),
				{},
				{SourcePurityRefreshEvent::LoadTopology},
				session,
				diagnostic
			) == Status::Ok
		);
	}
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	CHECK_FALSE(session.Ready("unknown-activity"));
	CHECK_FALSE(session.Ready("source"));
	const auto previous = session;
	const auto plan = CompileGraph(document);
	Diagnostic diagnostic;
	CHECK(
		ProcessGroupRender(
			document, plan, {}, {GroupRenderMode::ForceGroup, "group"}, session, diagnostic, 1
		) == Status::LimitExceeded
	);
	CHECK(session.Outputs == previous.Outputs);
	CHECK(session.Nodes == previous.Nodes);
	document.Groups.front().PureFunction = true;
	const auto enabledPlan = CompileGraph(document);
	REQUIRE(
		RefreshSourceGroupPurity(
			document, enabledPlan, {}, {SourcePurityRefreshEvent::PureFunction}, session, diagnostic
		) == Status::Ok
	);
	const auto unknownBefore = session;
	CHECK(
		ProcessGroupRender(
			document, enabledPlan, {}, {GroupRenderMode::ForceGroup, "group"}, session, diagnostic
		) == Status::UnsupportedExecution
	);
	CHECK(session.Outputs == unknownBefore.Outputs);
	CHECK(session.Nodes == unknownBefore.Nodes);
	CHECK(session.Purities == unknownBefore.Purities);
}

namespace {
	Document RandomGroup() {
		auto document = GroupGraph(false);
		document.Nodes.front().Type = "pc.random";
		document.Nodes.front().Values = {{"seed", int64_t{12345}}, {"shuffle", false}};
		document.Links.front().FromPort = "result";
		return document;
	}
	void RandomProcess(const Document &document, GroupRenderSession &session, GroupRenderMode mode) {
		RandomEntropyCapture capture;
		capture.NodeId = "source";
		capture.CurrentTimeMilliseconds = 42;
		capture.ReshuffleSeed = 123456;
		EvaluationRequest request;
		request.RandomEntropy = std::span(&capture, 1);
		Diagnostic diagnostic;
		const auto status = ProcessGroupRender(
			document,
			CompileGraph(document),
			request,
			{mode, mode == GroupRenderMode::ForceGroup ? "group" : ""},
			session,
			diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
}

TEST_CASE(
	"Curve constructor activity precedes its first update callback", "[imagegraph][groups][group_render]"
) {
	Document document;
	document.FormatVersion = 10;
	document.Groups = {{"group", "Group"}};
	document.Groups.front().RenderActive = false;
	document.Nodes = {{"curve", "pc.anim_curve", "group", {}, {{"animated", false}}}};
	document.Outputs = {{"result", "curve", "curve"}};
	GroupRenderSession session;
	Process(document, session);
	CHECK(Activity(session, "curve") == SourceFrameActivity::FrameDriven);
	CHECK_FALSE(session.Ready("curve"));
	Refresh(document, session);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
	document.Groups.front().RenderActive = true;
	Process(document, session);
	CHECK(Activity(session, "curve") == SourceFrameActivity::Static);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
	Refresh(document, session);
	CHECK(Purity(session) == SourceGroupPurity::Pure);
}

TEST_CASE(
	"Random callbacks change activity without refreshing cached group purity",
	"[imagegraph][groups][group_render]"
) {
	auto document = RandomGroup();
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	REQUIRE(Purity(session) == SourceGroupPurity::Pure);
	document.Groups.front().RenderActive = true;
	document.Nodes.front().Values.back().Data = true;
	RandomProcess(document, session, GroupRenderMode::AutomaticFull);
	CHECK(Activity(session, "source") == SourceFrameActivity::FrameDriven);
	CHECK(Purity(session) == SourceGroupPurity::Pure);
	Refresh(document, session);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);

	document.Nodes.front().Values.back().Data = false;
	RandomProcess(document, session, GroupRenderMode::AutomaticFull);
	CHECK(Activity(session, "source") == SourceFrameActivity::Static);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
	Process(document, session, GroupRenderMode::AutomaticPartial);
	CHECK(Activity(session, "source") == SourceFrameActivity::Static);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
	Refresh(document, session);
	CHECK(Purity(session) == SourceGroupPurity::Pure);
}

TEST_CASE(
	"Forced groups use cached purity after frame activity changes", "[imagegraph][groups][group_render]"
) {
	auto document = RandomGroup();
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	REQUIRE(Purity(session) == SourceGroupPurity::Pure);
	document.Nodes.front().Values.back().Data = true;
	RandomProcess(document, session, GroupRenderMode::ForceGroup);
	CHECK(session.Ready("source"));
	CHECK(Activity(session, "source") == SourceFrameActivity::FrameDriven);
	CHECK(Purity(session) == SourceGroupPurity::Pure);
	Process(document, session);
	CHECK_FALSE(session.Ready("source"));
	CHECK(Activity(session, "source") == SourceFrameActivity::FrameDriven);
	CHECK(Purity(session) == SourceGroupPurity::Pure);
	RandomProcess(document, session, GroupRenderMode::ForceGroup);
	CHECK(session.Ready("source"));
	Refresh(document, session);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
	Process(document, session);
	RandomProcess(document, session, GroupRenderMode::ForceGroup);
	CHECK_FALSE(session.Ready("source"));
}

TEST_CASE(
	"Replacing or moving group members invalidates cached purity", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	REQUIRE(Purity(session) == SourceGroupPurity::Pure);
	SECTION("Node type replacement") {
		document.Nodes.front().Type = "pc.number";
	}
	SECTION("Group membership change") {
		document.Groups.push_back({"other", "Other"});
		document.Nodes.front().GroupId = "other";
		document.Nodes[1].GroupId = "other";
		document.Groups.back().Ports = std::move(document.Groups.front().Ports);
		document.Groups.front().Ports.clear();
		document.Junctions.front().GroupId = "other";
	}
	Process(document, session, GroupRenderMode::AutomaticPartial);
	CHECK(Purity(session) == SourceGroupPurity::Unknown);
}

TEST_CASE(
	"Purity refresh refusal preserves activity history and all replay owners",
	"[imagegraph][groups][group_render]"
) {
	auto document = RandomGroup();
	document.Groups.front().RenderActive = true;
	GroupRenderSession session;
	RandomProcess(document, session, GroupRenderMode::AutomaticFull);
	Refresh(document, session);
	const auto nodes = session.Nodes;
	const auto purities = session.Purities;
	const auto outputs = session.Outputs;
	const auto simulation = session.Replay.Simulation;
	const auto surfaces = session.Replay.Surfaces;
	const auto random = session.Replay.Random;
	const auto data = session.Replay.Data;
	const auto rigid = session.Replay.Rigid;
	const auto cap = RetainedGroupRenderSessionBytes(session);
	Diagnostic diagnostic;
	CHECK(
		RefreshSourceGroupPurity(
			document,
			CompileGraph(document),
			{},
			{SourcePurityRefreshEvent::InputOutput},
			session,
			diagnostic,
			cap
		) == Status::LimitExceeded
	);
	CHECK(session.Nodes == nodes);
	CHECK(session.Purities == purities);
	CHECK(session.Outputs == outputs);
	CHECK(session.Replay.Simulation == simulation);
	CHECK(session.Replay.Surfaces == surfaces);
	CHECK(session.Replay.Random == random);
	CHECK(session.Replay.Data == data);
	CHECK(session.Replay.Rigid == rigid);
}

TEST_CASE(
	"Animated parent sockets do not animate their Group Input controls", "[imagegraph][groups][group_render]"
) {
	auto document = GroupGraph(false);
	document.Nodes.front() = {"source", "pc.group_input", "group", {}, {{"input_type", EnumValue{1}}}};
	document.Nodes.front().SourceAnimatedInputs = {"parent_value"};
	document.Groups.front().Ports.push_back({"input", "input-parent", PortDirection::Input, "source"});
	document.Junctions.push_back({"input-parent", "group", ValueType::Any, 3.0});
	document.Links.front().FromPort = "value";
	document.Links.push_back({"input-parent", "value", "source", "parent_value"});
	document.Keyframes = {
		{"source", "parent_value", 0, 3.0, "linear"}, {"source", "parent_value", 1, 7.0, "linear"}
	};
	GroupRenderSession session;
	Process(document, session);
	Refresh(document, session);
	CHECK(Purity(session) == SourceGroupPurity::Pure);
	Process(document, session, GroupRenderMode::ForceGroup, "group");
	CHECK(session.Ready("source"));
	CHECK(session.Ready("control"));
	CHECK(Held(document, session, "group-socket") == 3.0);
}

TEST_CASE(
	"Native audio producers advance during grouped partial processing", "[imagegraph][groups][group_render]"
) {
	Document document;
	document.FormatVersion = 10;
	document.Groups = {{"group", "Group"}};
	document.Timeline = TimelineSettings{8, 0, 7, "loop", 2.0};
	document.Nodes = {
		{"capture", "image.audio_recording", "group", {}, {{"source_id", std::string{"mono"}}}},
		{"window",
		 "image.audio_window",
		 "group",
		 {},
		 {{"width", int64_t{4}},
		  {"location", 0.0},
		  {"cursor_location", EnumValue{0}},
		  {"step", int64_t{2}},
		  {"match_timeline", true}}}
	};
	document.Links = {{"capture", "audio", "window", "audio"}};
	document.Outputs = {{"capture-result", "capture", "samples"}, {"window-result", "window", "samples"}};
	const AudioCaptureFrame captures[] = {
		{"mono", 0, {0, 1, 2, 3, 4, 5, 6, 7}, 4.0}, {"mono", 1, {10, 11, 12, 13, 14, 15, 16, 17}, 4.0}
	};
	EvaluationRequest request;
	request.AudioFrames = captures;
	GroupRenderSession session;
	Diagnostic diagnostic;
	const auto plan = CompileGraph(document);
	REQUIRE(ProcessGroupRender(document, plan, request, {}, session, diagnostic) == Status::Ok);
	CHECK(Activity(session, "capture") == SourceFrameActivity::FrameDriven);
	CHECK(Activity(session, "window") == SourceFrameActivity::FrameDriven);
	REQUIRE(
		RefreshSourceGroupPurity(
			document, plan, request, {SourcePurityRefreshEvent::LoadTopology}, session, diagnostic
		) == Status::Ok
	);
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
	const auto samples = [&](std::string_view id) {
		CacheGroupReplayOutput output;
		REQUIRE(ReadGroupRenderOutput(document, id, session, output, diagnostic) == Status::Ok);
		REQUIRE(output.Data.has_value());
		REQUIRE(std::holds_alternative<ArrayValue>(*output.Data));
		return std::get<ArrayValue>(*output.Data);
	};
	CHECK(samples("capture-result").Elements.front() == ElementValue{0.0});
	CHECK(samples("window-result").Elements == std::vector<ElementValue>{0.0, 2.0});
	request.Tick = 1;
	REQUIRE(
		ProcessGroupRender(
			document, plan, request, {GroupRenderMode::AutomaticPartial}, session, diagnostic
		) == Status::Ok
	);
	CHECK(samples("capture-result").Elements.front() == ElementValue{10.0});
	CHECK(samples("window-result").Elements == std::vector<ElementValue>{12.0, 14.0});
	CHECK(Purity(session) == SourceGroupPurity::Nonpure);
}
