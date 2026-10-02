#include <engine/imagegraph/SourceModeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.source_mode_transition")
using namespace engine::imagegraph;
namespace {
	Document Source(bool animated = false) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"owner", "pc.invert", {}, {}, {{"mix", .25}}}, {"target", "pc.invert", {}, {}, {{"mix", .75}}}
		};
		for (auto &node : document.Nodes)
			(animated ? node.SourceAnimatedInputs : node.SourceStaticInputs) = {"mix"};
		document.Keyframes = {
			{"owner", "mix", 0, .25, "source", KeyframeEase{}},
			{"owner", "mix", 4, .75, "source", KeyframeEase{}}
		};
		document.Tracks = {{"owner", "mix", "hold", -1}};
		document.Outputs = {{"result", "target", "surface_out"}};
		return document;
	}
	Document GroupSource() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{1}},
			  {"subtype", EnumValue{0}},
			  {"vector_size", EnumValue{0}},
			  {"parent_value", .25}}},
			{"output", "pc.group_output", "group", {}, {}}
		};
		document.Nodes.front().SourceAnimatedInputs = {"parent_value"};
		Group group{"group", "Group"};
		group.Ports = {
			{"input", "input/parent", PortDirection::Input, "input"},
			{"output", "output/parent", PortDirection::Output, "output"}
		};
		document.Groups.push_back(std::move(group));
		document.Junctions = {
			{"input/parent", "group", ValueType::Any, .25},
			{"output/parent", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {
			{"input", "value", "output", "value"}, {"output", "value", "output/parent", "value"}
		};
		document.Outputs = {{"result", "output", "value"}};
		document.Keyframes = {
			{"input", "parent_value", 0, .25, "source", KeyframeEase{}},
			{"input", "parent_value", 4, .75, "source", KeyframeEase{}}
		};
		document.Tracks = {{"input", "parent_value", "ping", -1}};
		return document;
	}
	EvaluationRequest BindingClock(const GroupReplayState *state = nullptr, uint64_t revision = 1) {
		EvaluationRequest clock;
		clock.Tick = 1;
		clock.NegativeFrame = true;
		clock.Subframe = .5;
		clock.GroupReplay = state;
		clock.GroupAuthoringRevision = revision;
		return clock;
	}
	void Bound(Document &document, GroupReplayState &replay, bool aliased = false) {
		GroupReplayState empty, local;
		Diagnostic diagnostic;
		if (aliased) {
			document.Nodes[1].InstanceBase = "owner";
			document.Nodes[1].InstanceOverrides = {"mix"};
		}
		const auto mode = [](const Node &node) {
			return node.SourceAnimatedInputs.empty() ? GroupSubtypeAnimator::Static
													 : GroupSubtypeAnimator::Animated;
		};
		const GroupSubtypeBinding binding{
			"target", "owner", mode(document.Nodes[1]), mode(document.Nodes[0]), "mix"
		};
		const auto localStatus = RebindGroupReplay(document, empty, 1, local, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(localStatus == Status::Ok);
		const auto diagnosticStatus101 = BindGroupReplay(
			document,
			aliased ? std::span{&binding, 1} : std::span<const GroupSubtypeBinding>{},
			local,
			1,
			replay,
			diagnostic
		);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(diagnosticStatus101 == Status::Ok);
	}
	void Saved(const Document &document) {
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		const auto status = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
}
TEST_CASE(
	"Enabling source animation retimes the owner's first key and changes only the selected mode",
	"[imagegraph][source_mode]"
) {
	auto document = Source();
	document.Keyframes.front().Kind = KeyframeKind::Adder;
	document.Keyframes.front().SourceDriver = KeyframeLinearDriver{.125};
	document.Keyframes.front().Ease = KeyframeEase{"cut", "bezier", {.2, .8}, {.3, .1}};
	GroupReplayState replay;
	Bound(document, replay, true);
	auto expected = document.Keyframes.front();
	const FrameTime time{1, .375, true};
	REQUIRE(SetFrameTime(expected, time));
	Document changed;
	Diagnostic diagnostic;
	REQUIRE(
		ToggleSourceInputMode(document, replay, 1, {"target", "mix", true, time}, changed, diagnostic) ==
		Status::Ok
	);
	CHECK(changed.Nodes[0].SourceStaticInputs == document.Nodes[0].SourceStaticInputs);
	CHECK(changed.Nodes[1].SourceAnimatedInputs == std::vector<std::string>{"mix"});
	CHECK(changed.Nodes[1].SourceStaticInputs.empty());
	CHECK(changed.Keyframes.front() == expected);
	CHECK(changed.Keyframes.back() == document.Keyframes.back());
	CHECK(changed.Tracks == document.Tracks);
	CHECK(document.Nodes[1].SourceAnimatedInputs.empty());
	Saved(changed);
}
TEST_CASE("Disabling an owner samples after its own mode flag changes", "[imagegraph][source_mode]") {
	auto document = Source(true);
	GroupReplayState replay;
	Bound(document, replay);
	Document changed;
	Diagnostic diagnostic;
	REQUIRE(
		ToggleSourceInputMode(
			document, replay, 1, {"owner", "mix", false, {2, .5, false}}, changed, diagnostic
		) == Status::Ok
	);
	REQUIRE(changed.Keyframes.size() == 1);
	CHECK(changed.Keyframes.front().Data == Value{.25});
	CHECK(GetFrameTime(changed.Keyframes.front()) == FrameTime{});
	CHECK(changed.Keyframes.front().Ease == KeyframeEase{});
	CHECK_FALSE(changed.Keyframes.front().SourceDriver);
	CHECK(changed.Tracks == document.Tracks);
	Saved(changed);
}
TEST_CASE("Disabling an alias samples its still-animated original owner", "[imagegraph][source_mode]") {
	auto document = Source(true);
	GroupReplayState replay;
	Bound(document, replay, true);
	Document changed;
	Diagnostic diagnostic;
	REQUIRE(
		ToggleSourceInputMode(
			document, replay, 1, {"target", "mix", false, {2, .5, false}}, changed, diagnostic
		) == Status::Ok
	);
	REQUIRE(changed.Keyframes.size() == 1);
	CHECK(changed.Keyframes.front().Data == Value{.5625});
	CHECK(changed.Nodes[0].SourceAnimatedInputs == document.Nodes[0].SourceAnimatedInputs);
	CHECK(changed.Nodes[1].SourceAnimatedInputs.empty());
	CHECK(changed.Nodes[1].SourceStaticInputs == std::vector<std::string>{"mix"});
	CHECK(changed.Tracks == document.Tracks);
	Saved(changed);
}
TEST_CASE(
	"Disabling an alias preserves source lone-driver sampling under a static owner",
	"[imagegraph][source_mode]"
) {
	auto document = Source();
	document.Nodes[1].SourceStaticInputs.clear();
	document.Nodes[1].SourceAnimatedInputs = {"mix"};
	document.Keyframes.resize(1);
	document.Keyframes.front().SourceDriver = KeyframeLinearDriver{.125};
	GroupReplayState replay;
	Bound(document, replay, true);
	Document changed;
	Diagnostic diagnostic;
	REQUIRE(
		ToggleSourceInputMode(
			document, replay, 1, {"target", "mix", false, {2, .5, false}}, changed, diagnostic
		) == Status::Ok
	);
	REQUIRE(changed.Keyframes.size() == 1);
	CHECK(changed.Keyframes.front().Data == Value{.5625});
	CHECK(changed.Nodes[0].SourceStaticInputs == document.Nodes[0].SourceStaticInputs);
	Saved(changed);
}
TEST_CASE(
	"An empty source animator gains one signed-clock key with fresh defaults", "[imagegraph][source_mode]"
) {
	auto document = Source();
	document.Keyframes.clear();
	GroupReplayState replay;
	Bound(document, replay);
	Document changed;
	Diagnostic diagnostic;
	const FrameTime time{3, .25, true};
	REQUIRE(
		ToggleSourceInputMode(document, replay, 1, {"owner", "mix", true, time}, changed, diagnostic) ==
		Status::Ok
	);
	REQUIRE(changed.Keyframes.size() == 1);
	CHECK(GetFrameTime(changed.Keyframes.front()) == time);
	CHECK(changed.Keyframes.front().Data == Value{0.0});
	CHECK(changed.Keyframes.front().Ease == KeyframeEase{});
	CHECK(changed.Tracks == document.Tracks);
	Saved(changed);
}
TEST_CASE(
	"Source mode transaction failures preserve prior output and borrowed state", "[imagegraph][source_mode]"
) {
	auto document = Source();
	GroupReplayState replay;
	Bound(document, replay);
	Document output = Source(true);
	const auto before = output;
	const auto original = document;
	Diagnostic diagnostic;
	CHECK(
		ToggleSourceInputMode(document, replay, 1, {"owner", "mix", true, {}}, output, diagnostic, 0) ==
		Status::LimitExceeded
	);
	CHECK(output == before);
	CHECK(
		ToggleSourceInputMode(document, replay, 1, {"owner", "mix", true, {}}, output, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(output == before);
	CHECK(
		ToggleSourceInputMode(
			document, replay, 1, {"owner", "mix", true, {4, 0, false}}, output, diagnostic
		) == Status::UnsupportedExecution
	);
	CHECK(output == before);
	CHECK(document == original);
	CHECK(replay.AuthoringRevision() == 1);
}
TEST_CASE(
	"Group parent mode remains local and retains its independent track settings", "[imagegraph][source_mode]"
) {
	auto document = GroupSource();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const GroupBootstrapTarget order[] = {{"input"}};
	GroupReplayState empty, loaded;
	REQUIRE(
		RestoreGroupDeclarations(document, plan, order, BindingClock(), empty, 1, loaded, diagnostic) ==
		Status::Ok
	);
	Document changed;
	REQUIRE(
		ToggleSourceInputMode(
			document, loaded, 1, {"input", "parent_value", false, {2, .5, false}}, changed, diagnostic
		) == Status::Ok
	);
	REQUIRE(changed.Keyframes.size() == 1);
	CHECK(changed.Keyframes.front().NodeId == "input");
	CHECK(changed.Keyframes.front().Port == "parent_value");
	CHECK(changed.Keyframes.front().Data == Value{.25});
	CHECK(changed.Tracks == document.Tracks);
	Saved(changed);
}
TEST_CASE(
	"Source mode transition supports a single-key quaternion retime without sampling",
	"[imagegraph][source_mode]"
) {
	auto document = Source();
	document.Nodes.front().Type = "pc.quarternion_to_euler";
	document.Nodes.front().Values = {{"rotation", Quaternion{0, 0, 0, 1}}};
	document.Nodes.front().SourceStaticInputs = {"rotation"};
	document.Keyframes = {{"owner", "rotation", 0, Quaternion{0, 0, 0, 1}, "source", KeyframeEase{}}};
	document.Tracks = {{"owner", "rotation", "hold", -1}};
	document.Tracks.front().QuaternionMode = 0;
	GroupReplayState replay;
	Bound(document, replay);
	Document changed;
	Diagnostic diagnostic;
	const auto status = ToggleSourceInputMode(
		document, replay, 1, {"owner", "rotation", true, {1, .25, true}}, changed, diagnostic
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK((GetFrameTime(changed.Keyframes.front()) == FrameTime{1, .25, true}));
	CHECK(changed.Keyframes.front().Data == document.Keyframes.front().Data);
	CHECK(changed.Tracks == document.Tracks);
	Saved(changed);
}

TEST_CASE(
	"Source Trigger mode transitions preserve map pulses and reject unproved clocks",
	"[imagegraph][source_mode]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"owner", "pc.trigger", "", {}, {{"trigger", false}}}};
	document.Nodes[0].SourceStaticInputs = {"trigger"};
	document.Outputs = {{"result", "owner", "trigger"}};
	GroupReplayState empty, replay;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(document, empty, 1, replay, diagnostic) == Status::Ok);
	Document output;
	REQUIRE(
		ToggleSourceInputMode(
			document, replay, 1, {"owner", "trigger", true, {5, 0, false}}, output, diagnostic
		) == Status::Ok
	);
	REQUIRE(output.Keyframes.size() == 1);
	CHECK(output.Keyframes[0].Tick == 5);
	CHECK(output.Keyframes[0].Data == Value{false});
	Plan plan;
	REQUIRE(Compile(output, plan, diagnostic) == Status::Ok);
	for (const uint64_t tick : {0, 4, 5, 6}) {
		EvaluatedValue pulse;
		REQUIRE(EvaluateValue(output, plan, "result", {.Tick = tick}, pulse, diagnostic) == Status::Ok);
		CHECK(std::get<bool>(pulse.Data) == (tick == 5));
	}
	Saved(output);
	GroupReplayState animatedReplay;
	REQUIRE(RebindGroupReplay(output, empty, 2, animatedReplay, diagnostic) == Status::Ok);
	Document disabled;
	REQUIRE(
		ToggleSourceInputMode(
			output, animatedReplay, 2, {"owner", "trigger", false, {7, 0, false}}, disabled, diagnostic
		) == Status::Ok
	);
	REQUIRE(disabled.Keyframes.size() == 1);
	CHECK(disabled.Keyframes[0].Tick == 0);
	CHECK(disabled.Keyframes[0].Data == Value{false});
	REQUIRE(Compile(disabled, plan, diagnostic) == Status::Ok);
	EvaluatedValue staticPulse;
	REQUIRE(EvaluateValue(disabled, plan, "result", {}, staticPulse, diagnostic) == Status::Ok);
	CHECK_FALSE(std::get<bool>(staticPulse.Data));
	const auto before = output;
	CHECK(
		ToggleSourceInputMode(
			document, replay, 1, {"owner", "trigger", true, {5, .5, false}}, output, diagnostic
		) == Status::UnsupportedExecution
	);
	CHECK(output == before);
}
TEST_CASE(
	"Source mode transaction accepts in-place output and creates missing default track",
	"[imagegraph][source_mode]"
) {
	auto document = Source();
	document.Keyframes.resize(1);
	document.Tracks.clear();
	GroupReplayState replay;
	Bound(document, replay);
	Diagnostic diagnostic;
	REQUIRE(
		ToggleSourceInputMode(
			document, replay, 1, {"owner", "mix", true, {1, .25, true}}, document, diagnostic
		) == Status::Ok
	);
	REQUIRE(document.Keyframes.size() == 1);
	REQUIRE(document.Tracks.size() == 1);
	CHECK(document.Tracks.front().End == "hold");
	CHECK(document.Tracks.front().LoopRange == -1);
	Saved(document);
}
TEST_CASE(
	"Source mode admission includes long owner names and prior output overlap", "[imagegraph][source_mode]"
) {
	auto document = Source();
	const std::string name(27, 'o');
	document.Nodes.front().Id = std::string(name);
	for (auto &key : document.Keyframes)
		key.NodeId = std::string(name);
	document.Tracks.front().NodeId = std::string(name);
	GroupReplayState replay;
	Bound(document, replay);
	auto output = Source(true);
	const auto before = output;
	const auto sourceBytes = DocumentRetainedPayloadBytes(document);
	const auto priorBytes = DocumentRetainedPayloadBytes(output);
	REQUIRE(sourceBytes);
	REQUIRE(priorBytes);
	Diagnostic diagnostic;
	CHECK(
		ToggleSourceInputMode(
			document,
			replay,
			1,
			{name, "mix", true, {1, .25, true}},
			output,
			diagnostic,
			*sourceBytes + *priorBytes + replay.RetainedBytes() - 1
		) == Status::LimitExceeded
	);
	CHECK(output == before);
	REQUIRE(
		ToggleSourceInputMode(document, replay, 1, {name, "mix", true, {1, .25, true}}, output, diagnostic) ==
		Status::Ok
	);
	CHECK(output.Keyframes.front().NodeId == name);
	Saved(output);
}
TEST_CASE("Source mode transition rejects ambiguous legacy fixed storage", "[imagegraph][source_mode]") {
	auto document = Source();
	document.Keyframes.clear();
	document.Tracks.clear();
	GroupReplayState replay;
	Bound(document, replay);
	auto output = Source(true);
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(
		ToggleSourceInputMode(document, replay, 1, {"owner", "mix", true, {}}, output, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.Message == "source static fixed storage needs a represented original animator key");
	CHECK(output == before);
}
TEST_CASE(
	"Source mode public count preflight runs before looking for the selected target",
	"[imagegraph][source_mode]"
) {
	auto document = Source();
	GroupReplayState replay;
	Bound(document, replay);
	auto output = Source(true);
	const auto before = output;
	document.Nodes.resize(Limits::MaximumNodes + 1);
	Diagnostic diagnostic;
	CHECK(
		ToggleSourceInputMode(document, replay, 1, {"missing", "mix", true, {}}, output, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(diagnostic.Message == "source mode public document counts exceed bounds");
	CHECK(output == before);
}
TEST_CASE(
	"Mode changes retain raw empty Group vector keys while resolving exact source getters",
	"[imagegraph][source_mode]"
) {
	auto document = GroupSource();
	document.Nodes.front().SourceStaticInputs = {"range", "gizmo_position"};
	document.Nodes.front().Values.push_back({"range", 0.0});
	document.Nodes.front().Values.push_back({"gizmo_position", 0.0});
	document.Tracks.push_back({"input", "range", "hold", -1});
	document.Tracks.push_back({"input", "gizmo_position", "hold", -1});
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const GroupBootstrapTarget order[] = {{"input"}};
	GroupReplayState empty, loaded;
	REQUIRE(
		RestoreGroupDeclarations(document, plan, order, BindingClock(), empty, 1, loaded, diagnostic) ==
		Status::Ok
	);
	Document changed;
	REQUIRE(
		ToggleSourceInputMode(
			document, loaded, 1, {"input", "range", true, {1, .375, true}}, changed, diagnostic
		) == Status::Ok
	);
	GroupReplayState rebound;
	REQUIRE(RebindGroupReplay(changed, loaded, 2, rebound, diagnostic) == Status::Ok);
	Document both;
	REQUIRE(
		ToggleSourceInputMode(
			changed, rebound, 2, {"input", "gizmo_position", true, {1, .375, true}}, both, diagnostic
		) == Status::Ok
	);
	const auto rawEmpty = [](const Value &value) {
		const auto *array = std::get_if<ArrayValue>(&value);
		return array && array->ElementType == ValueType::Scalar && array->Elements.empty() &&
			   array->Nested.empty() && array->Items.empty();
	};
	for (std::string_view port : {"range", "gizmo_position"}) {
		const auto value = std::find_if(
			both.Nodes.front().Values.begin(), both.Nodes.front().Values.end(), [&](const auto &v) {
				return v.Port == port;
			}
		);
		REQUIRE(value != both.Nodes.front().Values.end());
		CHECK(rawEmpty(value->Data));
		const auto key = std::find_if(both.Keyframes.begin(), both.Keyframes.end(), [&](const auto &k) {
			return k.NodeId == "input" && k.Port == port;
		});
		REQUIRE(key != both.Keyframes.end());
		CHECK(rawEmpty(key->Data));
	}
	Saved(both);
	Document restored;
	REQUIRE(Read(Write(both), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	auto clock = BindingClock();
	clock.Tick = 13;
	REQUIRE(EvaluateNodeInputs(restored, plan, "input", clock, snapshot, diagnostic) == Status::Ok);
	const auto checkGetters = [&](const auto &values) {
		const auto range =
			std::find_if(values.begin(), values.end(), [](const auto &v) { return v.Port == "range"; });
		const auto gizmo = std::find_if(values.begin(), values.end(), [](const auto &v) {
			return v.Port == "gizmo_position";
		});
		REQUIRE(range != values.end());
		CHECK((range->Data == Value{Vector2{0, 0}}));
		REQUIRE(gizmo != values.end());
		CHECK(rawEmpty(gizmo->Data));
	};
	checkGetters(snapshot.Values());
	std::vector<AuthoredValue> authored;
	REQUIRE(ResolveNodeValues(restored, plan, "result", "input", clock, authored, diagnostic) == Status::Ok);
	checkGetters(authored);
	GroupReplayState restoredReplay;
	REQUIRE(RebindGroupReplay(restored, rebound, 3, restoredReplay, diagnostic) == Status::Ok);
	Document disabledRange;
	REQUIRE(
		ToggleSourceInputMode(
			restored, restoredReplay, 3, {"input", "range", false, {3, .25, false}}, disabledRange, diagnostic
		) == Status::Ok
	);
	GroupReplayState staticReplay;
	REQUIRE(RebindGroupReplay(disabledRange, restoredReplay, 4, staticReplay, diagnostic) == Status::Ok);
	Document staticBoth;
	REQUIRE(
		ToggleSourceInputMode(
			disabledRange,
			staticReplay,
			4,
			{"input", "gizmo_position", false, {3, .25, false}},
			staticBoth,
			diagnostic
		) == Status::Ok
	);
	Saved(staticBoth);
	REQUIRE(Compile(staticBoth, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateNodeInputs(staticBoth, plan, "input", clock, snapshot, diagnostic) == Status::Ok);
	checkGetters(snapshot.Values());
	REQUIRE(
		ResolveNodeValues(staticBoth, plan, "result", "input", clock, authored, diagnostic) == Status::Ok
	);
	checkGetters(authored);
	auto invalid = both;
	invalid.Nodes.front().SourceAnimatedInputs.clear();
	CHECK(Compile(invalid, plan, diagnostic) == Status::TypeMismatch);
	invalid = both;
	const auto key = std::find_if(invalid.Keyframes.begin(), invalid.Keyframes.end(), [](const auto &k) {
		return k.NodeId == "input" && k.Port == "range";
	});
	REQUIRE(key != invalid.Keyframes.end());
	std::get<ArrayValue>(key->Data).Items.push_back(SourceArrayItem{ElementValue{0.0}});
	CHECK(Compile(invalid, plan, diagnostic) == Status::TypeMismatch);
}

TEST_CASE(
	"Disabling an alias captures animated owner Euler tuples before property conversion",
	"[imagegraph][source_mode]"
) {
	for (bool driven : {false, true}) {
		auto document = Source(true);
		for (auto &node : document.Nodes) {
			node.Type = "pc.quarternion_to_euler";
			node.Values = {{"rotation", Quaternion{10, 20, 30, 1}}};
			node.SourceAnimatedInputs = {"rotation"};
		}
		document.Nodes[1].InstanceBase = "owner";
		document.Nodes[1].InstanceOverrides = {"rotation"};
		document.Keyframes = {
			{"owner", "rotation", 0, Quaternion{10, 20, 30, 1}, "source", KeyframeEase{}},
			{"owner", "rotation", 4, Quaternion{50, 60, 70, 5}, "source", KeyframeEase{}}
		};
		if (driven) document.Keyframes[0].SourceDriver = KeyframeLinearDriver{2};
		document.Tracks = {{"owner", "rotation", "hold", -1}};
		document.Tracks[0].QuaternionMode = 1;
		document.Outputs = {{"result", "target", "euler_angles"}, {"owner-result", "owner", "euler_angles"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		const GroupSubtypeBinding binding{
			"target", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "rotation"
		};
		GroupReplayState replay, empty, local;
		const auto localStatus = RebindGroupReplay(document, empty, 1, local, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(localStatus == Status::Ok);
		const auto bindingStatus1 =
			BindGroupReplay(document, std::span{&binding, 1}, local, 1, replay, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(bindingStatus1 == Status::Ok);
		Document changed;
		REQUIRE(
			ToggleSourceInputMode(
				document, replay, 1, {"target", "rotation", false, {2}}, changed, diagnostic
			) == Status::Ok
		);
		const double offset = driven ? 4 : 0;
		const Quaternion raw{30 + offset, 40 + offset, 50 + offset, 3 + offset};
		REQUIRE(changed.Keyframes.size() == 1);
		CHECK(changed.Keyframes[0].Data == Value{raw});
		CHECK(GetFrameTime(changed.Keyframes[0]) == FrameTime{});
		CHECK_FALSE(changed.Keyframes[0].SourceDriver);
		CHECK(changed.Nodes[0].SourceAnimatedInputs == std::vector<std::string>{"rotation"});
		CHECK(changed.Nodes[1].SourceStaticInputs == std::vector<std::string>{"rotation"});
		CHECK(changed.Tracks == document.Tracks);
		Saved(changed);
		REQUIRE(Compile(changed, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		REQUIRE(SetFrameTime(request, {7}));
		std::vector<AuthoredValue> values;
		const auto resolved =
			ResolveNodeValues(changed, plan, "owner-result", "owner", request, values, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(resolved == Status::Ok);
		const auto rotation = std::find_if(values.begin(), values.end(), [](const AuthoredValue &value) {
			return value.Port == "rotation";
		});
		REQUIRE(rotation != values.end());
		Quaternion processed;
		REQUIRE(ConvertSourceQuaternion(raw, 1, processed));
		CHECK(rotation->Data == Value{processed});
	}
}

TEST_CASE("Source global modes require declared controls and disjoint names", "[imagegraph][source_mode]") {
	Document document;
	document.FormatVersion = 9;
	Node globals{"globals", "pc.global_scope", "", {}, {}};
	globals.DynamicInputs = {{"speed", ValueType::Scalar, Value{3.0}}};
	globals.SourceStaticInputs = {"speed"};
	Node consumer{"consumer", "pc.equation", "", {}, {{"equation", std::string{"speed"}}}};
	document.Nodes = {globals, consumer};
	document.ProjectGlobalNodeId = "globals";
	document.Outputs = {{"value", "consumer", "result"}};
	Diagnostic diagnostic;
	Plan plan;

	SECTION("declared static and animated controls survive migration and compilation") {
		REQUIRE(Migrate(document, diagnostic) == Status::Ok);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		document.Nodes.front().SourceStaticInputs.clear();
		document.Nodes.front().SourceAnimatedInputs = {"speed"};
		REQUIRE(Migrate(document, diagnostic) == Status::Ok);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Saved(document);
	}
	SECTION("an undeclared control is rejected") {
		document.Nodes.front().SourceStaticInputs = {"missing"};
		CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
		CHECK(Migrate(document, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.Port == "missing");
	}
	SECTION("one control cannot be both static and animated") {
		document.Nodes.front().SourceAnimatedInputs = {"speed"};
		CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
		CHECK(Migrate(document, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.Port == "speed");
	}
	SECTION("one mode cannot repeat a control") {
		document.Nodes.front().SourceStaticInputs.push_back("speed");
		CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
		CHECK(Migrate(document, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.Port == "speed");
	}
}
