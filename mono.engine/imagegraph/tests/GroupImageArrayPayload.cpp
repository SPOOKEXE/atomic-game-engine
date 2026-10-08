#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.group_image_array_payload")
using namespace engine::imagegraph;

namespace {
	Document ArrayGroup() {
		Document document;
		document.FormatVersion = 10;
		document.Nodes = {
			{"first",
			 "image.solid",
			 "group",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}},
			{"second",
			 "image.solid",
			 "group",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{4, 5, 6, 255}}}},
			{"array",
			 "value.array",
			 "group",
			 {},
			 {},
			 {{"first", ValueType::Image, {}}, {"second", ValueType::Image, {}}}},
			{"control", "pc.group_output", "group", {}, {}}
		};
		Group group{"group", "Group"};
		group.Ports = {{"result", "socket", PortDirection::Output, "control"}};
		document.Groups.push_back(group);
		document.Junctions = {{"socket", "group", ValueType::Any, {}}};
		document.Links = {
			{"first", "image", "array", "first"},
			{"second", "image", "array", "second"},
			{"array", "array", "control", "value"},
			{"control", "value", "socket", "value"}
		};
		document.Outputs = {{"out", "control", "value"}};
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	void Process(const Document &document, const Plan &plan, GroupRenderSession &session) {
		Diagnostic diagnostic;
		const auto status = ProcessGroupRender(document, plan, {}, {}, session, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	ImageArray Sample(const Document &document, const Plan &plan, const GroupRenderSession &session) {
		EvaluationRequest request;
		request.GroupRender = &session;
		ImageArray array;
		Diagnostic diagnostic;
		const auto status = EvaluateArray(document, plan, "out", request, array, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return array;
	}
}

TEST_CASE("Disabled Any Group sockets retain typed image arrays", "[imagegraph][group_render]") {
	auto document = ArrayGroup();
	auto plan = Checked(document);
	GroupRenderSession session;
	Process(document, plan, session);
	const auto before = Sample(document, plan, session);
	REQUIRE(before.Images.size() == 2);
	CHECK(before.Images[0].Pixels == std::vector<uint8_t>{1, 2, 3, 255});
	CHECK(before.Images[1].Pixels == std::vector<uint8_t>{4, 5, 6, 255});
	CacheGroupReplayOutput held;
	Diagnostic diagnostic;
	REQUIRE(ReadGroupRenderOutput(document, "out", session, held, diagnostic) == Status::Ok);
	CHECK(held.ImageArrayPayload);
	document.Groups.front().RenderActive = false;
	document.Nodes.front().Values.back().Data = Colour{9, 9, 9, 255};
	plan = Checked(document);
	Process(document, plan, session);
	const auto after = Sample(document, plan, session);
	CHECK(after.Items == before.Items);
	REQUIRE(after.Images.size() == before.Images.size());
	for (size_t index = 0; index < before.Images.size(); ++index) {
		CHECK(after.Images[index].Width == before.Images[index].Width);
		CHECK(after.Images[index].Height == before.Images[index].Height);
		CHECK(after.Images[index].Pixels == before.Images[index].Pixels);
		CHECK(after.Images[index].Format == before.Images[index].Format);
		CHECK(after.Images[index].Hash == before.Images[index].Hash);
	}
}

TEST_CASE("Empty typed image arrays thaw at disabled Group sockets", "[imagegraph][group_render]") {
	auto document = ArrayGroup();
	document.Groups.front().RenderActive = false;
	GroupRenderSession session;
	CacheGroupReplayOutput held{"value", Value{ArrayValue{ValueType::Any, {}}}};
	held.ImageArrayPayload = true;
	REQUIRE(
		RetainCacheGroupReplayNode(
			session.Outputs, "control", "pc.group_output", std::span(&held, 1), Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	const auto plan = Checked(document);
	Process(document, plan, session);
	const auto empty = Sample(document, plan, session);
	CHECK(empty.Images.empty());
	CHECK(empty.Items.empty());
	Diagnostic diagnostic;
	CacheGroupReplayOutput retained;
	REQUIRE(ReadGroupRenderOutput(document, "out", session, retained, diagnostic) == Status::Ok);
	CHECK(retained.ImageArrayPayload);
}

TEST_CASE("Raw heterogeneous surface arrays keep their value channel", "[imagegraph][group_render]") {
	auto document = ArrayGroup();
	document.Nodes.erase(document.Nodes.begin(), document.Nodes.begin() + 3);
	document.Links.erase(document.Links.begin(), document.Links.begin() + 3);
	ArrayValue raw{ValueType::Any, {}};
	raw.Items = {{Image{1, 1, {7, 8, 9, 255}}}, {ElementValue{2.5}}};
	document.Groups.front().RenderActive = false;
	std::string_view outputPort = "value";
	SECTION("Generic source Group socket") {}
	SECTION("Native array producer preserves an unmarked raw journal") {
		document.Nodes.front().Type = "value.array";
		document.Nodes.front().DynamicInputs = {{"input", ValueType::Image, std::nullopt}};
		document.Nodes.push_back(
			{"hidden",
			 "image.solid",
			 "group",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 255, 255}}}}
		);
		document.Groups.front().Ports.clear();
		document.Junctions.clear();
		document.Links = {{"hidden", "image", "control", "input"}};
		document.Outputs.front().Port = "array";
		outputPort = "array";
	}
	// grug seeds the runtime journal. Surface pixels are not authored document literals.
	CacheGroupReplayOutput prior{std::string(outputPort), Value{raw}};
	GroupRenderSession session;
	const auto retained = RetainCacheGroupReplayNode(
		session.Outputs,
		"control",
		document.Nodes.front().Type,
		std::span(&prior, 1),
		Limits::MaximumEvaluationBytes
	);
	INFO(retained.Error.Message);
	REQUIRE(retained.Code == Status::Ok);
	const auto plan = Checked(document);
	Process(document, plan, session);
	EvaluationRequest request;
	request.GroupRender = &session;
	Diagnostic diagnostic;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(value.Data == Value{raw});
	CacheGroupReplayOutput held;
	REQUIRE(ReadGroupRenderOutput(document, "out", session, held, diagnostic) == Status::Ok);
	CHECK_FALSE(held.ImageArrayPayload);
}
