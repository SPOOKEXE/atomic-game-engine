#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_tunnel")
using namespace engine::imagegraph;
namespace {
	Document TunnelDocument() {
		Document doc;
		doc.FormatVersion = 10;
		doc.Groups = {{"a", "a"}, {"b", "b"}};
		doc.Nodes = {
			{"receive", "pc.tunnel_out", "a", {}, {{"name", std::string{"signal"}}}},
			{"global",
			 "pc.tunnel_in",
			 "b",
			 {},
			 {{"name", std::string{"signal"}}, {"scope", EnumValue{0}}, {"value_in", 11.}}},
			{"local", "pc.tunnel_in", "a", {}, {{"name", std::string{"signal"}}, {"value_in", 22.}}},
			{"later", "pc.tunnel_in", "a", {}, {{"name", std::string{"signal"}}, {"value_in", 33.}}}
		};
		doc.Outputs = {{"out", "receive", "value_out"}};
		return doc;
	}
	Value ReadTunnel(const Document &doc) {
		Plan plan;
		Diagnostic diagnostic;
		const Status compiled = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue value;
		const Status evaluated = EvaluateValue(doc, plan, "out", {}, value, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(evaluated == Status::Ok);
		return value.Data;
	}
} // namespace
TEST_CASE(
	"tunnel prefers exact group before global and last declared sender wins", "[imagegraph][source_tunnel]"
) {
	auto doc = TunnelDocument();
	CHECK(std::get<double>(ReadTunnel(doc)) == 33.);
	doc.Nodes.pop_back();
	CHECK(std::get<double>(ReadTunnel(doc)) == 22.);
	doc.Nodes.pop_back();
	CHECK(std::get<double>(ReadTunnel(doc)) == 11.);
	doc.Nodes[1].Values[0].Data = std::string{};
	CHECK(std::get<int64_t>(ReadTunnel(doc)) == -4);
	doc.Nodes[0].Values[0].Data = std::string{};
	CHECK(std::get<int64_t>(ReadTunnel(doc)) == -4);
}
TEST_CASE(
	"tunnel implicit source edges demand linked producer and reject "
	"cycles atomically",
	"[imagegraph][source_tunnel]"
) {
	auto doc = TunnelDocument();
	doc.Nodes.resize(2);
	doc.Nodes.push_back({"payload", "pc.string", "b", {}, {{"text", std::string{"full text"}}}});
	doc.Links = {{"payload", "text", "global", "value_in"}};
	CHECK(std::get<std::string>(ReadTunnel(doc)) == "full text");
	Plan accepted;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, accepted, diagnostic) == Status::Ok);
	const Plan before = accepted;
	doc.Nodes[1].GroupId = "a";
	doc.Links = {{"receive", "value_out", "global", "value_in"}};
	CHECK(Compile(doc, accepted, diagnostic) == Status::Cycle);
	CHECK(accepted == before);
}
TEST_CASE(
	"tunnel keeps complete nested Any data and sender ordering across "
	"native save",
	"[imagegraph][source_tunnel]"
) {
	auto doc = TunnelDocument();
	doc.Nodes.resize(2);
	ArrayValue nested{ValueType::Any, {}};
	nested.Items = {
		{ElementValue{std::string{"label"}}},
		{std::vector<SourceArrayItem>{{ElementValue{2.}}, {ElementValue{false}}}}
	};
	doc.Nodes[1].Values[2].Data = nested;
	CHECK(std::get<ArrayValue>(ReadTunnel(doc)) == nested);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(restored.Nodes.size() == doc.Nodes.size());
	CHECK(restored.Nodes[1].Id == "global");
	CHECK(std::get<ArrayValue>(ReadTunnel(restored)) == nested);
}
TEST_CASE(
	"unmatched warm tunnel retains value while output domain resets to Any", "[imagegraph][source_tunnel]"
) {
	auto doc = TunnelDocument();
	doc.Nodes.resize(2);
	CapturedFeedbackHost host;
	Plan plan;
	Diagnostic diagnostic;
	EvaluationRequest request;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(host.Prepare(doc, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(host.Value("out"));
	CHECK(std::get<double>(std::get<EvaluatedValue>(host.Value("out")->Output).Data) == 11.);
	doc.Nodes.erase(doc.Nodes.begin() + 1);
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(host.Prepare(doc, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(host.Value("out"));
	CHECK(std::get<double>(std::get<EvaluatedValue>(host.Value("out")->Output).Data) == 11.);
	REQUIRE(std::get<EvaluatedValue>(host.Value("out")->Output).Domain);
	CHECK(std::get<EvaluatedValue>(host.Value("out")->Output).Domain->Type == ValueType::Any);
	const Value before = std::get<EvaluatedValue>(host.Value("out")->Output).Data;
	CHECK_FALSE(host.Prepare(doc, plan, 2, 1, request, diagnostic, 1, "out"));
	REQUIRE(host.Value("out"));
	CHECK(std::get<EvaluatedValue>(host.Value("out")->Output).Data == before);
}
TEST_CASE(
	"global tunnel reads disabled sender input without processing its group", "[imagegraph][source_tunnel]"
) {
	auto doc = TunnelDocument();
	doc.Nodes.resize(2);
	doc.Groups[1].RenderActive = false;
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
	CHECK(std::get<double>(*output.Data) == 11.);
	CHECK_FALSE(session.Ready("global"));
	doc.Nodes[1].Values[2].Data = 22.;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(ProcessGroupRender(doc, plan, request, full, session, diagnostic) == Status::Ok);
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, output, diagnostic) == Status::Ok);
	REQUIRE(output.Data);
	CHECK(std::get<double>(*output.Data) == 22.);
	CHECK_FALSE(session.Ready("global"));
}

TEST_CASE(
	"tunnel authored Any accepts source keys and rejects runtime surfaces atomically",
	"[imagegraph][source_tunnel]"
) {
	auto doc = TunnelDocument();
	doc.Nodes.resize(2);
	doc.Keyframes = {{"global", "value_in", 0, std::string{"keyed"}, "source", KeyframeEase{}}};
	doc.Tracks = {{"global", "value_in", "hold", -1}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	CHECK(restored.Keyframes == doc.Keyframes);
	CHECK(restored.Tracks == doc.Tracks);
	const Plan before = plan;
	doc.Keyframes.clear();
	doc.Tracks.clear();
	Image image;
	image.Width = image.Height = 1;
	image.Pixels = {1, 2, 3, 255};
	doc.Nodes[1].Values[2].Data = SurfaceValue{image};
	CHECK(Compile(doc, plan, diagnostic) != Status::Ok);
	CHECK(plan == before);
	ArrayValue imageLeaf{ValueType::Any, {}};
	imageLeaf.Items = {{image}};
	doc.Nodes[1].Values[2].Data = imageLeaf;
	CHECK(Compile(doc, plan, diagnostic) != Status::Ok);
	CHECK(plan == before);
}
