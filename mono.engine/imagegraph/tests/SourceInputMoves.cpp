#include "../src/SourceInputEvaluation.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <string>
TEST_SUITE_ID("engine.imagegraph.source_input_moves")
using namespace engine::imagegraph;
TEST_CASE(
	"A moved instance keeps its current getter index and its original animator writer", "[source_input_moves]"
) {
	Document doc;
	doc.FormatVersion = 9;
	for (const auto *id : {"base", "copy", "sibling"}) {
		Node node{id, "pc.lua_compute", "", {}, {}};
		for (size_t n = 0; n < 3; ++n) {
			auto suffix = std::to_string(n);
			node.DynamicInputs.push_back(
				{"argument_name_" + suffix, ValueType::Text, Value{std::string("v") + suffix}}
			);
			node.DynamicInputs.push_back({"argument_type_" + suffix, ValueType::Enum, Value{EnumValue{0}}});
			node.DynamicInputs.push_back(
				{"argument_value_" + suffix, ValueType::Scalar, Value{double(n + 1) * 10}}
			);
			node.SourceAnimatedInputs.push_back("argument_value_" + suffix);
		}
		if (node.Id != "base") node.InstanceBase = "base";
		doc.Nodes.push_back(std::move(node));
	}
	for (size_t n = 0; n < 3; ++n) {
		const auto port = "argument_value_" + std::to_string(n);
		doc.Keyframes.push_back({"base", port, 0, double(n + 1) * 10, "source", KeyframeEase{}});
		doc.Tracks.push_back({"base", port, "hold", -1});
	}
	doc.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}}
	);
	doc.Outputs = {{"out", "solid", "image"}};
	const bool overridden = GENERATE(false, true);
	if (overridden) doc.Nodes[1].InstanceOverrides.push_back("argument_value_2");
	Diagnostic error;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(doc, empty, 1, initial, error) == Status::Ok);
	std::vector<GroupSubtypeBinding> bindings;
	for (const auto *id : {"copy", "sibling"})
		for (size_t n = 0; n < 3; ++n)
			bindings.push_back(
				{id,
				 "base",
				 GroupSubtypeAnimator::Animated,
				 GroupSubtypeAnimator::Animated,
				 "argument_value_" + std::to_string(n)}
			);
	REQUIRE(BindGroupReplay(doc, bindings, initial, 1, bound, error) == Status::Ok);
	auto capture = [&](const Document &graph,
					   const GroupReplayState &state,
					   std::string_view node,
					   std::string_view port,
					   uint64_t tick = 0) {
		Plan plan;
		auto status = Compile(graph, plan, error);
		INFO(error.Message);
		INFO(error.NodeId);
		INFO(error.Port);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		request.GroupReplay = &state;
		request.GroupAuthoringRevision = state.AuthoringRevision();
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(graph, plan, node, request, snapshot, error) == Status::Ok);
		auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &v) {
			return v.Port == port;
		});
		REQUIRE(value != snapshot.Values().end());
		return value->Data;
	};
	CHECK(capture(doc, bound, "copy", "argument_value_2") == Value{30.});
	auto staged = doc;
	auto &copy = staged.Nodes[1];
	copy.DynamicInputs.erase(copy.DynamicInputs.begin() + 3, copy.DynamicInputs.begin() + 6);
	copy.DynamicInputs[3].Id = "argument_name_1";
	copy.DynamicInputs[4].Id = "argument_type_1";
	copy.DynamicInputs[5].Id = "argument_value_1";
	copy.SourceAnimatedInputs = {"argument_value_0", "argument_value_1"};
	if (overridden) copy.InstanceOverrides = {"argument_value_1"};
	const SourceInputMove movesExact[]{
		{"copy", "argument_name_1", ""},
		{"copy", "argument_type_1", ""},
		{"copy", "argument_value_1", ""},
		{"copy", "argument_name_2", "argument_name_1"},
		{"copy", "argument_type_2", "argument_type_1"},
		{"copy", "argument_value_2", "argument_value_1"}
	};
	GroupReplayState rebound;
	REQUIRE(RebindGroupReplayWithInputMoves(doc, staged, movesExact, bound, 2, rebound, error) == Status::Ok);
	REQUIRE(rebound.Binding("copy", "argument_value_1"));
	CHECK(rebound.Binding("copy", "argument_value_1")->AnimatorPort == "argument_value_2");
	CHECK(rebound.Binding("copy", "argument_value_1")->OwnerId == "base");
	CHECK(capture(staged, rebound, "copy", "argument_value_1") == Value{overridden ? 30. : 20.});
	CHECK(capture(staged, rebound, "sibling", "argument_value_2") == Value{30.});
	CHECK(doc.Keyframes == staged.Keyframes);
	Value editedValue = 99.;
	GroupRefreshEvent event;
	event.NodeId = "copy";
	event.Reason = GroupRefreshReason::Edit;
	event.EditedPort = "argument_value_1";
	event.LocalValue = &editedValue;
	event.LocalAnimated = true;
	event.At.Tick = 2;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(staged, {&event, 1}, rebound, 2, edited, error) == Status::Ok);
	REQUIRE(edited.SharedSubtype("base", "argument_value_2"));
	CHECK(edited.SharedSubtype("base", "argument_value_2")->Keys.back().Data == editedValue);
	CHECK(edited.SharedSubtype("base", "argument_value_1") == nullptr);
	CHECK(capture(staged, edited, "copy", "argument_value_1", 2) == Value{overridden ? 99. : 20.});
	CHECK(capture(staged, edited, "sibling", "argument_value_2", 2) == editedValue);
	// A later host Bind may rebuild current socket names. The retained writer must survive.
	std::erase_if(bindings, [](const auto &binding) {
		return binding.NodeId == "copy" && binding.Port == "argument_value_2";
	});
	GroupReplayState reconciled;
	REQUIRE(BindGroupReplay(staged, bindings, edited, 2, reconciled, error) == Status::Ok);
	CHECK(reconciled.Binding("copy", "argument_value_1")->AnimatorPort == "argument_value_2");
	CHECK(capture(staged, reconciled, "copy", "argument_value_1", 2) == Value{overridden ? 99. : 20.});
	CHECK(capture(staged, reconciled, "sibling", "argument_value_2", 2) == editedValue);
	Document projected;
	REQUIRE(ProjectGroupReplay(staged, reconciled, 2, projected, error) == Status::Ok);
	CHECK(projected.Keyframes.back().NodeId == "base");
	CHECK(projected.Keyframes.back().Port == "argument_value_2");
	CHECK(projected.Keyframes.back().Data == editedValue);
	GroupReplayState cleared;
	REQUIRE(RebindProjectedGroupReplay(projected, reconciled, 2, cleared, error) == Status::Ok);
	CHECK(capture(projected, cleared, "copy", "argument_value_1", 2) == Value{overridden ? 99. : 20.});
	CHECK(capture(projected, cleared, "sibling", "argument_value_2", 2) == editedValue);
}

namespace {
	Document MakeMoveGraph() {
		Document doc;
		doc.FormatVersion = 9;
		for (const auto *id : {"base", "copy", "sibling"}) {
			Node node{id, "pc.lua_compute", "", {}, {}};
			for (size_t n = 0; n < 3; ++n) {
				auto suffix = std::to_string(n);
				node.DynamicInputs.push_back(
					{"argument_name_" + suffix, ValueType::Text, Value{std::string("v") + suffix}}
				);
				node.DynamicInputs.push_back(
					{"argument_type_" + suffix, ValueType::Enum, Value{EnumValue{0}}}
				);
				node.DynamicInputs.push_back(
					{"argument_value_" + suffix, ValueType::Scalar, Value{double(n + 1) * 10}}
				);
				node.SourceAnimatedInputs.push_back("argument_value_" + suffix);
			}
			if (node.Id != "base") node.InstanceBase = "base";
			doc.Nodes.push_back(std::move(node));
		}
		for (size_t n = 0; n < 3; ++n) {
			const auto port = "argument_value_" + std::to_string(n);
			doc.Keyframes.push_back({"base", port, 0, double(n + 1) * 10, "source", KeyframeEase{}});
			doc.Tracks.push_back({"base", port, "hold", -1});
		}
		doc.Nodes.push_back(
			{"solid",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}}
		);
		doc.Outputs = {{"out", "solid", "image"}};
		return doc;
	}
	const SourceInputMove CopyMoves[]{
		{"copy", "argument_name_1", ""},
		{"copy", "argument_type_1", ""},
		{"copy", "argument_value_1", ""},
		{"copy", "argument_name_2", "argument_name_1"},
		{"copy", "argument_type_2", "argument_type_1"},
		{"copy", "argument_value_2", "argument_value_1"}
	};
	void StageMove(Document &doc, std::string_view id) {
		auto &node =
			*std::find_if(doc.Nodes.begin(), doc.Nodes.end(), [&](const auto &n) { return n.Id == id; });
		node.DynamicInputs.erase(node.DynamicInputs.begin() + 3, node.DynamicInputs.begin() + 6);
		for (size_t i = 3; i < 6; ++i)
			node.DynamicInputs[i].Id.back() = '1';
		for (auto *ports : {&node.SourceAnimatedInputs, &node.SourceStaticInputs, &node.InstanceOverrides}) {
			std::erase(*ports, "argument_value_1");
			for (auto &port : *ports)
				if (port == "argument_value_2") port = "argument_value_1";
		}
		std::erase_if(doc.Keyframes, [&](const auto &k) {
			return k.NodeId == id && k.Port == "argument_value_1";
		});
		for (auto &key : doc.Keyframes)
			if (key.NodeId == id && key.Port == "argument_value_2") key.Port = "argument_value_1";
		std::erase_if(doc.Tracks, [&](const auto &t) {
			return t.NodeId == id && t.Port == "argument_value_1";
		});
		for (auto &track : doc.Tracks)
			if (track.NodeId == id && track.Port == "argument_value_2") track.Port = "argument_value_1";
		for (auto &exp : node.SourceInputExpressions)
			if (exp.Port == "argument_value_2") exp.Port = "argument_value_1";
	}
	GroupReplayState BindMoveGraph(const Document &doc) {
		Diagnostic error;
		GroupReplayState empty, initial, bound;
		REQUIRE(RebindGroupReplay(doc, empty, 1, initial, error) == Status::Ok);
		std::vector<GroupSubtypeBinding> bindings;
		for (const auto *id : {"copy", "sibling"})
			for (size_t n = 0; n < 3; ++n)
				bindings.push_back(
					{id,
					 "base",
					 GroupSubtypeAnimator::Animated,
					 GroupSubtypeAnimator::Animated,
					 "argument_value_" + std::to_string(n)}
				);
		REQUIRE(BindGroupReplay(doc, bindings, initial, 1, bound, error) == Status::Ok);
		return bound;
	}
	Value CaptureMove(
		const Document &doc,
		const GroupReplayState &state,
		std::string_view node,
		std::string_view port,
		uint64_t tick = 2
	) {
		Diagnostic error;
		Plan plan;
		REQUIRE(Compile(doc, plan, error) == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		request.GroupReplay = &state;
		request.GroupAuthoringRevision = state.AuthoringRevision();
		EvaluationSnapshot snapshot;
		auto code = EvaluateNodeInputs(doc, plan, node, request, snapshot, error);
		INFO(error.Message);
		REQUIRE(code == Status::Ok);
		auto found = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &v) {
			return v.Port == port;
		});
		REQUIRE(found != snapshot.Values().end());
		return found->Data;
	}
}
TEST_CASE(
	"Local source moves keep effects and later writes on the surviving animator", "[source_input_moves]"
) {
	auto original = MakeMoveGraph();
	Diagnostic error;
	GroupReplayState empty, prior;
	Value value = 77.;
	GroupRefreshEvent event;
	event.NodeId = "base";
	event.Reason = GroupRefreshReason::Edit;
	event.EditedPort = "argument_value_2";
	event.LocalValue = &value;
	event.LocalAnimated = true;
	event.At.Tick = 1;
	GroupReplayState initialized;
	REQUIRE(RebindGroupReplay(original, empty, 1, initialized, error) == Status::Ok);
	GroupReplayState locallyBound;
	REQUIRE(BindGroupReplay(original, {}, initialized, 1, locallyBound, error) == Status::Ok);
	REQUIRE(ReplayGroupAnimatorEdits(original, {&event, 1}, locallyBound, 1, prior, error) == Status::Ok);
	auto staged = original;
	StageMove(staged, "base");
	SourceInputMove moves[6];
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), moves);
	for (auto &m : moves)
		m.NodeId = "base";
	GroupReplayState moved;
	REQUIRE(RebindGroupReplayWithInputMoves(original, staged, moves, prior, 2, moved, error) == Status::Ok);
	CHECK(moved.SharedSubtype("base", "argument_value_2") == nullptr);
	REQUIRE(moved.SharedSubtype("base", "argument_value_1"));
	CHECK(CaptureMove(staged, moved, "base", "argument_value_1") == value);
	value = 88.;
	event.EditedPort = "argument_value_1";
	event.At.Tick = 3;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(staged, {&event, 1}, moved, 2, edited, error) == Status::Ok);
	CHECK(CaptureMove(staged, edited, "base", "argument_value_1", 3) == value);
	CHECK(edited.SharedSubtype("base", "argument_value_1")->Keys.back().Port == "argument_value_1");
}
TEST_CASE("Moved source mode toggles mutate the retained original animator only", "[source_input_moves]") {
	auto original = MakeMoveGraph();
	original.Nodes[1].InstanceOverrides = {"argument_value_2"};
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "copy");
	Diagnostic error;
	GroupReplayState moved;
	REQUIRE(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, moved, error) == Status::Ok
	);
	Document toggled;
	SourceModeTransition mode{"copy", "argument_value_1", false, FrameTime{2}};
	REQUIRE(ToggleSourceInputMode(staged, moved, 2, mode, toggled, error) == Status::Ok);
	CHECK(
		std::find(
			toggled.Nodes[1].SourceStaticInputs.begin(),
			toggled.Nodes[1].SourceStaticInputs.end(),
			"argument_value_1"
		) != toggled.Nodes[1].SourceStaticInputs.end()
	);
	CHECK(std::count_if(toggled.Keyframes.begin(), toggled.Keyframes.end(), [](const auto &k) {
			  return k.NodeId == "base" && k.Port == "argument_value_1";
		  }) == 1);
	auto retained = std::find_if(toggled.Keyframes.begin(), toggled.Keyframes.end(), [](const auto &k) {
		return k.NodeId == "base" && k.Port == "argument_value_2";
	});
	REQUIRE(retained != toggled.Keyframes.end());
	CHECK(retained->Tick == 0);
	CHECK(retained->Data == Value{30.});
	CHECK(toggled.Nodes[0].SourceAnimatedInputs == staged.Nodes[0].SourceAnimatedInputs);
}
TEST_CASE("Physical source move refusal preserves prior and separate destination", "[source_input_moves]") {
	auto original = MakeMoveGraph();
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "copy");
	Diagnostic error;
	GroupReplayState output;
	REQUIRE(RebindGroupReplay(original, bound, 1, output, error) == Status::Ok);
	const auto held = output.RetainedBytes();
	std::vector<SourceInputMove> duplicate(std::begin(CopyMoves), std::end(CopyMoves));
	duplicate.push_back(CopyMoves[0]);
	CHECK(
		RebindGroupReplayWithInputMoves(original, staged, duplicate, bound, 2, output, error) ==
		Status::InvalidValue
	);
	CHECK(output.AuthoringRevision() == 1);
	CHECK(output.RetainedBytes() == held);
	auto malformed = staged;
	malformed.Nodes[1].DynamicInputs.back().Id = "argument_value_0";
	CHECK(
		RebindGroupReplayWithInputMoves(original, malformed, CopyMoves, bound, 2, output, error) ==
		Status::InvalidGroup
	);
	CHECK(output.RetainedBytes() == held);
	uint64_t lo = 1, hi = Limits::MaximumEvaluationBytes;
	while (lo < hi) {
		const auto mid = lo + (hi - lo) / 2;
		GroupReplayState candidate;
		auto code =
			RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, candidate, error, mid);
		if (code == Status::Ok)
			hi = mid;
		else {
			REQUIRE(code == Status::LimitExceeded);
			lo = mid + 1;
		}
	}
	GroupReplayState exact;
	REQUIRE(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, exact, error, lo) == Status::Ok
	);
	CHECK(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, exact, error, lo - 1) ==
		Status::LimitExceeded
	);
	CHECK(exact.AuthoringRevision() == 2);
	CHECK(exact.Binding("copy", "argument_value_1")->AnimatorPort == "argument_value_2");
	// The populated separate destination overlaps the candidate and borrowed states.
	CHECK(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, output, error, lo) ==
		Status::LimitExceeded
	);
	CHECK(output.RetainedBytes() == held);
	REQUIRE(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, output, 2, output, error) == Status::Ok
	);
	CHECK(output.Binding("copy", "argument_value_1")->AnimatorPort == "argument_value_2");
}
TEST_CASE("Source input links keep priority over a moved retained animator", "[source_input_moves]") {
	auto original = MakeMoveGraph();
	original.Nodes.push_back({"literal", "pc.number", "", {}, {{"value", 123.}}});
	const bool junction = GENERATE(false, true);
	if (junction) {
		original.Nodes.pop_back();
		original.Junctions = {{"literal", "", ValueType::Scalar, Value{123.}}};
	}
	original.Links = {{"literal", junction ? "value" : "number", "copy", "argument_value_2"}};
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "copy");
	staged.Links[0].ToPort = "argument_value_1";
	Diagnostic error;
	GroupReplayState moved;
	REQUIRE(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, moved, error) == Status::Ok
	);
	CHECK(CaptureMove(staged, moved, "copy", "argument_value_1") == Value{123.});
	CHECK(CaptureMove(staged, moved, "sibling", "argument_value_2") == Value{30.});
}
TEST_CASE(
	"Moved input expressions follow current inheritance rather than retained writer identity",
	"[source_input_moves]"
) {
	auto original = MakeMoveGraph();
	const bool overridden = GENERATE(false, true);
	original.Nodes[0].SourceInputExpressions = {{"argument_value_1", "value+1", true}};
	original.Nodes[1].SourceInputExpressions = {{"argument_value_2", "value+100", true}};
	if (overridden) original.Nodes[1].InstanceOverrides = {"argument_value_2"};
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "copy");
	Diagnostic error;
	GroupReplayState moved;
	REQUIRE(
		RebindGroupReplayWithInputMoves(original, staged, CopyMoves, bound, 2, moved, error) == Status::Ok
	);
	const auto sampled = CaptureMove(staged, moved, "copy", "argument_value_1");
	INFO(overridden);
	INFO(std::get<double>(sampled));
	CHECK(sampled == Value{overridden ? 130. : 21.});
}
TEST_CASE(
	"Dynamic struct input resolves a literal Junction before its local animator", "[source_input_moves]"
) {
	auto doc = MakeMoveGraph();
	Node object{"record", "pc.struct", "", {}, {}};
	object.DynamicInputs = {
		{"key_0", ValueType::Text, Value{std::string("answer")}}, {"value_0", ValueType::Any, Value{0.}}
	};
	doc.Nodes.push_back(std::move(object));
	doc.Junctions = {{"literal", "", ValueType::Scalar, Value{44.}}};
	doc.Links = {{"literal", "value", "record", "value_0"}};
	Diagnostic error;
	GroupReplayState empty, prior;
	REQUIRE(RebindGroupReplay(doc, empty, 1, prior, error) == Status::Ok);
	GroupReplayState bound;
	REQUIRE(BindGroupReplay(doc, {}, prior, 1, bound, error) == Status::Ok);
	CHECK(CaptureMove(doc, bound, "record", "value_0") == Value{44.});
}

namespace {
	constexpr std::string_view Point0 = "point_i_0";
	constexpr std::string_view Point1 = "point_i_1";

	Document MakeGradientPointMoveGraph() {
		Document document;
		document.FormatVersion = 9;
		Node owner{"base", "pc.gradient_points_n", "", {}, {}};
		owner.Values = {
			{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}, {"blend_mode", EnumValue{0}}
		};
		owner.DynamicInputs = {
			{std::string(Point0), ValueType::Vector2, Vector2{10, 1}},
			{std::string(Point1), ValueType::Vector2, Vector2{30, 3}}
		};
		owner.SourceAnimatedInputs = {std::string(Point0)};
		SourceSeparatedVec2Animator axes;
		axes.Port = Point0;
		axes.Axes[0].Keys = {
			{"base", std::string(Point0), 0, 10.0, "source", KeyframeEase{}},
			{"base", std::string(Point0), 10, 20.0, "source", KeyframeEase{}}
		};
		axes.Axes[1].Keys = {{"base", std::string(Point0), 0, 2.0, "source", KeyframeEase{}}};
		axes.Axes[0].Keys[0].SourceKeyId = "point-x-start";
		axes.Axes[0].Keys[1].SourceKeyId = "point-x-end";
		axes.Axes[1].Keys[0].SourceKeyId = "point-y-start";
		owner.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		Node copy = owner;
		copy.Id = "copy";
		copy.InstanceBase = "base";
		copy.SourceSeparatedVec2Animators.emplace().Inputs.clear();
		SourceSeparatedVec2Animator local;
		local.Port = Point0;
		local.Separated = true;
		copy.SourceSeparatedVec2Animators->Inputs.push_back(std::move(local));
		document.Nodes = {std::move(owner), std::move(copy)};
		document.Outputs = {{"image", "base", "surface_out"}};
		return document;
	}

	std::vector<GroupSubtypeBinding> GradientPointBindings() {
		return {
			{"copy",
			 "base",
			 GroupSubtypeAnimator::Animated,
			 GroupSubtypeAnimator::Animated,
			 std::string(Point0)}
		};
	}

	GroupSubtypeBinding GradientPointBinding(std::string_view nodeId) {
		return {
			std::string(nodeId),
			"base",
			GroupSubtypeAnimator::Animated,
			GroupSubtypeAnimator::Animated,
			std::string(Point0)
		};
	}

	void RenamePointInputs(Node &node, std::string_view from, std::string_view to) {
		for (auto &input : node.DynamicInputs)
			if (input.Id == from) input.Id = to;
		for (auto *ports : {&node.SourceAnimatedInputs, &node.SourceStaticInputs, &node.InstanceOverrides})
			for (auto &port : *ports)
				if (port == from) port = to;
		if (node.SourceSeparatedVec2Animators)
			for (auto &axes : node.SourceSeparatedVec2Animators->Inputs)
				if (axes.Port == from) {
					axes.Port = to;
					for (auto &axis : axes.Axes)
						for (auto &key : axis.Keys)
							key.Port = to;
				}
	}

	void RemovePointInput(Node &node, std::string_view port) {
		std::erase_if(node.DynamicInputs, [&](const auto &input) { return input.Id == port; });
		for (auto *ports : {&node.SourceAnimatedInputs, &node.SourceStaticInputs, &node.InstanceOverrides})
			std::erase(*ports, port);
		if (node.SourceSeparatedVec2Animators) {
			std::erase_if(node.SourceSeparatedVec2Animators->Inputs, [&](const auto &axes) {
				return axes.Port == port;
			});
			if (node.SourceSeparatedVec2Animators->Inputs.empty()) node.SourceSeparatedVec2Animators = {};
		}
	}

	Value SamplePoint(
		const Document &document,
		const GroupReplayState &replay,
		std::string_view nodeId,
		std::string_view port,
		uint64_t tick
	) {
		Diagnostic diagnostic;
		EvaluationRequest request;
		request.Tick = tick;
		request.GroupReplay = &replay;
		request.GroupAuthoringRevision = replay.AuthoringRevision();
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		REQUIRE(documentBytes);
		auto documentCharge = budget.Reserve(*documentBytes);
		REQUIRE(documentCharge);
		detail::AllocationReservation charge;
		Value result;
		const auto status =
			detail::EvaluateSourceInput(document, nodeId, port, request, budget, result, charge, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
}

TEST_CASE("Dynamic Vec2 input move carries split edit overlay onto the new socket", "[source_input_moves]") {
	auto original = MakeGradientPointMoveGraph();
	original.Nodes[1].InstanceOverrides = {std::string(Point0)};
	Diagnostic error;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(original, empty, 1, initial, error) == Status::Ok);
	auto bindings = GradientPointBindings();
	REQUIRE(BindGroupReplay(original, bindings, initial, 1, bound, error) == Status::Ok);
	const Value editedValue = Vector2{8, 4};
	GroupRefreshEvent edit;
	edit.NodeId = "copy";
	edit.Reason = GroupRefreshReason::Edit;
	edit.EditedPort = Point0;
	edit.LocalValue = &editedValue;
	edit.LocalAnimated = true;
	edit.At.Tick = 5;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(original, {&edit, 1}, bound, 1, edited, error) == Status::Ok);
	const auto *beforeMove = edited.SharedSubtype("base", Point0);
	REQUIRE(beforeMove);
	REQUIRE(beforeMove->SeparatedVec2);

	auto staged = original;
	RemovePointInput(staged.Nodes[0], Point1);
	RenamePointInputs(staged.Nodes[0], Point0, Point1);
	const SourceInputMove moves[] = {
		{"base", std::string(Point0), std::string(Point1)}, {"base", std::string(Point1), ""}
	};
	GroupReplayState moved;
	const auto status = RebindGroupReplayWithInputMoves(original, staged, moves, edited, 2, moved, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto *overlay = moved.SharedSubtype("base", Point1);
	REQUIRE(overlay);
	CHECK(overlay->Port == Point1);
	REQUIRE(overlay->SeparatedVec2);
	CHECK(overlay->SeparatedVec2->Port == Point1);
	for (const auto &axis : overlay->SeparatedVec2->Axes)
		for (const auto &key : axis.Keys)
			CHECK(key.Port == Point1);
	REQUIRE(moved.Binding("copy", Point0));
	CHECK(moved.Binding("copy", Point0)->AnimatorPort == Point1);
	CHECK(moved.Binding("copy", Point0)->OwnerId == "base");
	CHECK(moved.Binding("copy", Point0)->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(moved.Binding("copy", Point0)->Axes.OwnerId == "base");
	CHECK(moved.Binding("copy", Point0)->Axes.Port == Point1);

	GroupReplayState cloned;
	REQUIRE(RebindGroupReplay(staged, moved, 3, cloned, error) == Status::Ok);
	CHECK(SamplePoint(staged, cloned, "copy", Point0, 5) == Value{editedValue});
}

TEST_CASE(
	"Deleting a dynamic Vec2 writer retains axes for overridden alias sampling and rebind",
	"[source_input_moves]"
) {
	auto original = MakeGradientPointMoveGraph();
	const bool separated = GENERATE(false, true);
	original.Nodes[1].SourceSeparatedVec2Animators->Inputs[0].Separated = separated;
	Diagnostic error;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(original, empty, 1, initial, error) == Status::Ok);
	auto bindings = GradientPointBindings();
	REQUIRE(BindGroupReplay(original, bindings, initial, 1, bound, error) == Status::Ok);
	auto staged = original;
	RemovePointInput(staged.Nodes[0], Point0);
	RenamePointInputs(staged.Nodes[0], Point1, Point0);
	staged.Nodes[1].InstanceOverrides = {std::string(Point0)};
	const SourceInputMove moves[] = {
		{"base", std::string(Point0), ""}, {"base", std::string(Point1), std::string(Point0)}
	};
	GroupReplayState detached;
	const auto status = RebindGroupReplayWithInputMoves(original, staged, moves, bound, 2, detached, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(detached.DetachedAnimators().size() == 1);
	GroupReplayState refused;
	REQUIRE(BindGroupReplay(original, bindings, initial, 1, refused, error) == Status::Ok);
	const auto retainedBytes = refused.RetainedBytes();
	REQUIRE(
		RebindGroupReplayWithInputMoves(original, staged, moves, bound, 2, refused, error, 1) ==
		Status::LimitExceeded
	);
	CHECK(refused.RetainedBytes() == retainedBytes);
	CHECK(refused.AuthoringRevision() == 1);
	CHECK(refused.DetachedAnimators().empty());
	CHECK(refused.Binding("copy", Point0)->AnimatorPort == bound.Binding("copy", Point0)->AnimatorPort);
	const auto identity = detached.DetachedAnimators().front().Id;
	const auto *detachedBinding = detached.Binding("copy", Point0);
	REQUIRE(detachedBinding);
	CHECK(detachedBinding->OwnerId == "base");
	CHECK(detachedBinding->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(detachedBinding->Axes.OwnerId == "base");
	CHECK(detachedBinding->Axes.Port == identity);
	const auto *overlay = detached.SharedSubtype("base", identity);
	REQUIRE(overlay);
	CHECK(overlay->Port == identity);
	REQUIRE(overlay->SeparatedVec2);
	CHECK(overlay->SeparatedVec2->Port == identity);
	for (const auto &axis : overlay->SeparatedVec2->Axes)
		for (const auto &key : axis.Keys)
			CHECK(key.Port == identity);

	const Value expected = separated ? Value{Vector2{15, 2}} : Value{Vector2{10, 1}};
	CHECK(SamplePoint(staged, detached, "copy", Point0, 5) == expected);
	GroupReplayState rebound;
	REQUIRE(RebindGroupReplay(staged, detached, 3, rebound, error) == Status::Ok);
	CHECK(SamplePoint(staged, rebound, "copy", Point0, 5) == expected);
	Document projected = original;
	const auto saved = Write(projected);
	REQUIRE(ProjectGroupReplay(staged, rebound, 3, projected, error, 1) == Status::LimitExceeded);
	CHECK(Write(projected) == saved);
	REQUIRE(ProjectGroupReplay(staged, rebound, 3, projected, error) == Status::Ok);
	const auto &projectedAxes = projected.Nodes[1].SourceSeparatedVec2Animators->Inputs[0];
	CHECK(projectedAxes.Separated == separated);
	for (const auto &axis : projectedAxes.Axes)
		for (const auto &key : axis.Keys) {
			CHECK(key.SourceKeyId.empty());
			CHECK(key.NodeId == "copy");
			CHECK(key.Port == Point0);
		}
	GroupReplayState projectedReplay;
	REQUIRE(RebindProjectedGroupReplay(projected, rebound, 3, projectedReplay, error) == Status::Ok);
	CHECK(SamplePoint(projected, projectedReplay, "copy", Point0, 5) == expected);
	const auto reboundBytes = projectedReplay.RetainedBytes();
	projected.Nodes[1].SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].Data = 999.;
	REQUIRE(
		RebindProjectedGroupReplay(projected, rebound, 3, projectedReplay, error) == Status::InvalidValue
	);
	CHECK(projectedReplay.AuthoringRevision() == 3);
	CHECK(projectedReplay.RetainedBytes() == reboundBytes);
	CHECK(SamplePoint(staged, projectedReplay, "copy", Point0, 5) == expected);
}

TEST_CASE(
	"Bound Vec2 axis storage keeps its captured owner across late initialization and nested aliases",
	"[source_input_moves][mirror_axes]"
) {
	auto warmDocument = MakeGradientPointMoveGraph();
	Diagnostic error;
	GroupReplayState empty, warmInitial, warm;
	REQUIRE(RebindGroupReplay(warmDocument, empty, 1, warmInitial, error) == Status::Ok);
	auto bindings = GradientPointBindings();
	REQUIRE(BindGroupReplay(warmDocument, bindings, warmInitial, 1, warm, error) == Status::Ok);
	const auto *warmCopy = warm.Binding("copy", Point0);
	REQUIRE(warmCopy);
	CHECK(warmCopy->OwnerId == "base");
	CHECK(warmCopy->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(warmCopy->Axes.OwnerId == "base");
	CHECK(warmCopy->Axes.Port == Point0);
	CHECK(warmCopy->Axes.InstanceBase == "base");
	CHECK(warmCopy->Axes.Writer == GroupSubtypeAnimator::Animated);

	auto coldDocument = MakeGradientPointMoveGraph();
	coldDocument.Nodes.front().SourceSeparatedVec2Animators = {};
	GroupReplayState coldInitial, cold;
	REQUIRE(RebindGroupReplay(coldDocument, empty, 1, coldInitial, error) == Status::Ok);
	REQUIRE(BindGroupReplay(coldDocument, bindings, coldInitial, 1, cold, error) == Status::Ok);
	const auto *coldCopy = cold.Binding("copy", Point0);
	REQUIRE(coldCopy);
	CHECK(coldCopy->OwnerId == "base");
	CHECK(coldCopy->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(coldCopy->Axes.OwnerId == "copy");
	CHECK(coldCopy->Axes.Port == Point0);
	CHECK(coldCopy->Axes.InstanceBase == "base");

	coldDocument.Nodes.front().SourceSeparatedVec2Animators.emplace() =
		*warmDocument.Nodes.front().SourceSeparatedVec2Animators;
	GroupReplayState revised, refreshed;
	REQUIRE(RebindGroupReplay(coldDocument, cold, 2, revised, error) == Status::Ok);
	REQUIRE(BindGroupReplay(coldDocument, bindings, revised, 2, refreshed, error) == Status::Ok);
	const auto *retainedCold = refreshed.Binding("copy", Point0);
	REQUIRE(retainedCold);
	CHECK(retainedCold->OwnerId == "base");
	CHECK(retainedCold->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(retainedCold->Axes.OwnerId == "copy");
	CHECK(retainedCold->Axes.Port == Point0);
	CHECK(retainedCold->Axes.InstanceBase == "base");

	Node late = coldDocument.Nodes[1];
	late.Id = "late";
	late.InstanceBase = "base";
	Node nested = coldDocument.Nodes[1];
	nested.Id = "nested";
	nested.InstanceBase = "copy";
	coldDocument.Nodes.push_back(std::move(late));
	coldDocument.Nodes.push_back(std::move(nested));
	bindings.push_back(GradientPointBinding("late"));
	bindings.push_back(GradientPointBinding("nested"));
	GroupReplayState aliasesInitial, aliases;
	REQUIRE(RebindGroupReplay(coldDocument, refreshed, 3, aliasesInitial, error) == Status::Ok);
	REQUIRE(BindGroupReplay(coldDocument, bindings, aliasesInitial, 3, aliases, error) == Status::Ok);
	const auto *oldCopy = aliases.Binding("copy", Point0);
	const auto *lateShared = aliases.Binding("late", Point0);
	const auto *nestedCold = aliases.Binding("nested", Point0);
	REQUIRE(oldCopy);
	REQUIRE(lateShared);
	REQUIRE(nestedCold);
	CHECK(oldCopy->OwnerId == "base");
	CHECK(oldCopy->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(oldCopy->Axes.OwnerId == "copy");
	CHECK(oldCopy->Axes.Port == Point0);
	CHECK(oldCopy->Axes.InstanceBase == "base");
	CHECK(lateShared->OwnerId == "base");
	CHECK(lateShared->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(lateShared->Axes.OwnerId == "base");
	CHECK(lateShared->Axes.Port == Point0);
	CHECK(lateShared->Axes.InstanceBase == "base");
	CHECK(nestedCold->OwnerId == "base");
	CHECK(nestedCold->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(nestedCold->Axes.OwnerId == "nested");
	CHECK(nestedCold->Axes.Port == Point0);
	CHECK(nestedCold->Axes.InstanceBase == "copy");

	GroupReplayState unchanged;
	REQUIRE(BindGroupReplay(coldDocument, bindings, aliasesInitial, 3, unchanged, error) == Status::Ok);
	const auto retainedBytes = unchanged.RetainedBytes();
	const auto oldRevision = unchanged.AuthoringRevision();
	CHECK(
		BindGroupReplay(coldDocument, bindings, aliasesInitial, 3, unchanged, error, 1) ==
		Status::LimitExceeded
	);
	CHECK(unchanged.RetainedBytes() == retainedBytes);
	CHECK(unchanged.AuthoringRevision() == oldRevision);
	REQUIRE(unchanged.Binding("copy", Point0));
	CHECK(unchanged.Binding("copy", Point0)->Axes == oldCopy->Axes);
	CHECK(unchanged.Binding("late", Point0)->Axes == lateShared->Axes);
	CHECK(unchanged.Binding("nested", Point0)->Axes == nestedCold->Axes);
}

TEST_CASE("Nested Vec2 captures only its immediate base axis state", "[source_input_moves][mirror_axes]") {
	const auto makeNested = [] {
		auto document = MakeGradientPointMoveGraph();
		auto copyAxes = *document.Nodes[0].SourceSeparatedVec2Animators;
		for (auto &input : copyAxes.Inputs)
			for (auto &axis : input.Axes)
				for (auto &key : axis.Keys) {
					key.NodeId = "copy";
					key.SourceKeyId = "copy-" + key.SourceKeyId;
				}
		document.Nodes[1].SourceSeparatedVec2Animators.emplace() = std::move(copyAxes);
		Node nested = document.Nodes[1];
		nested.Id = "nested";
		nested.InstanceBase = "copy";
		for (auto &input : nested.SourceSeparatedVec2Animators->Inputs)
			for (auto &axis : input.Axes)
				for (auto &key : axis.Keys) {
					key.NodeId = "nested";
					key.SourceKeyId = "nested-" + key.SourceKeyId;
				}
		document.Nodes.push_back(std::move(nested));
		return document;
	};
	Diagnostic error;
	auto warmDocument = makeNested();
	GroupReplayState empty, warmInitial, warm;
	REQUIRE(RebindGroupReplay(warmDocument, empty, 1, warmInitial, error) == Status::Ok);
	const std::array nestedBinding{GradientPointBinding("nested")};
	REQUIRE(BindGroupReplay(warmDocument, nestedBinding, warmInitial, 1, warm, error) == Status::Ok);
	const auto *warmNested = warm.Binding("nested", Point0);
	REQUIRE(warmNested);
	CHECK(warmNested->OwnerId == "base");
	CHECK(warmNested->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(warmNested->Axes.OwnerId == "copy");
	CHECK(warmNested->Axes.Port == Point0);
	CHECK(warmNested->Axes.InstanceBase == "copy");

	auto coldDocument = makeNested();
	coldDocument.Nodes[1].SourceSeparatedVec2Animators = {};
	GroupReplayState coldInitial, cold;
	REQUIRE(RebindGroupReplay(coldDocument, empty, 1, coldInitial, error) == Status::Ok);
	REQUIRE(BindGroupReplay(coldDocument, nestedBinding, coldInitial, 1, cold, error) == Status::Ok);
	const auto *coldNested = cold.Binding("nested", Point0);
	REQUIRE(coldNested);
	CHECK(coldNested->OwnerId == "base");
	CHECK(coldNested->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(coldNested->Axes.OwnerId == "nested");
	CHECK(coldNested->Axes.Port == Point0);
	CHECK(coldNested->Axes.InstanceBase == "copy");
}

TEST_CASE(
	"Nested Vec2 reads and edits its captured immediate-base axes", "[source_input_moves][mirror_axes]"
) {
	auto document = MakeGradientPointMoveGraph();
	auto copyAxes = *document.Nodes[0].SourceSeparatedVec2Animators;
	for (size_t axisIndex = 0; axisIndex < copyAxes.Inputs.front().Axes.size(); ++axisIndex) {
		auto &axis = copyAxes.Inputs.front().Axes[axisIndex];
		for (size_t keyIndex = 0; keyIndex < axis.Keys.size(); ++keyIndex) {
			auto &key = axis.Keys[keyIndex];
			key.NodeId = "copy";
			key.SourceKeyId = "copy-" + std::to_string(axisIndex) + "-" + std::to_string(keyIndex);
			key.Data = axisIndex == 0 ? Value{keyIndex == 0 ? 100.0 : 200.0} : Value{20.0};
		}
	}
	document.Nodes[1].SourceSeparatedVec2Animators.emplace() = std::move(copyAxes);
	Node nested = document.Nodes[1];
	nested.Id = "nested";
	nested.InstanceBase = "copy";
	nested.InstanceOverrides = {std::string(Point0)};
	nested.SourceSeparatedVec2Animators->Inputs.front().Separated = true;
	for (size_t axisIndex = 0; axisIndex < nested.SourceSeparatedVec2Animators->Inputs.front().Axes.size();
		 ++axisIndex) {
		auto &axis = nested.SourceSeparatedVec2Animators->Inputs.front().Axes[axisIndex];
		for (size_t keyIndex = 0; keyIndex < axis.Keys.size(); ++keyIndex) {
			auto &key = axis.Keys[keyIndex];
			key.NodeId = "nested";
			key.SourceKeyId = "nested-" + std::to_string(axisIndex) + "-" + std::to_string(keyIndex);
		}
	}
	document.Nodes.push_back(std::move(nested));
	const std::array bindings{GradientPointBinding("nested")};
	Diagnostic error;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, error) == Status::Ok);
	REQUIRE(BindGroupReplay(document, bindings, initial, 1, bound, error) == Status::Ok);
	const auto *binding = bound.Binding("nested", Point0);
	REQUIRE(binding);
	CHECK(binding->OwnerId == "base");
	CHECK(binding->Axes.Storage == GroupAxisStorage::Shared);
	CHECK(binding->Axes.OwnerId == "copy");
	CHECK(binding->Axes.Port == Point0);
	CHECK(SamplePoint(document, bound, "nested", Point0, 5) == Value{Vector2{150, 20}});
	CHECK(SamplePoint(document, bound, "base", Point0, 5) == Value{Vector2{15, 2}});

	auto inheritedDocument = document;
	inheritedDocument.Nodes[1].InstanceOverrides = {std::string(Point0)};
	inheritedDocument.Nodes[2].InstanceOverrides.clear();
	GroupReplayState inheritedInitial, inheritedBound;
	REQUIRE(RebindGroupReplay(inheritedDocument, empty, 1, inheritedInitial, error) == Status::Ok);
	REQUIRE(
		BindGroupReplay(inheritedDocument, bindings, inheritedInitial, 1, inheritedBound, error) == Status::Ok
	);
	CHECK(SamplePoint(inheritedDocument, inheritedBound, "nested", Point0, 5) == Value{Vector2{150, 20}});

	Document nestedStatic;
	GroupReplayState nestedStaticReplay;
	const auto nestedMode = ToggleSourceInputMode(
		document, bound, 1, {"nested", Point0, false, {5}}, nestedStatic, nestedStaticReplay, error
	);
	INFO(error.Message);
	REQUIRE(nestedMode == Status::Ok);
	const auto *nestedStaticBinding = nestedStaticReplay.Binding("nested", Point0);
	REQUIRE(nestedStaticBinding);
	CHECK(nestedStaticBinding->OwnerId == "base");
	CHECK(nestedStaticBinding->Writer == GroupSubtypeAnimator::Animated);
	CHECK(nestedStaticBinding->Axes.OwnerId == "copy");
	CHECK(nestedStaticBinding->Axes.Writer == GroupSubtypeAnimator::Animated);
	REQUIRE(nestedStatic.Nodes[1].SourceSeparatedVec2Animators);
	const auto &staticInputs = nestedStatic.Nodes[1].SourceSeparatedVec2Animators->Inputs;
	REQUIRE(staticInputs.size() == 1);
	const auto &staticAxes = staticInputs.front();
	CHECK(staticAxes.Port == Point0);
	for (size_t axis = 0; axis < 2; ++axis) {
		const auto &keys = staticAxes.Axes[axis].Keys;
		REQUIRE(keys.size() == 1);
		CHECK(keys.front().Tick == 0);
		CHECK(keys.front().Data == Value{axis == 0 ? 150.0 : 20.0});
	}
	CHECK(SamplePoint(nestedStatic, nestedStaticReplay, "nested", Point0, 5) == Value{Vector2{150, 20}});
	CHECK(SamplePoint(document, bound, "base", Point0, 5) == Value{Vector2{15, 2}});

	Document baseStatic;
	GroupReplayState baseStaticReplay;
	const auto baseMode = ToggleSourceInputMode(
		nestedStatic, nestedStaticReplay, 1, {"base", Point0, false, {5}}, baseStatic, baseStaticReplay, error
	);
	INFO(error.Message);
	REQUIRE(baseMode == Status::Ok);
	const auto *baseModeNestedBinding = baseStaticReplay.Binding("nested", Point0);
	REQUIRE(baseModeNestedBinding);
	CHECK(baseModeNestedBinding->OwnerId == "base");
	CHECK(baseModeNestedBinding->Writer == GroupSubtypeAnimator::Static);
	CHECK(baseModeNestedBinding->Axes.OwnerId == "copy");
	CHECK(baseModeNestedBinding->Axes.Writer == GroupSubtypeAnimator::Animated);
	CHECK(SamplePoint(baseStatic, baseStaticReplay, "nested", Point0, 5) == Value{Vector2{150, 20}});
	CHECK(SamplePoint(document, bound, "base", Point0, 5) == Value{Vector2{15, 2}});

	const Value editedValue = Vector2{300, 40};
	GroupRefreshEvent edit;
	edit.NodeId = "nested";
	edit.Reason = GroupRefreshReason::Edit;
	edit.EditedPort = Point0;
	edit.LocalValue = &editedValue;
	edit.LocalAnimated = true;
	edit.At.Tick = 5;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, error) == Status::Ok);
	const auto *copyOverlay = edited.SharedSubtype("copy", Point0);
	REQUIRE(copyOverlay);
	REQUIRE(copyOverlay->SeparatedVec2);
	CHECK(
		std::any_of(
			copyOverlay->SeparatedVec2->Axes[0].Keys.begin(),
			copyOverlay->SeparatedVec2->Axes[0].Keys.end(),
			[&](const auto &key) { return key.Tick == 5 && key.Data == Value{300.0}; }
		)
	);
	CHECK(
		std::any_of(
			copyOverlay->SeparatedVec2->Axes[1].Keys.begin(),
			copyOverlay->SeparatedVec2->Axes[1].Keys.end(),
			[&](const auto &key) { return key.Tick == 5 && key.Data == Value{40.0}; }
		)
	);
	CHECK(edited.SharedSubtype("base", Point0) == nullptr);
	CHECK(SamplePoint(document, edited, "nested", Point0, 5) == editedValue);
	CHECK(SamplePoint(document, edited, "base", Point0, 5) == Value{Vector2{15, 2}});
	Document projected;
	REQUIRE(ProjectGroupReplay(document, edited, 1, projected, error) == Status::Ok);
	GroupReplayState projectedReplay;
	REQUIRE(RebindProjectedGroupReplay(projected, edited, 1, projectedReplay, error) == Status::Ok);
	CHECK(SamplePoint(projected, projectedReplay, "nested", Point0, 5) == editedValue);
	CHECK(SamplePoint(projected, projectedReplay, "base", Point0, 5) == Value{Vector2{15, 2}});
}

TEST_CASE(
	"Cold nested Vec2 binding refuses later ancestor axes without changing prior output",
	"[source_input_moves][mirror_axes]"
) {
	auto warmDocument = MakeGradientPointMoveGraph();
	auto coldDocument = MakeGradientPointMoveGraph();
	coldDocument.Nodes.front().SourceSeparatedVec2Animators = {};
	coldDocument.Nodes[1].InstanceOverrides = {std::string(Point0)};
	const std::array bindings{GradientPointBinding("copy")};
	Diagnostic error;
	GroupReplayState empty, initial, cold;
	REQUIRE(RebindGroupReplay(coldDocument, empty, 1, initial, error) == Status::Ok);
	REQUIRE(BindGroupReplay(coldDocument, bindings, initial, 1, cold, error) == Status::Ok);
	const auto *binding = cold.Binding("copy", Point0);
	REQUIRE(binding);
	CHECK(binding->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(binding->Axes.OwnerId == "copy");

	coldDocument.Nodes.front().SourceSeparatedVec2Animators.emplace() =
		*warmDocument.Nodes.front().SourceSeparatedVec2Animators;
	EvaluationRequest request;
	request.Tick = 5;
	request.GroupReplay = &cold;
	request.GroupAuthoringRevision = cold.AuthoringRevision();
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	const auto documentBytes = DocumentRetainedPayloadBytes(coldDocument);
	REQUIRE(documentBytes);
	auto documentCharge = budget.Reserve(*documentBytes);
	REQUIRE(documentCharge);
	Value result = Vector2{77, 66};
	auto prior = budget.Reserve(sizeof(Vector2));
	REQUIRE(prior);
	detail::AllocationReservation charge = std::move(*prior);
	const auto retained = budget.Used();
	CHECK(
		detail::EvaluateSourceInput(coldDocument, "copy", Point0, request, budget, result, charge, error) ==
		Status::UnsupportedExecution
	);
	CHECK(result == Value{Vector2{77, 66}});
	CHECK(charge.Bytes() == sizeof(Vector2));
	CHECK(budget.Used() == retained);
}

TEST_CASE(
	"Fresh same-transaction aliases capture their immediate source axis state",
	"[source_input_moves][mirror_axes]"
) {
	const bool reverseBindings = GENERATE(false, true);
	const auto axesFor = [](Node &node, double first) {
		node.SourceSeparatedVec2Animators.emplace().Inputs.clear();
		SourceSeparatedVec2Animator axes;
		axes.Port = Point0;
		axes.Axes[0].Keys = {{node.Id, std::string(Point0), 0, first, "source", KeyframeEase{}}};
		axes.Axes[1].Keys = {{node.Id, std::string(Point0), 0, 2.0, "source", KeyframeEase{}}};
		for (size_t axis = 0; axis < axes.Axes.size(); ++axis)
			for (auto &key : axes.Axes[axis].Keys)
				key.SourceKeyId = node.Id + "-" + std::to_string(axis);
		node.SourceSeparatedVec2Animators->Inputs.push_back(std::move(axes));
	};
	const auto bindAliases = [&](bool baseWarm,
								 bool copyWarm,
								 GroupAxisStorage expectedStorage,
								 std::string_view copyAxisOwner,
								 std::string_view nestedAxisOwner) {
		auto document = MakeGradientPointMoveGraph();
		if (!baseWarm) document.Nodes[0].SourceSeparatedVec2Animators = {};
		if (copyWarm)
			axesFor(document.Nodes[1], 100.0);
		else
			document.Nodes[1].SourceSeparatedVec2Animators = {};
		Node nested = document.Nodes[1];
		nested.Id = "nested";
		nested.InstanceBase = "copy";
		if (nested.SourceSeparatedVec2Animators)
			for (auto &input : nested.SourceSeparatedVec2Animators->Inputs)
				for (auto &axis : input.Axes)
					for (auto &key : axis.Keys) {
						key.NodeId = "nested";
						key.SourceKeyId = "nested-" + key.SourceKeyId;
					}
		document.Nodes.push_back(std::move(nested));
		std::vector<GroupSubtypeBinding> bindings{
			GradientPointBinding("copy"), GradientPointBinding("nested")
		};
		if (reverseBindings) std::reverse(bindings.begin(), bindings.end());
		Diagnostic diagnostic;
		GroupReplayState empty, initial, replay;
		REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
		REQUIRE(BindGroupReplay(document, bindings, initial, 1, replay, diagnostic) == Status::Ok);
		const auto *copy = replay.Binding("copy", Point0);
		const auto *nestedBinding = replay.Binding("nested", Point0);
		REQUIRE(copy);
		REQUIRE(nestedBinding);
		CHECK(copy->OwnerId == "base");
		CHECK(nestedBinding->OwnerId == "base");
		CHECK(copy->Axes.Storage == expectedStorage);
		CHECK(nestedBinding->Axes.Storage == expectedStorage);
		CHECK(copy->Axes.OwnerId == copyAxisOwner);
		CHECK(nestedBinding->Axes.OwnerId == nestedAxisOwner);
		CHECK(copy->Axes.Port == Point0);
		CHECK(nestedBinding->Axes.Port == Point0);
		CHECK(copy->Axes.InstanceBase == "base");
		CHECK(nestedBinding->Axes.InstanceBase == "copy");
	};

	bindAliases(true, false, GroupAxisStorage::Shared, "base", "base");
	bindAliases(true, true, GroupAxisStorage::Shared, "base", "base");
	bindAliases(false, true, GroupAxisStorage::Uninitialized, "copy", "nested");
}

TEST_CASE(
	"Deleted original inputs retain one live animator for their surviving aliases", "[source_input_moves]"
) {
	auto original = MakeMoveGraph();
	original.Nodes[1].InstanceOverrides = {"argument_value_1"};
	auto bound = BindMoveGraph(original);
	Diagnostic error;
	const bool existingEffect = GENERATE(false, true);
	GroupReplayState prior;
	Value before = 44.;
	GroupRefreshEvent event;
	event.NodeId = "copy";
	event.Reason = GroupRefreshReason::Edit;
	event.EditedPort = "argument_value_1";
	event.LocalValue = &before;
	event.LocalAnimated = true;
	event.At.Tick = 1;
	if (existingEffect)
		REQUIRE(ReplayGroupAnimatorEdits(original, {&event, 1}, bound, 1, prior, error) == Status::Ok);
	const auto &source = existingEffect ? prior : bound;
	auto staged = original;
	StageMove(staged, "base");
	SourceInputMove moves[6];
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), moves);
	for (auto &move : moves)
		move.NodeId = "base";
	GroupReplayState moved;
	auto status = RebindGroupReplayWithInputMoves(original, staged, moves, source, 2, moved, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(moved.DetachedAnimators().size() == 1);
	const auto identity = moved.DetachedAnimators()[0].Id;
	REQUIRE(moved.SharedSubtype("base", identity));
	CHECK(moved.Binding("copy", "argument_value_1")->AnimatorPort == identity);
	CHECK(moved.Binding("copy", "argument_value_2")->AnimatorPort == "argument_value_1");
	CHECK(moved.SharedSubtype("base", "argument_value_2") == nullptr);
	CHECK(CaptureMove(staged, moved, "copy", "argument_value_1") == Value{existingEffect ? 44. : 20.});
	CHECK(CaptureMove(staged, moved, "sibling", "argument_value_1") == Value{30.});
	Value editedValue = 99.;
	event.LocalValue = &editedValue;
	event.At.Tick = 2;
	GroupReplayState edited;
	status = ReplayGroupAnimatorEdits(staged, {&event, 1}, moved, 2, edited, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	CHECK(CaptureMove(staged, edited, "copy", "argument_value_1") == editedValue);
	CHECK(CaptureMove(staged, edited, "base", "argument_value_1") == Value{30.});
	CHECK(CaptureMove(staged, edited, "sibling", "argument_value_1") == Value{30.});
	CHECK(CaptureMove(original, source, "copy", "argument_value_1") == Value{existingEffect ? 44. : 20.});
	// The next writer retains the surviving original animator, separate from the retired one.
	editedValue = 77.;
	event.EditedPort = "argument_value_2";
	event.At.Tick = 3;
	GroupReplayState second;
	REQUIRE(ReplayGroupAnimatorEdits(staged, {&event, 1}, edited, 2, second, error) == Status::Ok);
	CHECK(CaptureMove(staged, second, "copy", "argument_value_1", 3) == Value{99.});
	CHECK(CaptureMove(staged, second, "base", "argument_value_1", 3) == editedValue);
	Document projected;
	status = ProjectGroupReplay(staged, second, 2, projected, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	GroupReplayState retained;
	status = RebindProjectedGroupReplay(projected, second, 2, retained, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(retained.DetachedAnimators().size() == 1);
	CHECK(CaptureMove(projected, retained, "copy", "argument_value_1", 3) == Value{99.});
	CHECK(CaptureMove(projected, retained, "sibling", "argument_value_1", 3) == Value{77.});
	const auto saved = Write(projected);
	REQUIRE(!saved.empty());
	Document loaded;
	REQUIRE(Read(saved, loaded, error) == Status::Ok);
	GroupReplayState empty, fresh, reloaded;
	REQUIRE(RebindGroupReplay(loaded, empty, 3, fresh, error) == Status::Ok);
	std::vector<GroupSubtypeBinding> bindings;
	for (const auto *id : {"copy", "sibling"})
		for (size_t n = 0; n < 2; ++n)
			bindings.push_back(
				{id,
				 "base",
				 GroupSubtypeAnimator::Animated,
				 GroupSubtypeAnimator::Animated,
				 "argument_value_" + std::to_string(n)}
			);
	REQUIRE(BindGroupReplay(loaded, bindings, fresh, 3, reloaded, error) == Status::Ok);
	CHECK(reloaded.DetachedAnimators().empty());
	CHECK(CaptureMove(loaded, reloaded, "copy", "argument_value_1", 3) == Value{77.});
	// Retiring all remaining aliases releases the detached animator, without touching the earlier undo state.
	auto retired = staged;
	StageMove(retired, "copy");
	StageMove(retired, "sibling");
	SourceInputMove retireMoves[12];
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), retireMoves);
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), retireMoves + 6);
	for (size_t n = 6; n < 12; ++n)
		retireMoves[n].NodeId = "sibling";
	GroupReplayState released;
	REQUIRE(
		RebindGroupReplayWithInputMoves(staged, retired, retireMoves, second, 4, released, error) ==
		Status::Ok
	);
	CHECK(released.DetachedAnimators().empty());
	CHECK(released.SharedSubtype("base", identity) == nullptr);
	CHECK(CaptureMove(staged, second, "copy", "argument_value_1", 3) == Value{99.});
}

TEST_CASE(
	"Retained source modes publish physical flags and the single animator atomically", "[source_input_moves]"
) {
	auto original = MakeMoveGraph();
	original.Nodes[1].InstanceOverrides = {"argument_value_1"};
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "base");
	SourceInputMove moves[6];
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), moves);
	for (auto &move : moves)
		move.NodeId = "base";
	GroupReplayState moved;
	Diagnostic error;
	REQUIRE(RebindGroupReplayWithInputMoves(original, staged, moves, bound, 2, moved, error) == Status::Ok);
	const auto identity = moved.DetachedAnimators()[0].Id;
	Value value = 66.;
	GroupRefreshEvent edit;
	edit.NodeId = "copy";
	edit.Reason = GroupRefreshReason::Edit;
	edit.EditedPort = "argument_value_1";
	edit.LocalValue = &value;
	edit.LocalAnimated = true;
	edit.At.Tick = 5;
	GroupReplayState keyed;
	REQUIRE(ReplayGroupAnimatorEdits(staged, {&edit, 1}, moved, 2, keyed, error) == Status::Ok);
	Document disabled;
	GroupReplayState disabledReplay;
	auto code = ToggleSourceInputMode(
		staged, keyed, 2, {"copy", "argument_value_1", false, {5}}, disabled, disabledReplay, error
	);
	INFO(error.Message);
	REQUIRE(code == Status::Ok);
	CHECK(disabledReplay.DetachedAnimators()[0].Writer == GroupSubtypeAnimator::Animated);
	REQUIRE(disabledReplay.SharedSubtype("base", identity));
	REQUIRE(disabledReplay.SharedSubtype("base", identity)->Keys.size() == 1);
	CHECK(disabledReplay.SharedSubtype("base", identity)->Keys[0].Data == Value{66.});
	CHECK(disabledReplay.SharedSubtype("base", identity)->Keys[0].Tick == 0);
	CHECK(CaptureMove(disabled, disabledReplay, "copy", "argument_value_1", 9) == Value{66.});
	CHECK(CaptureMove(disabled, disabledReplay, "sibling", "argument_value_1", 9) == Value{30.});
	CHECK(CaptureMove(staged, keyed, "copy", "argument_value_1", 5) == Value{66.});
	CHECK(
		std::find(
			disabled.Nodes[1].SourceStaticInputs.begin(),
			disabled.Nodes[1].SourceStaticInputs.end(),
			"argument_value_1"
		) != disabled.Nodes[1].SourceStaticInputs.end()
	);
	Document enabled;
	GroupReplayState enabledReplay;
	code = ToggleSourceInputMode(
		disabled,
		disabledReplay,
		2,
		{"copy", "argument_value_1", true, {2, .25, true}},
		enabled,
		enabledReplay,
		error
	);
	INFO(error.Message);
	REQUIRE(code == Status::Ok);
	CHECK(GetFrameTime(enabledReplay.SharedSubtype("base", identity)->Keys[0]) == FrameTime{2, .25, true});
	CHECK(enabledReplay.DetachedAnimators()[0].Writer == GroupSubtypeAnimator::Animated);
	CHECK(CaptureMove(enabled, enabledReplay, "copy", "argument_value_1", 6) == Value{66.});
	Value next = 91.;
	edit.LocalValue = &next;
	edit.At.Tick = 6;
	GroupReplayState later;
	REQUIRE(ReplayGroupAnimatorEdits(enabled, {&edit, 1}, enabledReplay, 2, later, error) == Status::Ok);
	CHECK(CaptureMove(enabled, later, "copy", "argument_value_1", 6) == next);
	Document noOp;
	GroupReplayState noOpReplay;
	REQUIRE(
		ToggleSourceInputMode(
			enabled, later, 2, {"copy", "argument_value_1", true, {6}}, noOp, noOpReplay, error
		) == Status::Ok
	);
	CHECK(noOpReplay.SharedSubtype("base", identity)->Keys == later.SharedSubtype("base", identity)->Keys);
	CHECK(CaptureMove(noOp, noOpReplay, "copy", "argument_value_1", 6) == next);
	const auto unchanged = Write(noOp);
	const auto bytes = noOpReplay.RetainedBytes();
	REQUIRE(
		ToggleSourceInputMode(
			enabled, later, 2, {"copy", "argument_value_1", false, {6}}, noOp, noOpReplay, error, 1
		) == Status::LimitExceeded
	);
	CHECK(Write(noOp) == unchanged);
	CHECK(noOpReplay.RetainedBytes() == bytes);
	CHECK(CaptureMove(noOp, noOpReplay, "copy", "argument_value_1", 6) == next);
	REQUIRE(
		ToggleSourceInputMode(
			enabled, later, 2, {"copy", "argument_value_1", false, {6}}, enabled, enabledReplay, error
		) == Status::Ok
	);
	CHECK(CaptureMove(enabled, enabledReplay, "copy", "argument_value_1", 6) == next);
}

TEST_CASE(
	"Detached moves and paired modes admit old and separate new owners before publishing",
	"[source_input_moves]"
) {
	auto original = MakeMoveGraph();
	original.Nodes[1].InstanceOverrides = {"argument_value_1"};
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "base");
	SourceInputMove moves[6];
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), moves);
	for (auto &move : moves)
		move.NodeId = "base";
	Diagnostic error;
	const auto attemptMove = [&](uint64_t maximum) {
		auto destination = BindMoveGraph(original);
		const auto oldBytes = destination.RetainedBytes();
		const auto status =
			RebindGroupReplayWithInputMoves(original, staged, moves, bound, 2, destination, error, maximum);
		if (status != Status::Ok) {
			CHECK(status == Status::LimitExceeded);
			CHECK(destination.AuthoringRevision() == 1);
			CHECK(destination.RetainedBytes() == oldBytes);
			CHECK(destination.DetachedAnimators().empty());
		} else
			CHECK(CaptureMove(staged, destination, "copy", "argument_value_1") == Value{20.});
		return status;
	};
	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	REQUIRE(attemptMove(high) == Status::Ok);
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		if (attemptMove(middle) == Status::Ok)
			high = middle;
		else
			low = middle + 1;
	}
	REQUIRE(attemptMove(low) == Status::Ok);
	REQUIRE(attemptMove(low - 1) == Status::LimitExceeded);
	GroupReplayState moved;
	REQUIRE(RebindGroupReplayWithInputMoves(original, staged, moves, bound, 2, moved, error) == Status::Ok);
	const auto attemptMode = [&](uint64_t maximum) {
		auto destination = BindMoveGraph(original);
		auto documentResult = original;
		const auto saved = Write(documentResult);
		const auto priorBytes = destination.RetainedBytes();
		const auto status = ToggleSourceInputMode(
			staged,
			moved,
			2,
			{"copy", "argument_value_1", false, {2}},
			documentResult,
			destination,
			error,
			maximum
		);
		if (status != Status::Ok) {
			CHECK(status == Status::LimitExceeded);
			CHECK(Write(documentResult) == saved);
			CHECK(destination.RetainedBytes() == priorBytes);
			CHECK(destination.AuthoringRevision() == 1);
		} else
			CHECK(CaptureMove(documentResult, destination, "copy", "argument_value_1") == Value{20.});
		return status;
	};
	low = 1;
	high = Limits::MaximumEvaluationBytes;
	REQUIRE(attemptMode(high) == Status::Ok);
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		if (attemptMode(middle) == Status::Ok)
			high = middle;
		else
			low = middle + 1;
	}
	REQUIRE(attemptMode(low) == Status::Ok);
	REQUIRE(attemptMode(low - 1) == Status::LimitExceeded);
	CHECK(CaptureMove(original, bound, "copy", "argument_value_1") == Value{20.});
}

TEST_CASE(
	"Removed original writers preserve local links and current-index expression owners",
	"[source_input_moves]"
) {
	auto original = MakeMoveGraph();
	const bool overridden = GENERATE(false, true);
	original.Nodes[0].SourceInputExpressions = {{"argument_value_2", "value+1", true}};
	original.Nodes[1].SourceInputExpressions = {{"argument_value_1", "value+100", true}};
	if (overridden) original.Nodes[1].InstanceOverrides = {"argument_value_1"};
	auto bound = BindMoveGraph(original);
	auto staged = original;
	StageMove(staged, "base");
	SourceInputMove moves[6];
	std::copy(std::begin(CopyMoves), std::end(CopyMoves), moves);
	for (auto &move : moves)
		move.NodeId = "base";
	GroupReplayState moved;
	Diagnostic error;
	REQUIRE(RebindGroupReplayWithInputMoves(original, staged, moves, bound, 2, moved, error) == Status::Ok);
	CHECK(CaptureMove(staged, moved, "copy", "argument_value_1") == Value{overridden ? 120. : 31.});
	staged.Junctions = {{"literal", "", ValueType::Scalar, Value{123.}}};
	staged.Links = {{"literal", "value", "copy", "argument_value_1"}};
	CHECK(CaptureMove(staged, moved, "copy", "argument_value_1") == Value{223.});
	CHECK(CaptureMove(staged, moved, "sibling", "argument_value_1") == Value{31.});
}
