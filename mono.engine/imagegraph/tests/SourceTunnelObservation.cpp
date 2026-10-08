#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.source_tunnel_observation")
using namespace engine::imagegraph;
TEST_CASE(
	"tunnel registry observation rejects duplicate identities and selected cycles atomically",
	"[imagegraph][source_tunnel]"
) {
	Document doc;
	doc.FormatVersion = 10;
	doc.Nodes = {
		{"name", "pc.string", {}, {}, {{"text", std::string{"signal"}}}},
		{"sender", "pc.tunnel_in", {}, {}, {{"scope", EnumValue{0}}, {"value_in", 12.}}},
		{"receiver", "pc.tunnel_out", {}, {}, {{"name", std::string{"signal"}}}}
	};
	doc.Links = {{"name", "text", "sender", "name"}};
	doc.Outputs = {{"out", "receiver", "value_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	const std::array observations{SourceTunnelRegistryObservation{"sender", "signal", 0.}};
	EvaluationRequest request;
	request.SourceTunnelRegistryObservations = observations;
	EvaluatedValue output;
	REQUIRE(EvaluateValue(doc, plan, "out", request, output, diagnostic) == Status::Ok);
	REQUIRE(std::get<double>(output.Data) == 12.);
	const EvaluatedValue before = output;
	const std::array duplicate{observations[0], observations[0]};
	request.SourceTunnelRegistryObservations = duplicate;
	CHECK(EvaluateValue(doc, plan, "out", request, output, diagnostic) == Status::DuplicateId);
	CHECK(output == before);
	request.SourceTunnelRegistryObservations = observations;
	doc.Links.push_back({"receiver", "value_out", "sender", "value_in"});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateValue(doc, plan, "out", request, output, diagnostic) == Status::Cycle);
	CHECK(output == before);
}
