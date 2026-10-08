#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_point_sdf_common")
using namespace engine::imagegraph;
namespace {
	Document Graph(bool empty = false) {
		Document d;
		d.FormatVersion = 11;
		ArrayValue points{ValueType::Vector2, {}};
		if (!empty) points.Elements.push_back(Vector2{1.5, 1.5});
		d.Nodes = {
			{"sdf",
			 "pc.point_sdf",
			 "",
			 {},
			 {{"dimension", Vector2{3, 3}},
			  {"dimension_unit", EnumValue{0}},
			  {"points", points},
			  {"max_distance", 2.},
			  {"attribute_color_depth", EnumValue{5}}}}
		};
		d.Outputs = {{"image", "sdf", "surface_out"}};
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = "sdf";
		owner.SourceType = "Node_Point_SDF";
		owner.NativeOwnerId = "sdf";
		owner.ShowUpdateTrigger = true;
		owner.UpdateAnimatorOwnerId = "sdf";
		owner.UpdateAnimatorPort = "native:animator:1";
		d.SourceCommonOwners = {owner};
		d.SourceAnimators.emplace();
		DetachedSourceAnimator animator;
		animator.OwnerId = "sdf";
		animator.Id = "native:animator:1";
		animator.OriginalPort = "pxcx.update_in_trigger";
		animator.Type = ValueType::Boolean;
		animator.Writer = GroupSubtypeAnimator::Animated;
		d.SourceAnimators->Detached.push_back(animator);
		GroupSubtypeOverlay payload;
		payload.NodeId = "sdf";
		payload.Port = animator.Id;
		Keyframe key;
		key.NodeId = "sdf";
		key.Port = animator.Id;
		key.Tick = 0;
		key.Data = false;
		payload.Keys = {key};
		d.SourceAnimators->DetachedValues.push_back(payload);
		return d;
	}
	Plan Checked(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto status = Compile(d, p, diag);
		INFO(diag.Message << " port=" << diag.Port);
		REQUIRE(status == Status::Ok);
		return p;
	}
	GroupRenderSession Cold(const Document &d) {
		GroupRenderSession s;
		Diagnostic diag;
		REQUIRE(
			InitializeNativeSourceCommonRuntime(d, Checked(d), {}, SourceNodeInitialState::Loaded, s, diag) ==
			Status::Ok
		);
		REQUIRE(s.Outputs.Nodes.size() == 1);
		REQUIRE(s.Outputs.Nodes[0].Outputs.size() == 1);
		const auto &constructor = s.Outputs.Nodes[0].Outputs[0];
		REQUIRE(constructor.Port == "surface_out");
		REQUIRE(constructor.Data);
		REQUIRE(std::holds_alternative<int64_t>(*constructor.Data));
		CHECK(std::get<int64_t>(*constructor.Data) == -4);
		REQUIRE(constructor.Domain);
		CHECK(constructor.Domain->Type == ValueType::Image);
		return s;
	}
}
TEST_CASE(
	"Point SDF common callback publishes and retains output without render readiness",
	"[imagegraph][source-point-sdf][source-common]"
) {
	auto d = Graph();
	auto session = Cold(d);
	Diagnostic diag;
	auto status = NativeSourceStepBounded(d, Checked(d), {}, {}, session, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	CHECK_FALSE(session.Ready("sdf"));
	const auto held = session.Outputs;
	d.Nodes[0].Values[2].Data = ArrayValue{ValueType::Vector2, {}};
	status = NativeSourceStepBounded(d, Checked(d), {}, {}, session, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	CHECK(session.Outputs == held);
	CHECK_FALSE(session.Ready("sdf"));
}
TEST_CASE(
	"Point SDF fresh empty common callback refuses atomically",
	"[imagegraph][source-point-sdf][source-common]"
) {
	auto d = Graph(true);
	auto session = Cold(d);
	const auto outputs = session.Outputs;
	const auto nodes = session.Nodes;
	Diagnostic diag;
	auto status = NativeSourceStepBounded(d, Checked(d), {}, {}, session, diag);
	CHECK(status == Status::UnsupportedExecution);
	CHECK(session.Outputs == outputs);
	CHECK(session.Nodes == nodes);
}
