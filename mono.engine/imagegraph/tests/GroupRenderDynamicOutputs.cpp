#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>

TEST_SUITE_ID("engine.imagegraph.group_render_dynamic_outputs")
using namespace engine::imagegraph;

TEST_CASE(
	"A newly declared socket receives cold state in a retained disabled producer", "[imagegraph][groups]"
) {
	Document document;
	document.FormatVersion = 10;
	document.Groups = {{"group", "Group"}};
	document.Groups.front().RenderActive = false;
	document.Nodes = {{"split", "pc.array_split", "group", {}, {}}};
	document.Nodes.front().DynamicOutputs = {{"held", ValueType::Any}};
	document.Outputs = {{"old", "split", "held"}};
	GroupRenderSession session;
	const CacheGroupReplayOutput held{"held", Value{3.0}};
	REQUIRE(
		RetainCacheGroupReplayNode(
			session.Outputs, "split", "pc.array_split", std::span(&held, 1), Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	session.Nodes = {{"split", true}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(
		ProcessGroupRender(document, plan, {}, {GroupRenderMode::AutomaticPartial}, session, diagnostic) ==
		Status::Ok
	);
	document.Nodes.front().DynamicOutputs.push_back({"added", ValueType::Any});
	document.Outputs.push_back({"new", "split", "added"});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(
		ProcessGroupRender(document, plan, {}, {GroupRenderMode::AutomaticPartial}, session, diagnostic) ==
		Status::Ok
	);
	CacheGroupReplayOutput output;
	// Node_Array_Split.preApplyDeserialize creates each declared output as Any with value zero.
	// Disabled processing must expose the new socket's constructor without invoking its update.
	const auto status = ReadGroupRenderOutput(document, "new", session, output, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	CHECK(status == Status::Ok);
	if (status == Status::Ok) {
		REQUIRE(output.Data.has_value());
		REQUIRE(std::holds_alternative<double>(*output.Data));
		CHECK(std::get<double>(*output.Data) == 0.0);
	}
	REQUIRE(ReadGroupRenderOutput(document, "old", session, output, diagnostic) == Status::Ok);
	CHECK(output.Data == std::optional<Value>{Value{3.0}});
}

TEST_CASE("Array Split cold sockets seed zero only for representable declarations", "[imagegraph][groups]") {
	for (const auto type : {ValueType::Any, ValueType::Scalar, ValueType::Image}) {
		Document document;
		document.FormatVersion = 10;
		document.Groups = {{"group", "Group"}};
		document.Groups.front().RenderActive = false;
		document.Nodes = {{"split", "pc.array_split", "group", {}, {}}};
		document.Nodes.front().DynamicOutputs = {{"cold", type}};
		document.Outputs = {{"out", "split", "cold"}};
		GroupRenderSession session;
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		REQUIRE(ProcessGroupRender(document, plan, {}, {}, session, diagnostic) == Status::Ok);
		CacheGroupReplayOutput output;
		const auto status = ReadGroupRenderOutput(document, "out", session, output, diagnostic);
		if (type == ValueType::Image) {
			CHECK(status == Status::UnsupportedExecution);
		} else {
			REQUIRE(status == Status::Ok);
			CHECK(output.Data == std::optional<Value>{Value{0.0}});
		}
	}
}

TEST_CASE(
	"Dynamic socket reconciliation retains cache ownership and old refusals atomically",
	"[imagegraph][groups]"
) {
	Document document;
	document.FormatVersion = 10;
	document.Nodes = {{"cache", "pc.cache", "", {}, {}}, {"split", "pc.array_split", "", {}, {}}};
	document.Nodes.front().SourceProperties = {
		{"cache_group", ArrayValue{ValueType::Text, {std::string{"split"}}}}
	};
	document.Nodes.back().DynamicOutputs = {{"held", ValueType::Any}};
	CacheGroupReplayState empty, loaded;
	Diagnostic diagnostic;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(
			document, empty, loaded, Limits::MaximumEvaluationBytes, diagnostic
		) == Status::Ok
	);
	const Diagnostic refused{Status::UnsupportedExecution, "split", "held", "retained source refusal"};
	CacheGroupReplayOutput held{"held", {}, {}, refused};
	REQUIRE(
		RetainCacheGroupReplayNode(
			loaded, "split", "pc.array_split", std::span(&held, 1), Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	for (auto &row : loaded.Nodes)
		if (row.NodeId == "split") row.RenderActive = false;
	const auto owners = loaded.Owners;
	document.Nodes.back().DynamicOutputs.push_back({"added", ValueType::Scalar});
	const auto before = loaded;
	CHECK(
		InitializeAuthoredCacheGroupReplay(document, loaded, loaded, 64, diagnostic) == Status::LimitExceeded
	);
	CHECK(loaded == before);
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(
			document, loaded, loaded, Limits::MaximumEvaluationBytes, diagnostic
		) == Status::Ok
	);
	CHECK(loaded.Owners == owners);
	const auto split = std::find_if(loaded.Nodes.begin(), loaded.Nodes.end(), [](const auto &row) {
		return row.NodeId == "split";
	});
	REQUIRE(split != loaded.Nodes.end());
	CHECK(split->OwnerId == "cache");
	CHECK_FALSE(split->RenderActive);
	REQUIRE(split->Outputs.size() == 2);
	CHECK(split->Outputs.front() == held);
	CHECK(split->Outputs.back().Port == "added");
	CHECK(split->Outputs.back().Data == std::optional<Value>{Value{0.0}});
	const auto reconciled = loaded;
	document.Nodes.back().Type = "pc.number_simple";
	CHECK(
		InitializeAuthoredCacheGroupReplay(
			document, loaded, loaded, Limits::MaximumEvaluationBytes, diagnostic
		) == Status::InvalidValue
	);
	CHECK(loaded == reconciled);
}
