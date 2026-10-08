#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.source_tunnel_frozen_domains")
using namespace engine::imagegraph;

TEST_CASE(
	"disabled tunnel reads held linked data while sender domain changes only in its callback",
	"[imagegraph][source_tunnel]"
) {
	Document doc;
	doc.FormatVersion = 10;
	doc.Groups = {{"receive_group", "receive"}, {"send_group", "send"}};
	doc.Groups[1].RenderActive = false;
	doc.Nodes = {
		{"receive", "pc.tunnel_out", "receive_group", {}, {{"name", std::string{"signal"}}}},
		{"send",
		 "pc.tunnel_in",
		 "send_group",
		 {},
		 {{"name", std::string{"signal"}}, {"scope", EnumValue{0}}, {"value_in", 42.}}},
		{"text", "pc.string", "send_group", {}, {{"text", std::string{"first"}}}},
		{"number", "pc.number", "send_group", {}, {{"value", 17.}}}
	};
	doc.Links = {{"text", "text", "send", "value_in"}};
	doc.Outputs = {{"out", "receive", "value_out"}};
	GroupRenderSession session;
	EvaluationRequest request;
	GroupRenderOperation full;
	Diagnostic diagnostic;
	const auto read = [&]() {
		Plan plan;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		REQUIRE(ProcessGroupRender(doc, plan, request, full, session, diagnostic) == Status::Ok);
		CacheGroupReplayOutput output;
		REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
		REQUIRE(output.Data);
		REQUIRE(output.Domain);
		++request.Tick;
		return output;
	};
	const auto cold = read();
	CHECK(std::get<std::string>(*cold.Data).empty());
	CHECK(cold.Domain->Type == ValueType::Any);
	CHECK_FALSE(session.Ready("send"));
	doc.Groups[1].RenderActive = true;
	const auto warm = read();
	CHECK(warm.Domain->Type == ValueType::Text);
	CHECK(std::get<std::string>(*warm.Data) == "first");
	REQUIRE(session.Ready("send"));
	const auto callbackData = session.Replay.Data;
	doc.Groups[1].RenderActive = false;
	doc.Links[0] = {"number", "number", "send", "value_in"};
	const auto rewired = read();
	CHECK(std::get<double>(*rewired.Data) == 17.);
	CHECK(rewired.Domain == warm.Domain);
	CHECK_FALSE(session.Ready("send"));
	for (const auto &prior : callbackData.Entries)
		if (prior.NodeId == "send") {
			const auto current = std::find_if(
				session.Replay.Data.Entries.begin(),
				session.Replay.Data.Entries.end(),
				[](const auto &entry) { return entry.NodeId == "send"; }
			);
			REQUIRE(current != session.Replay.Data.Entries.end());
			CHECK(*current == prior);
		}
	const auto heldOutputs = session.Outputs;
	const auto heldNodes = session.Nodes;
	const auto heldData = session.Replay.Data;
	const auto heldPurities = session.Purities;
	Plan refusedPlan;
	REQUIRE(Compile(doc, refusedPlan, diagnostic) == Status::Ok);
	CHECK(
		ProcessGroupRender(doc, refusedPlan, request, full, session, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(session.Outputs == heldOutputs);
	CHECK(session.Nodes == heldNodes);
	CHECK(session.Replay.Data == heldData);
	CHECK(session.Purities == heldPurities);

	doc.Groups[1].RenderActive = true;
	const auto updated = read();
	CHECK(updated.Domain->Type == ValueType::Scalar);
	doc.Groups[1].RenderActive = false;
	doc.Links.clear();
	const auto unlinked = read();
	CHECK(std::get<double>(*unlinked.Data) == 42.);
	CHECK(unlinked.Domain == updated.Domain);
	doc.Groups[1].RenderActive = true;
	const auto reset = read();
	CHECK(reset.Domain->Type == ValueType::Any);
}

TEST_CASE(
	"tunnel instance borrows payload getter without inheriting local sender link domain",
	"[imagegraph][source_tunnel]"
) {
	Document doc;
	doc.FormatVersion = 10;
	doc.Groups = {{"group", "group"}};
	doc.Nodes = {
		{"receive", "pc.tunnel_out", "group", {}, {{"name", std::string{"signal"}}}},
		{"base", "pc.tunnel_in", "group", {}, {{"name", std::string{"signal"}}, {"scope", EnumValue{0}}}},
		{"text", "pc.string", "group", {}, {{"text", std::string{"borrowed"}}}},
		{"instance", "pc.tunnel_in", "group", {}, {}}
	};
	doc.Nodes.back().InstanceBase = "base";
	doc.Links = {{"text", "text", "base", "value_in"}};
	doc.Outputs = {{"out", "receive", "value_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	GroupRenderSession session;
	REQUIRE(ProcessGroupRender(doc, plan, {}, {}, session, diagnostic) == Status::Ok);
	CacheGroupReplayOutput output;
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	REQUIRE(output.Domain);
	CHECK(std::get<std::string>(*output.Data).empty());
	CHECK(output.Domain->Type == ValueType::Any);
	CHECK(session.Ready("instance"));
	EvaluationRequest request;
	request.Tick = 1;
	REQUIRE(ProcessGroupRender(doc, plan, request, {}, session, diagnostic) == Status::Ok);
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	REQUIRE(output.Domain);
	CHECK(std::get<std::string>(*output.Data) == "borrowed");
	CHECK(output.Domain->Type == ValueType::Any);
	CHECK(session.Ready("instance"));
}
