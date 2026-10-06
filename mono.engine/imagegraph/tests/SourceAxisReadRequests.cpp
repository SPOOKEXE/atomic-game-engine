#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_axis_read_requests")
using namespace engine::imagegraph;

namespace {
	constexpr std::string_view Point = "point_i_0";

	void SetAxes(Node &node, bool initialized) {
		SourceSeparatedVec2Animator axes;
		axes.Port = Point;
		axes.Separated = true;
		axes.Initialized = initialized;
		if (initialized) {
			axes.Axes[0].Keys = {{node.Id, std::string(Point), 0, 12.0, "source", KeyframeEase{}}};
			axes.Axes[1].Keys = {{node.Id, std::string(Point), 0, 3.0, "source", KeyframeEase{}}};
			for (size_t axis = 0; axis < 2; ++axis)
				axes.Axes[axis].Keys.front().SourceKeyId = node.Id + "-axis-" + std::to_string(axis);
		}
		node.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
	}

	Document GradientAlias(bool overridden, bool initialized) {
		Node base{
			"base",
			"pc.gradient_points_n",
			"",
			{},
			{{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}, {"blend_mode", EnumValue{0}}}
		};
		base.DynamicInputs = {
			{std::string(Point), ValueType::Vector2, Vector2{99, 88}},
			{"point_i_1", ValueType::Vector2, Vector2{30, 3}}
		};
		base.SourceAnimatedInputs = {std::string(Point)};
		SetAxes(base, initialized);

		Node copy = base;
		copy.Id = "copy";
		copy.InstanceBase = "base";
		copy.InstanceOverrides =
			overridden ? std::vector<std::string>{std::string(Point)} : std::vector<std::string>{};
		if (overridden)
			SetAxes(copy, initialized);
		else
			copy.SourceSeparatedVec2Animators = {};

		Document document;
		document.FormatVersion = 9;
		document.Nodes = {std::move(base), std::move(copy)};
		document.Outputs = {{"image", "copy", "surface_out"}};
		return document;
	}

	Plan CompileDocument(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
}

TEST_CASE(
	"Node input reads report the actual cold getter owner and preserve the previous snapshot",
	"[source_axis_read_requests]"
) {
	for (const bool overridden : {false, true}) {
		INFO("overridden getter: " << overridden);
		const auto warm = GradientAlias(overridden, true);
		const auto warmPlan = CompileDocument(warm);
		EvaluationRequest request;
		EvaluationSnapshot snapshot;
		Diagnostic diagnostic;
		REQUIRE(EvaluateNodeInputs(warm, warmPlan, "copy", request, snapshot, diagnostic) == Status::Ok);
		const auto oldBytes = snapshot.RetainedBytes();
		const auto oldValues =
			std::vector<EvaluationInputValue>(snapshot.Values().begin(), snapshot.Values().end());
		const auto oldGetter = std::find_if(oldValues.begin(), oldValues.end(), [](const auto &value) {
			return value.Port == Point;
		});
		REQUIRE(oldGetter != oldValues.end());

		const auto cold = GradientAlias(overridden, false);
		const auto coldPlan = CompileDocument(cold);
		const auto status = EvaluateNodeInputs(cold, coldPlan, "copy", request, snapshot, diagnostic);
		CHECK(status == Status::SourceAxisInitializationRequired);
		CHECK(diagnostic.Code == Status::SourceAxisInitializationRequired);
		CHECK(diagnostic.NodeId == (overridden ? "copy" : "base"));
		CHECK(diagnostic.Port == Point);
		CHECK(snapshot.RetainedBytes() == oldBytes);
		REQUIRE(snapshot.Values().size() == oldValues.size());
		const auto retainedGetter =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == Point;
			});
		REQUIRE(retainedGetter != snapshot.Values().end());
		CHECK(retainedGetter->Data == oldGetter->Data);
	}
}
