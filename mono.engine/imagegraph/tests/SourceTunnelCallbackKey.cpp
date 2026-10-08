#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_tunnel_callback_key")
using namespace engine::imagegraph;
namespace {
	Document ReceiverFirst() {
		Document document;
		document.FormatVersion = 10;
		document.Groups = {{"g", "g"}};
		document.Nodes = {
			{"receiver", "pc.tunnel_out", "g", {}, {{"name", std::string{"signal"}}}},
			{"value", "pc.number_simple", "g", {}, {{"value", 11.}}},
			{"sender", "pc.tunnel_in", "g", {}, {{"name", std::string{"signal"}}}}
		};
		document.Links = {{"value", "number", "sender", "value_in"}};
		document.Outputs = {{"out", "receiver", "value_out"}};
		return document;
	}
}
TEST_CASE(
	"cold tunnel receiver key does not force its newly selected sender callback",
	"[imagegraph][source_tunnel]"
) {
	const auto document = ReceiverFirst();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	GroupRenderSession session;
	EvaluationRequest request;
	GroupRenderOperation full;
	REQUIRE(ProcessGroupRender(document, plan, request, full, session, diagnostic) == Status::Ok);
	CacheGroupReplayOutput output;
	REQUIRE(ReadGroupRenderOutput(document, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<double>(*output.Data) == 0.);
	REQUIRE(output.Domain);
	CHECK(output.Domain->Type == ValueType::Any);
	request.Tick = 1;
	REQUIRE(ProcessGroupRender(document, plan, request, full, session, diagnostic) == Status::Ok);
	REQUIRE(ReadGroupRenderOutput(document, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<double>(*output.Data) == 11.);
	REQUIRE(output.Domain);
	CHECK(output.Domain->Type == ValueType::Scalar);
}
TEST_CASE("future receiver callback key cannot steer source topology", "[imagegraph][source_tunnel]") {
	const auto document = ReceiverFirst();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	GroupRenderSession session;
	EvaluationRequest request;
	GroupRenderOperation full;
	REQUIRE(ProcessGroupRender(document, plan, request, full, session, diagnostic) == Status::Ok);
	bool changed = false;
	for (auto &entry : session.Replay.Data.Entries) {
		if (entry.NodeId != "receiver" || entry.ProcessorRow != 0) continue;
		REQUIRE(entry.Values.size() == 1);
		entry.Tick = 99;
		entry.Values.front().Frame = 99;
		changed = true;
	}
	REQUIRE(changed);
	const auto prior = session;
	request.Tick = 1;
	CHECK(ProcessGroupRender(document, plan, request, full, session, diagnostic) == Status::InvalidValue);
	CHECK(session.Outputs == prior.Outputs);
	CHECK(session.Nodes == prior.Nodes);
	CHECK(session.Purities == prior.Purities);
	CHECK(session.Replay.Data == prior.Replay.Data);
	CHECK(session.Replay.Simulation == prior.Replay.Simulation);
	CHECK(session.Replay.Surfaces == prior.Replay.Surfaces);
	CHECK(session.Replay.Random == prior.Replay.Random);
	CHECK(session.Replay.Rigid == prior.Replay.Rigid);
}

TEST_CASE(
	"source tunnel getters retain exact unconnected constructor defaults", "[imagegraph][source_tunnel]"
) {
	for (const bool named : {false, true}) {
		Document document;
		document.FormatVersion = 10;
		document.Groups = {{"g", "g"}};
		document.Nodes = {
			{"receiver", "pc.tunnel_out", "g", {}, {}}, {"sender", "pc.tunnel_in", "g", {}, {}}
		};
		if (named) {
			document.Nodes[0].Values = {{"name", std::string{"signal"}}};
			document.Nodes[1].Values = {{"name", std::string{"signal"}}};
		}
		document.Outputs = {{"out", "receiver", "value_out"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		GroupRenderSession session;
		EvaluationRequest request;
		GroupRenderOperation full;
		for (uint64_t tick = 0; tick < 2; ++tick) {
			request.Tick = tick;
			REQUIRE(ProcessGroupRender(document, plan, request, full, session, diagnostic) == Status::Ok);
			CacheGroupReplayOutput output;
			REQUIRE(ReadGroupRenderOutput(document, "out", session, output, diagnostic) == Status::Ok);
			REQUIRE(output.Data);
			CHECK(std::get<int64_t>(*output.Data) == -4);
			REQUIRE(output.Domain);
			CHECK(output.Domain->Type == ValueType::Any);
		}
	}
}
