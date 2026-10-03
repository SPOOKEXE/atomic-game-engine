#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
