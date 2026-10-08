#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_tunnel_transport")
using namespace engine::imagegraph;
namespace {
	Document Transport() {
		Document doc;
		doc.FormatVersion = 10;
		doc.Groups = {{"sender-group", "sender group"}, {"receiver-group", "receiver group"}};
		doc.Nodes = {
			{"sender",
			 "pc.tunnel_in",
			 "sender-group",
			 {},
			 {{"name", std::string{"pixels"}}, {"scope", EnumValue{0}}}},
			{"receiver", "pc.tunnel_out", "receiver-group", {}, {{"name", std::string{"pixels"}}}},
			{"image",
			 "image.solid",
			 "sender-group",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{9, 8, 7, 255}}}}
		};
		doc.Links = {{"image", "image", "sender", "value_in"}};
		doc.Outputs = {{"out", "receiver", "value_out"}};
		return doc;
	}
	Plan Checked(const Document &doc) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
}
TEST_CASE(
	"tunnel Any transports images and typed empty image arrays across global scope",
	"[imagegraph][source_tunnel]"
) {
	auto doc = Transport();
	auto plan = Checked(doc);
	Image image;
	Diagnostic diagnostic;
	REQUIRE(Evaluate(doc, plan, "out", {}, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK(image.Pixels == std::vector<uint8_t>{9, 8, 7, 255});
	doc.Nodes.push_back(
		{"array",
		 "value.array",
		 "sender-group",
		 {},
		 {},
		 {{"first", ValueType::Image, {}}, {"again", ValueType::Image, {}}}}
	);
	doc.Links = {
		{"image", "image", "array", "first"},
		{"image", "image", "array", "again"},
		{"array", "array", "sender", "value_in"}
	};
	plan = Checked(doc);
	ImageArray array;
	REQUIRE(EvaluateArray(doc, plan, "out", {}, array, diagnostic) == Status::Ok);
	REQUIRE(array.Images.size() == 2);
	REQUIRE(array.Items.size() == 2);
	CHECK(array.Images[0].Pixels == image.Pixels);
	CHECK(array.Images[1].Pixels == image.Pixels);

	DataReplayState replay;
	CacheGroupReplayOutput empty;
	empty.Port = "array";
	empty.Data = Value{ArrayValue{ValueType::Any, {}}};
	empty.ImageArrayPayload = true;
	REQUIRE(
		RetainCacheGroupReplayNode(
			replay.CacheGroups, "array", "value.array", std::span(&empty, 1), Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	replay.CacheGroups.Nodes.front().RenderActive = false;
	EvaluationRequest request;
	request.DataReplay = &replay;
	REQUIRE(EvaluateArray(doc, plan, "out", request, array, diagnostic) == Status::Ok);
	CHECK(array.Images.empty());
	CHECK(array.Items.empty());
}
TEST_CASE(
	"tunnel preserves image-array provenance when its receiver group becomes disabled",
	"[imagegraph][source_tunnel]"
) {
	auto doc = Transport();
	doc.Nodes.push_back({"array", "value.array", "sender-group", {}, {}, {{"first", ValueType::Image, {}}}});
	doc.Links = {{"image", "image", "array", "first"}, {"array", "array", "sender", "value_in"}};
	// grug warm native producers before receiver can read their unrepresented cold outputs.
	doc.Groups[1].RenderActive = false;
	auto plan = Checked(doc);
	GroupRenderSession session;
	Diagnostic diagnostic;
	EvaluationRequest request;
	const auto process = [&](std::string_view phase) {
		const auto status = ProcessGroupRender(doc, plan, request, {}, session, diagnostic);
		INFO(phase << ": " << diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
	};
	process("warm sender");
	CHECK_FALSE(session.Ready("receiver"));
	doc.Groups[1].RenderActive = true;
	plan = Checked(doc);
	request.Tick = 1;
	process("read retained native array");
	CacheGroupReplayOutput held;
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, held, diagnostic) == Status::Ok);
	CHECK(held.ImageArrayPayload);
	doc.Groups[1].RenderActive = false;
	plan = Checked(doc);
	request.Tick = 2;
	process("freeze receiver");
	REQUIRE(ReadGroupRenderOutput(doc, "out", session, held, diagnostic) == Status::Ok);
	CHECK(held.ImageArrayPayload);
	request.GroupRender = &session;
	ImageArray array;
	REQUIRE(EvaluateArray(doc, plan, "out", request, array, diagnostic) == Status::Ok);
	REQUIRE(array.Images.size() == 1);
	CHECK(array.Images[0].Pixels == std::vector<uint8_t>{9, 8, 7, 255});
}
