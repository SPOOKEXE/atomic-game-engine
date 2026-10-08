#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_tunnel_registry")
using namespace engine::imagegraph;
TEST_CASE(
	"linked tunnel names observe pre-render held outputs before current "
	"frame producers update",
	"[imagegraph][source_tunnel]"
) {
	Document doc;
	doc.FormatVersion = 10;
	doc.Groups = {{"g", "g"}};
	doc.Nodes = {
		{"name", "pc.string", "g", {}, {{"text", std::string{"signal"}}}},
		{"value", "pc.number_simple", "g", {}, {{"value", 11.}}},
		{"sender", "pc.tunnel_in", "g", {}, {}},
		{"receiver", "pc.tunnel_out", "g", {}, {{"name", std::string{"signal"}}}}
	};
	doc.Links = {{"name", "text", "sender", "name"}, {"value", "number", "sender", "value_in"}};
	doc.Outputs = {{"out", "receiver", "value_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	GroupRenderSession session;
	EvaluationRequest request;
	GroupRenderOperation full;
	REQUIRE(ProcessGroupRender(doc, plan, request, full, session, diagnostic) == Status::Ok);
	CacheGroupReplayOutput output;
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<int64_t>(*output.Data) == -4);
	request.Tick = 1;
	REQUIRE(ProcessGroupRender(doc, plan, request, full, session, diagnostic) == Status::Ok);
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<double>(*output.Data) == 11.);
	REQUIRE(output.Domain);
	CHECK(output.Domain->Type == ValueType::Scalar);
	doc.Nodes[0].Values[0].Data = std::string{"other"};
	doc.Nodes[1].Values[0].Data = 22.;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	request.Tick = 2;
	REQUIRE(ProcessGroupRender(doc, plan, request, full, session, diagnostic) == Status::Ok);
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<double>(*output.Data) == 22.);
	REQUIRE(output.Domain);
	CHECK(output.Domain->Type == ValueType::Scalar);
	request.Tick = 3;
	REQUIRE(ProcessGroupRender(doc, plan, request, full, session, diagnostic) == Status::Ok);
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<double>(*output.Data) == 22.);
	REQUIRE(output.Domain);
	CHECK(output.Domain->Type == ValueType::Any);
}
