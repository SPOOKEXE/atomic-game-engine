#include "../src/SourceAxisStorage.hpp"
#include "../src/SourceSeparatedVec2.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.source_axis_initialization")
using namespace engine::imagegraph;
namespace {
	constexpr std::string_view Point = "point_i_0";

	Node GradientNode(std::string id) {
		Node node{
			std::move(id),
			"pc.gradient_points_n",
			"",
			{},
			{{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}, {"blend_mode", EnumValue{0}}}
		};
		node.DynamicInputs = {
			{std::string(Point), ValueType::Vector2, Value{Vector2{99, 88}}},
			{"point_i_1", ValueType::Vector2, Value{Vector2{30, 3}}}
		};
		node.SourceAnimatedInputs = {std::string(Point)};
		return node;
	}

	Document AliasedGraph(bool baseDefault = false) {
		Document document;
		document.FormatVersion = 9;
		auto base = GradientNode("base");
		if (baseDefault)
			base.SourceVec2Defaults.emplace().Inputs.push_back({std::string(Point), Vector2{5, 6}});
		auto copy = GradientNode("copy");
		copy.InstanceBase = "base";
		copy.InstanceOverrides = {std::string(Point)};
		copy.SourceVec2Defaults.emplace().Inputs.push_back({std::string(Point), Vector2{7, 8}});
		auto cold = GradientNode("cold");
		cold.InstanceBase = "copy";
		cold.InstanceOverrides = {std::string(Point)};
		auto late = GradientNode("late");
		late.InstanceBase = "copy";
		late.InstanceOverrides = {std::string(Point)};
		document.Nodes = {std::move(base), std::move(copy), std::move(cold), std::move(late)};
		return document;
	}

	GroupSubtypeBinding Binding(std::string_view nodeId) {
		return {
			std::string(nodeId),
			"base",
			GroupSubtypeAnimator::Animated,
			GroupSubtypeAnimator::Animated,
			std::string(Point)
		};
	}

	void AddAxes(Node &node, double x, double y, bool separated = true) {
		SourceSeparatedVec2Animator axes;
		axes.Port = std::string(Point);
		axes.Separated = separated;
		axes.Axes[0].Keys = {{node.Id, std::string(Point), 0, x, "source", KeyframeEase{}}};
		axes.Axes[1].Keys = {{node.Id, std::string(Point), 0, y, "source", KeyframeEase{}}};
		for (size_t axis = 0; axis < 2; ++axis)
			axes.Axes[axis].Keys.front().SourceKeyId = node.Id + "-axis-" + std::to_string(axis);
		node.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
	}
}

TEST_CASE(
	"Cold aliases initialize local axes from constructor defaults without warming old descendants",
	"[source_axis_initialization]"
) {
	auto document = AliasedGraph();
	const std::array initialBindings{Binding("copy"), Binding("cold")};
	Diagnostic diagnostic;
	GroupReplayState empty, initial, coldReplay;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(document, initialBindings, initial, 1, coldReplay, diagnostic) == Status::Ok);
	const auto *coldCopyBinding = coldReplay.Binding("copy", Point);
	const auto *coldDescendant = coldReplay.Binding("cold", Point);
	REQUIRE(coldCopyBinding);
	REQUIRE(coldDescendant);
	CHECK(coldCopyBinding->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(coldDescendant->Axes.Storage == GroupAxisStorage::Uninitialized);

	AddAxes(document.Nodes[0], 301, 401);
	SourceSeparatedVec2Animator localMode;
	localMode.Port = std::string(Point);
	localMode.Separated = true;
	localMode.Initialized = false;
	document.Nodes[1].SourceSeparatedVec2Animators.emplace().Inputs.push_back(localMode);
	const std::array targets{SourceAxisInitialization{"copy", Point}};
	GroupReplayState initialized;
	REQUIRE(
		InitializeSourceVec2Axes(document, targets, coldReplay, 1, initialized, diagnostic) == Status::Ok
	);
	const auto *copyBinding = initialized.Binding("copy", Point);
	REQUIRE(copyBinding);
	CHECK(copyBinding->OwnerId == "base");
	CHECK(copyBinding->Writer == GroupSubtypeAnimator::Animated);
	CHECK(copyBinding->Axes.Storage == GroupAxisStorage::Local);
	CHECK(copyBinding->Axes.OwnerId == "copy");
	CHECK(copyBinding->Axes.Port == Point);
	CHECK(copyBinding->Axes.Writer == GroupSubtypeAnimator::Animated);
	const auto *coldAfterInitialization = initialized.Binding("cold", Point);
	REQUIRE(coldAfterInitialization);
	CHECK(coldAfterInitialization->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(coldAfterInitialization->Axes.OwnerId == "cold");

	const auto *axesOverlay = initialized.SharedSubtype("copy", Point);
	REQUIRE(axesOverlay);
	REQUIRE(axesOverlay->SeparatedVec2);
	CHECK(axesOverlay->SeparatedVec2->Separated);
	for (size_t axis = 0; axis < 2; ++axis) {
		const auto &keys = axesOverlay->SeparatedVec2->Axes[axis].Keys;
		REQUIRE(keys.size() == 1);
		CHECK(keys.front().NodeId == "copy");
		CHECK(keys.front().Port == Point);
		CHECK(keys.front().Tick == 0);
		CHECK(keys.front().Data == Value{axis == 0 ? 7.0 : 8.0});
		CHECK(keys.front().SourceKeyId.empty());
		CHECK_FALSE(keys.front().SourceDriver);
		CHECK_FALSE(keys.front().SineDriver);
	}
	CHECK(document.Nodes[1].DynamicInputs.front().Default == Value{Vector2{99, 88}});
	CHECK(document.Nodes[1].SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});
	CHECK(
		document.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front().Data ==
		Value{301.0}
	);
	CHECK(
		document.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.front().Data ==
		Value{401.0}
	);

	const std::array allBindings{Binding("copy"), Binding("cold"), Binding("late")};
	GroupReplayState withLateDescendant;
	REQUIRE(
		BindGroupReplay(document, allBindings, initialized, 1, withLateDescendant, diagnostic) == Status::Ok
	);
	const auto *lateBinding = withLateDescendant.Binding("late", Point);
	REQUIRE(lateBinding);
	CHECK(lateBinding->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(lateBinding->Axes.OwnerId == "copy");
	CHECK(lateBinding->Axes.Port == Point);

	const std::array coldTarget{SourceAxisInitialization{"cold", Point}};
	GroupReplayState initializedCold;
	REQUIRE(
		InitializeSourceVec2Axes(document, coldTarget, initialized, 1, initializedCold, diagnostic) ==
		Status::Ok
	);
	const auto *fallbackAxes = initializedCold.SharedSubtype("cold", Point);
	REQUIRE(fallbackAxes);
	REQUIRE(fallbackAxes->SeparatedVec2);
	CHECK(fallbackAxes->SeparatedVec2->Axes[0].Keys.front().Data == Value{0.0});
	CHECK(fallbackAxes->SeparatedVec2->Axes[1].Keys.front().Data == Value{0.0});

	const Value editedValue = Vector2{17, 18};
	GroupRefreshEvent edit;
	edit.NodeId = "copy";
	edit.Reason = GroupRefreshReason::Edit;
	edit.EditedPort = Point;
	edit.LocalValue = &editedValue;
	edit.LocalAnimated = true;
	edit.At.Tick = 5;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, initialized, 1, edited, diagnostic) == Status::Ok);
	GroupReplayState repeated;
	REQUIRE(InitializeSourceVec2Axes(document, targets, edited, 1, repeated, diagnostic) == Status::Ok);
	const auto *editedAxes = repeated.SharedSubtype("copy", Point);
	REQUIRE(editedAxes);
	REQUIRE(editedAxes->SeparatedVec2);
	for (size_t axis = 0; axis < 2; ++axis)
		CHECK(
			std::any_of(
				editedAxes->SeparatedVec2->Axes[axis].Keys.begin(),
				editedAxes->SeparatedVec2->Axes[axis].Keys.end(),
				[&](const auto &key) { return key.Tick == 5 && key.Data == Value{axis == 0 ? 17.0 : 18.0}; }
			)
		);

	Document projected;
	REQUIRE(ProjectGroupReplay(document, repeated, 1, projected, diagnostic) == Status::Ok);
	GroupReplayState rebound;
	REQUIRE(RebindProjectedGroupReplay(projected, repeated, 2, rebound, diagnostic) == Status::Ok);
	const auto *projectedCopy = rebound.Binding("copy", Point);
	REQUIRE(projectedCopy);
	CHECK(projectedCopy->Axes.Storage == GroupAxisStorage::Local);
	const auto copyNode = std::find_if(projected.Nodes.begin(), projected.Nodes.end(), [](const auto &node) {
		return node.Id == "copy";
	});
	REQUIRE(copyNode != projected.Nodes.end());
	REQUIRE(copyNode->SourceSeparatedVec2Animators);
	CHECK(copyNode->SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.back().Data == Value{17.0});
	CHECK(copyNode->SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back().Data == Value{18.0});
}

TEST_CASE(
	"Ordinary source axis initialization seeds unbound inputs for future instance bindings",
	"[source_axis_initialization]"
) {
	auto document = AliasedGraph(true);
	document.Nodes[3].InstanceBase = "base";
	const std::array copyBinding{Binding("copy")};
	Diagnostic diagnostic;
	GroupReplayState empty, initial, cold;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(document, copyBinding, initial, 1, cold, diagnostic) == Status::Ok);
	const auto *coldCopy = cold.Binding("copy", Point);
	REQUIRE(coldCopy);
	CHECK(coldCopy->Axes.Storage == GroupAxisStorage::Uninitialized);

	const std::array target{SourceAxisInitialization{"base", Point}};
	GroupReplayState initialized;
	REQUIRE(InitializeSourceVec2Axes(document, target, cold, 1, initialized, diagnostic) == Status::Ok);
	const auto *baseAxes = initialized.SharedSubtype("base", Point);
	REQUIRE(baseAxes);
	REQUIRE(baseAxes->SeparatedVec2);
	CHECK_FALSE(baseAxes->SeparatedVec2->Separated);
	CHECK(baseAxes->SeparatedVec2->Axes[0].Keys.front().Data == Value{5.0});
	CHECK(baseAxes->SeparatedVec2->Axes[1].Keys.front().Data == Value{6.0});
	const auto *stillCold = initialized.Binding("copy", Point);
	REQUIRE(stillCold);
	CHECK(stillCold->Axes.Storage == GroupAxisStorage::Uninitialized);

	const std::array newBindings{Binding("copy"), Binding("late")};
	GroupReplayState rebound;
	REQUIRE(BindGroupReplay(document, newBindings, initialized, 1, rebound, diagnostic) == Status::Ok);
	const auto *oldCopy = rebound.Binding("copy", Point);
	REQUIRE(oldCopy);
	CHECK(oldCopy->Axes.Storage == GroupAxisStorage::Uninitialized);
	const auto *newCopy = rebound.Binding("late", Point);
	REQUIRE(newCopy);
	CHECK(newCopy->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(newCopy->Axes.OwnerId == "base");
}

TEST_CASE(
	"Axis initialization rejects duplicate and unbound instance provenance atomically",
	"[source_axis_initialization]"
) {
	auto document = AliasedGraph();
	const std::array bindings{Binding("copy"), Binding("cold")};
	Diagnostic diagnostic;
	GroupReplayState empty, initial, prior;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(document, bindings, initial, 1, prior, diagnostic) == Status::Ok);
	const auto originalBytes = prior.RetainedBytes();
	const std::array duplicate{
		SourceAxisInitialization{"copy", Point}, SourceAxisInitialization{"copy", Point}
	};
	CHECK(InitializeSourceVec2Axes(document, duplicate, prior, 1, prior, diagnostic) == Status::DuplicateId);
	CHECK(prior.RetainedBytes() == originalBytes);
	CHECK(prior.Binding("copy", Point)->Axes.Storage == GroupAxisStorage::Uninitialized);

	const std::array missing{SourceAxisInitialization{"late", Point}};
	GroupReplayState destination;
	REQUIRE(RebindGroupReplay(document, prior, 1, destination, diagnostic) == Status::Ok);
	const auto destinationBytes = destination.RetainedBytes();
	CHECK(
		InitializeSourceVec2Axes(document, missing, prior, 1, destination, diagnostic) == Status::InvalidGroup
	);
	CHECK(destination.RetainedBytes() == destinationBytes);
	const auto *destinationCopy = destination.Binding("copy", Point);
	REQUIRE(destinationCopy);
	CHECK(destinationCopy->Axes.Storage == GroupAxisStorage::Uninitialized);

	const std::array limitedTarget{SourceAxisInitialization{"copy", Point}};
	CHECK(
		InitializeSourceVec2Axes(document, limitedTarget, prior, 1, prior, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(prior.RetainedBytes() == originalBytes);
	const auto *priorCopy = prior.Binding("copy", Point);
	REQUIRE(priorCopy);
	CHECK(priorCopy->Axes.Storage == GroupAxisStorage::Uninitialized);
}

TEST_CASE(
	"Axis initialization refuses runtime preview defaults without inventing constructor history",
	"[source_axis_initialization]"
) {
	auto document = AliasedGraph();
	document.Nodes.push_back(
		{"mirror", "pc.mirror_polar", {}, {}, {{"constant_dimension", Vector2{99, 88}}}}
	);
	Diagnostic diagnostic;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(document, {}, initial, 1, bound, diagnostic) == Status::Ok);
	const auto before = bound.RetainedBytes();
	const std::array targets{
		SourceAxisInitialization{"base", Point}, SourceAxisInitialization{"mirror", "constant_dimension"}
	};
	CHECK(
		InitializeSourceVec2Axes(document, targets, bound, 1, bound, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(bound.RetainedBytes() == before);
	CHECK_FALSE(bound.SharedSubtype("base", Point));
	CHECK_FALSE(bound.SharedSubtype("mirror", "constant_dimension"));
	document.Nodes.back().SourceVec2Defaults.emplace().Inputs.push_back(
		{"constant_dimension", Vector2{17, 19}}
	);
	GroupReplayState initialized;
	REQUIRE(InitializeSourceVec2Axes(document, targets, bound, 1, initialized, diagnostic) == Status::Ok);
	const auto *axes = initialized.SharedSubtype("mirror", "constant_dimension");
	REQUIRE(axes);
	REQUIRE(axes->SeparatedVec2);
	CHECK(axes->SeparatedVec2->Axes[0].Keys.front().Data == Value{17.0});
	CHECK(axes->SeparatedVec2->Axes[1].Keys.front().Data == Value{19.0});
}

TEST_CASE(
	"Initialized overlays share the aggregate key limit with ordinary keys and later creations",
	"[source_axis_initialization]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {GradientNode("base"), {"number", "pc.number_simple", {}, {}, {{"value", 1.0}}}};
	Diagnostic diagnostic;
	GroupReplayState empty, initial, bound, first;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(document, {}, initial, 1, bound, diagnostic) == Status::Ok);
	const std::array firstTarget{SourceAxisInitialization{"base", Point}};
	REQUIRE(InitializeSourceVec2Axes(document, firstTarget, bound, 1, first, diagnostic) == Status::Ok);
	document.Keyframes.reserve(Limits::MaximumKeyframes - 3);
	for (size_t key = 0; key < Limits::MaximumKeyframes - 3; ++key)
		document.Keyframes.push_back({"number", "value", uint64_t(key), 1.0, "linear"});
	const std::array secondTarget{SourceAxisInitialization{"base", "point_i_1"}};
	const auto before = first.RetainedBytes();
	CHECK(
		InitializeSourceVec2Axes(document, secondTarget, first, 1, first, diagnostic) == Status::LimitExceeded
	);
	CHECK(diagnostic.Message.find("aggregate key") != std::string::npos);
	CHECK(first.RetainedBytes() == before);
	CHECK_FALSE(first.SharedSubtype("base", "point_i_1"));
	document.Keyframes.pop_back();
	GroupReplayState complete;
	REQUIRE(InitializeSourceVec2Axes(document, secondTarget, first, 1, complete, diagnostic) == Status::Ok);
	REQUIRE(complete.SharedSubtype("base", "point_i_1"));
	CHECK(complete.SharedSubtype("base", "point_i_1")->SeparatedVec2->Axes[0].Keys.size() == 1);
}

TEST_CASE(
	"Cold local flags roundtrip without becoming initialized empty arrays", "[source_axis_initialization]"
) {
	auto document = AliasedGraph(true);
	for (size_t index = 0; index < 2; ++index)
		document.Nodes[index].SourceSeparatedVec2Animators.emplace().Inputs.push_back(
			{std::string(Point), {}, index == 1, false}
		);
	const auto text = Write(document);
	REQUIRE_FALSE(text.empty());
	CHECK(text.find("source_vec2_axis_cold ") != std::string::npos);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK_FALSE(detail::FindInitializedSeparatedVec2(restored.Nodes[0], Point));
	CHECK_FALSE(detail::FindInitializedSeparatedVec2(restored.Nodes[1], Point));
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(restored, empty, 1, initial, diagnostic) == Status::Ok);
	const std::array bindings{Binding("copy")};
	REQUIRE(BindGroupReplay(restored, bindings, initial, 1, bound, diagnostic) == Status::Ok);
	REQUIRE(bound.Binding("copy", Point));
	CHECK(bound.Binding("copy", Point)->Axes.Storage == GroupAxisStorage::Uninitialized);
	uint64_t work = 0;
	const auto cold = detail::ResolveSourceGetterAxes(
		restored, restored.Nodes[1], Point, &bound, "base", Point, true, work
	);
	CHECK(cold.Separated);
	CHECK(cold.Code == Status::UnsupportedExecution);
	const std::array targets{
		SourceAxisInitialization{"base", Point}, SourceAxisInitialization{"copy", Point}
	};
	GroupReplayState initialized;
	REQUIRE(InitializeSourceVec2Axes(restored, targets, bound, 1, initialized, diagnostic) == Status::Ok);
	REQUIRE(initialized.SharedSubtype("base", Point));
	REQUIRE(initialized.SharedSubtype("copy", Point));
	CHECK(initialized.SharedSubtype("base", Point)->SeparatedVec2->Initialized);
	CHECK_FALSE(initialized.SharedSubtype("base", Point)->SeparatedVec2->Separated);
	CHECK(initialized.SharedSubtype("base", Point)->SeparatedVec2->Axes[0].Keys.front().Data == Value{5.0});
	CHECK(initialized.SharedSubtype("copy", Point)->SeparatedVec2->Initialized);
	CHECK(initialized.SharedSubtype("copy", Point)->SeparatedVec2->Separated);
	CHECK(initialized.SharedSubtype("copy", Point)->SeparatedVec2->Axes[0].Keys.front().Data == Value{7.0});
	Document projected;
	REQUIRE(ProjectGroupReplay(restored, initialized, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Initialized);
	CHECK(projected.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Initialized);

	auto warm = document;
	warm.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Initialized = true;
	GroupReplayState warmInitial, warmBound;
	REQUIRE(RebindGroupReplay(warm, empty, 1, warmInitial, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(warm, bindings, warmInitial, 1, warmBound, diagnostic) == Status::Ok);
	CHECK(warmBound.Binding("copy", Point)->Axes.Storage == GroupAxisStorage::Shared);
	const auto warmText = Write(warm);
	CHECK(warmText.find("source_vec2_axis ") != std::string::npos);
	REQUIRE(Read(warmText, restored, diagnostic) == Status::Ok);
	CHECK(restored == warm);
}

TEST_CASE(
	"Cold scalar markers reject mixed storage and keys without changing prior output",
	"[source_axis_initialization]"
) {
	auto document = AliasedGraph();
	document.Nodes[0].SourceSeparatedVec2Animators.emplace().Inputs.push_back(
		{std::string(Point), {}, true, false}
	);
	const auto text = Write(document);
	REQUIRE_FALSE(text.empty());
	Document output = document;
	Diagnostic diagnostic;
	const std::array malformed{
		text + "source_vec2_axis_cold \"base\" \"point_i_0\" 1\n",
		text + "source_vec2_axis \"base\" \"point_i_0\" x\nsource_vec2_axis_end\n",
		text + "source_vec2_axis_cold \"copy\" \"point_i_0\" 2\n"
	};
	for (const auto &encoded : malformed) {
		CHECK(Read(encoded, output, diagnostic) != Status::Ok);
		CHECK(output == document);
	}
	auto warm = document;
	warm.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Initialized = true;
	const auto warmText = Write(warm);
	CHECK(
		Read(warmText + "source_vec2_axis_cold \"base\" \"point_i_0\" 1\n", output, diagnostic) != Status::Ok
	);
	CHECK(output == document);
	AddAxes(document.Nodes[1], 1, 2);
	auto &invalid = document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	invalid.Initialized = false;
	size_t count = 0;
	CHECK(detail::ValidateSeparatedVec2(document.Nodes[1], count, diagnostic) == Status::InvalidValue);
	CHECK_FALSE(detail::SeparatedAnimatorBytes(invalid, false));
	CHECK(Write(document).empty());
}
