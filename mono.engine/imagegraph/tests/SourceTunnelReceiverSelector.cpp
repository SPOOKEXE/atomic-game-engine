#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_tunnel_receiver_selector")
using namespace engine::imagegraph;
TEST_CASE(
	"tunnel receiver uses current linked name against pre-render sender registry",
	"[imagegraph][source_tunnel]"
) {
	Document doc;
	doc.FormatVersion = 10;
	doc.Groups = {{"g", "g"}};
	doc.Nodes = {
		{"name", "pc.string", "g", {}, {{"text", std::string{"old"}}}},
		{"old", "pc.tunnel_in", "g", {}, {{"name", std::string{"old"}}, {"value_in", 11.}}},
		{"new", "pc.tunnel_in", "g", {}, {{"name", std::string{"new"}}, {"value_in", 22.}}},
		{"receive", "pc.tunnel_out", "g"}
	};
	doc.Links = {{"name", "text", "receive", "name"}};
	doc.Outputs = {{"out", "receive", "value_out"}};
	GroupRenderSession session;
	EvaluationRequest request;
	Diagnostic diagnostic;
	const auto pulse = [&](double expected) {
		Plan plan;
		REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
		REQUIRE(ProcessGroupRender(doc, plan, request, {}, session, diagnostic) == Status::Ok);
		CacheGroupReplayOutput output;
		REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
		REQUIRE(output.Data);
		CHECK(std::holds_alternative<double>(*output.Data));
		if (const auto *number = std::get_if<double>(&*output.Data)) CHECK(*number == expected);
		++request.Tick;
	};
	pulse(11.);
	doc.Nodes[0].Values[0].Data = std::string{"new"};
	pulse(22.);
}
