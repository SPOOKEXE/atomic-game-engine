#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceAxisTransition.hpp>
#include <engine/imagegraph/SourceInputProcessingObserver.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>

TEST_SUITE_ID("engine.imagegraph.source_axis_transition")
using namespace engine::imagegraph;

namespace {
	Keyframe CombinedKey(uint64_t tick, Vector2 value) {
		Keyframe key{"mirror", "center", tick, value, "source", KeyframeEase{}};
		key.SourceKeyId = "combined-" + std::to_string(tick);
		key.Kind = KeyframeKind::Adder;
		key.SourceDriver = KeyframeLinearDriver{2};
		key.Ease->In = {.2, .3};
		key.Ease->Out = {.7, .8};
		return key;
	}

	SourceSeparatedVec2Animator Axes(bool separated = true) {
		SourceSeparatedVec2Animator animator;
		animator.Port = "center";
		animator.Separated = separated;
		animator.Axes[0].Keys = {
			{"mirror", "center", 0, 0.0, "source", KeyframeEase{}},
			{"mirror", "center", 10, 1.0, "source", KeyframeEase{}}
		};
		animator.Axes[1].Keys = {
			{"mirror", "center", 0, 2.0, "source", KeyframeEase{}},
			{"mirror", "center", 5, 4.0, "source", KeyframeEase{}},
			{"mirror", "center", 10, 6.0, "source", KeyframeEase{}}
		};
		return animator;
	}

	Document MirrorDocument(bool separated = false) {
		Document document;
		document.FormatVersion = 9;
		Node mirror{"mirror", "pc.mirror_polar", "", {}, {{"center", Vector2{.9, .9}}}};
		mirror.SourceAnimatedInputs = {"center"};
		mirror.SourceSeparatedVec2Animators.emplace().Inputs.push_back(Axes(separated));
		document.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			std::move(mirror)
		};
		document.Links = {{"source", "image", "mirror", "surface_in"}};
		document.Outputs = {{"out", "mirror", "surface_out"}};
		document.Keyframes = {CombinedKey(0, {.1, .2}), CombinedKey(10, {.9, .8})};
		document.Tracks = {{"mirror", "center", "hold"}};
		document.Timeline = TimelineSettings{};
		document.Timeline->Frames = 11;
		return document;
	}

	GroupReplayState BoundReplay(
		const Document &document, std::span<const GroupSubtypeBinding> bindings, Diagnostic &diagnostic
	) {
		GroupReplayState empty, declarations, bound;
		REQUIRE(RebindGroupReplay(document, empty, 1, declarations, diagnostic) == Status::Ok);
		REQUIRE(BindGroupReplay(document, bindings, declarations, 1, bound, diagnostic) == Status::Ok);
		return bound;
	}

	EvaluationRequest Request(const GroupReplayState &replay, uint64_t tick = 0) {
		EvaluationRequest request;
		request.Tick = tick;
		request.GroupReplay = &replay;
		request.GroupAuthoringRevision = replay.AuthoringRevision();
		return request;
	}
}

TEST_CASE(
	"Source axis transition splits combined local keys into raw component rows",
	"[source_axis_transition][mirror_axes]"
) {
	auto document = MirrorDocument();
	Diagnostic diagnostic;
	const std::array<GroupSubtypeBinding, 0> noBindings{};
	auto replay = BoundReplay(document, noBindings, diagnostic);
	const auto original = document;
	const SourceAxisTransition transition{"mirror", "center", true, true};
	Document result;
	GroupReplayState replayResult;
	const auto status =
		ToggleSourceAxes(document, replay, 1, transition, Request(replay), result, replayResult, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	CHECK(document == original);
	CHECK(result.Keyframes == original.Keyframes);
	REQUIRE(result.Nodes[1].SourceSeparatedVec2Animators);
	const auto &axes = result.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	CHECK(axes.Separated);
	CHECK(axes.Initialized);
	for (size_t axis = 0; axis < 2; ++axis) {
		const auto &keys = axes.Axes[axis].Keys;
		REQUIRE(keys.size() == 2);
		for (size_t index = 0; index < keys.size(); ++index) {
			const auto &source = original.Keyframes[index];
			const auto &key = keys[index];
			CHECK(key.Tick == source.Tick);
			CHECK(key.Subframe == source.Subframe);
			CHECK(key.NegativeFrame == source.NegativeFrame);
			CHECK(key.Ease == source.Ease);
			CHECK(key.Data == Value{axis == 0 ? (index == 0 ? .1 : .9) : (index == 0 ? .2 : .8)});
			CHECK_FALSE(key.SourceDriver);
			CHECK_FALSE(key.SineDriver);
			CHECK(key.Kind == KeyframeKind::Normal);
			CHECK(key.SourceKeyId.empty());
		}
	}
}

TEST_CASE(
	"Source axis transition combines sampled values at the sorted union of component times",
	"[source_axis_transition][mirror_axes]"
) {
	auto document = MirrorDocument(true);
	document.Keyframes.clear();
	document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front() = Axes();
	Diagnostic diagnostic;
	const std::array<GroupSubtypeBinding, 0> noBindings{};
	auto replay = BoundReplay(document, noBindings, diagnostic);
	const SourceAxisTransition transition{"mirror", "center", false, true};
	Document result;
	GroupReplayState replayResult;
	const auto status =
		ToggleSourceAxes(document, replay, 1, transition, Request(replay), result, replayResult, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Keyframes.size() == 3);
	const std::array<uint64_t, 3> ticks{0, 5, 10};
	const std::array<Vector2, 3> values{Vector2{0, 2}, Vector2{.5, 4}, Vector2{1, 6}};
	for (size_t index = 0; index < ticks.size(); ++index) {
		const auto &key = result.Keyframes[index];
		CHECK(key.Tick == ticks[index]);
		CHECK(key.Data == Value{values[index]});
		CHECK(key.Interpolation == "source");
		CHECK(key.Ease == KeyframeEase{});
		CHECK_FALSE(key.SourceDriver);
		CHECK_FALSE(key.SineDriver);
		CHECK(key.Kind == KeyframeKind::Normal);
		CHECK(key.SourceKeyId.empty());
	}
	const auto &retained = result.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	CHECK_FALSE(retained.Separated);
	CHECK(retained.Axes == document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes);
}

TEST_CASE(
	"Flag-only axis transition leaves cold storage untouched and refusals preserve both outputs",
	"[source_axis_transition][mirror_axes]"
) {
	auto document = MirrorDocument();
	auto &mirror = document.Nodes[1];
	mirror.SourceVec2Defaults.emplace().Inputs.push_back({"center", Vector2{7, 8}});
	auto &cold = mirror.SourceSeparatedVec2Animators->Inputs.front();
	cold.Initialized = false;
	cold.Axes = {};
	Node copy = mirror;
	copy.Id = "copy";
	copy.InstanceBase = "mirror";
	copy.InstanceOverrides = {"center"};
	const bool originalBaseFlag = cold.Separated;
	document.Nodes.push_back(std::move(copy));
	const GroupSubtypeBinding binding{
		"copy", "mirror", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
	};
	Diagnostic diagnostic;
	const std::array bindings{binding};
	auto replay = BoundReplay(document, bindings, diagnostic);
	REQUIRE(replay.Binding("copy", "center"));
	const auto transition = SourceAxisTransition{"copy", "center", true, false};
	Document result;
	GroupReplayState replayResult;
	const auto status =
		ToggleSourceAxes(document, replay, 1, transition, Request(replay), result, replayResult, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	const auto copyNode = std::find_if(result.Nodes.begin(), result.Nodes.end(), [](const auto &node) {
		return node.Id == "copy";
	});
	REQUIRE(copyNode != result.Nodes.end());
	REQUIRE(copyNode->SourceSeparatedVec2Animators);
	const auto &copyAxes = copyNode->SourceSeparatedVec2Animators->Inputs.front();
	CHECK(copyAxes.Separated);
	CHECK_FALSE(copyAxes.Initialized);
	CHECK(copyAxes.Axes[0].Keys.empty());
	CHECK(copyAxes.Axes[1].Keys.empty());
	CHECK(result.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Separated == originalBaseFlag);
	CHECK(copyNode->Values.front().Data == Value{Vector2{.9, .9}});
	CHECK(copyNode->SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});
	const auto *bound = replayResult.Binding("copy", "center");
	REQUIRE(bound);
	CHECK(bound->OwnerId == "mirror");
	CHECK(bound->Axes.Storage == GroupAxisStorage::Uninitialized);
	Document sameResult;
	GroupReplayState sameReplay;
	const auto sameStatus = ToggleSourceAxes(
		result, replayResult, 1, transition, Request(replayResult), sameResult, sameReplay, diagnostic
	);
	REQUIRE(sameStatus == Status::Ok);
	CHECK(sameResult == result);
	CHECK(sameReplay.RetainedBytes() == replayResult.RetainedBytes());
	REQUIRE(sameReplay.Binding("copy", "center"));
	REQUIRE(replayResult.Binding("copy", "center"));
	const auto &sameBinding = *sameReplay.Binding("copy", "center");
	const auto &retainedBinding = *replayResult.Binding("copy", "center");
	CHECK(sameBinding.NodeId == retainedBinding.NodeId);
	CHECK(sameBinding.OwnerId == retainedBinding.OwnerId);
	CHECK(sameBinding.Getter == retainedBinding.Getter);
	CHECK(sameBinding.Writer == retainedBinding.Writer);
	CHECK(sameBinding.Port == retainedBinding.Port);
	CHECK(sameBinding.AnimatorPort == retainedBinding.AnimatorPort);
	CHECK(sameBinding.Axes == retainedBinding.Axes);

	Document priorResult = result;
	GroupReplayState priorReplay = BoundReplay(result, bindings, diagnostic);
	const auto priorBytes = priorReplay.RetainedBytes();
	CHECK(
		ToggleSourceAxes(
			document, replay, 2, transition, Request(replay), priorResult, priorReplay, diagnostic
		) == Status::InvalidValue
	);
	CHECK(priorResult == result);
	CHECK(priorReplay.AuthoringRevision() == 1);
	CHECK(priorReplay.RetainedBytes() == priorBytes);
	Document refused = result;
	GroupReplayState refusedReplay = BoundReplay(result, bindings, diagnostic);
	const auto refusedBytes = refusedReplay.RetainedBytes();
	CHECK(
		ToggleSourceAxes(
			document, replay, 1, transition, Request(replay), refused, refusedReplay, diagnostic, 1
		) == Status::LimitExceeded
	);
	CHECK(refused == result);
	CHECK(refusedReplay.RetainedBytes() == refusedBytes);
}

TEST_CASE(
	"Axis separation edits the captured physical array and only the selected local flag",
	"[source_axis_transition]"
) {
	auto document = MirrorDocument(false);
	Node copy = document.Nodes[1];
	copy.Id = "copy";
	copy.InstanceBase = "mirror";
	copy.InstanceOverrides = {"center"};
	copy.SourceSeparatedVec2Animators->Inputs.front().Axes = {};
	copy.SourceSeparatedVec2Animators->Inputs.front().Initialized = false;
	document.Nodes.push_back(std::move(copy));
	Diagnostic diagnostic;
	const std::array bindings{GroupSubtypeBinding{
		"copy", "mirror", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
	}};
	auto replay = BoundReplay(document, bindings, diagnostic);
	REQUIRE(replay.Binding("copy", "center")->Axes.Storage == GroupAxisStorage::Shared);
	Document result;
	GroupReplayState changed;
	const auto status = ToggleSourceAxes(
		document, replay, 1, {"copy", "center", true}, Request(replay), result, changed, diagnostic
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &base = result.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	const auto &local = result.Nodes[2].SourceSeparatedVec2Animators->Inputs.front();
	CHECK_FALSE(base.Separated);
	CHECK(base.Axes[0].Keys[0].Data == Value{.1});
	CHECK(base.Axes[1].Keys[1].Data == Value{.8});
	CHECK(local.Separated);
	CHECK_FALSE(local.Initialized);
	CHECK(local.Axes[0].Keys.empty());
	CHECK(changed.Binding("copy", "center")->Axes.OwnerId == "mirror");
	CHECK(result.Keyframes == document.Keyframes);
}

TEST_CASE(
	"Cold axis conversion retains local constructors independently from the combined alias",
	"[source_axis_transition]"
) {
	auto document = MirrorDocument(false);
	auto &baseAxes = document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	baseAxes.Initialized = false;
	baseAxes.Axes = {};
	Node copy = document.Nodes[1];
	copy.Id = "copy";
	copy.InstanceBase = "mirror";
	copy.InstanceOverrides = {"center"};
	copy.SourceVec2Defaults.emplace().Inputs.push_back({"center", Vector2{7, 8}});
	document.Nodes.push_back(std::move(copy));
	Diagnostic diagnostic;
	const std::array bindings{GroupSubtypeBinding{
		"copy", "mirror", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
	}};
	auto replay = BoundReplay(document, bindings, diagnostic);
	Document result;
	GroupReplayState changed;
	const auto status = ToggleSourceAxes(
		document, replay, 1, {"copy", "center", true}, Request(replay), result, changed, diagnostic
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(changed.Binding("copy", "center")->OwnerId == "mirror");
	CHECK(changed.Binding("copy", "center")->Axes.OwnerId == "copy");
	CHECK(changed.Binding("copy", "center")->Axes.Storage == GroupAxisStorage::Local);
	CHECK_FALSE(result.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Initialized);
	const auto &local = result.Nodes[2].SourceSeparatedVec2Animators->Inputs.front();
	CHECK(local.Initialized);
	CHECK(local.Separated);
	CHECK(local.Axes[0].Keys[0].Data == Value{.1});
	CHECK(local.Axes[0].Keys[0].NodeId == "copy");
	CHECK(result.Keyframes == document.Keyframes);
}

TEST_CASE(
	"Axis combine follows a retained expression at signed fractional key times", "[source_axis_transition]"
) {
	auto document = MirrorDocument(true);
	auto &axes = document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	REQUIRE(SetFrameTime(axes.Axes[0].Keys.front(), {2, .25, true}));
	REQUIRE(SetFrameTime(axes.Axes[0].Keys.back(), {3, .5, false}));
	axes.Axes[1].Keys.resize(1);
	REQUIRE(SetFrameTime(axes.Axes[1].Keys.front(), {1, .75, true}));
	document.Nodes[1].SourceInputExpressions = {{"center", "self.center", true}};
	Diagnostic diagnostic;
	auto replay = BoundReplay(document, {}, diagnostic);
	const std::array observed{AuthoredValue{"center", Vector2{11, 22}}};
	SourceAxisTransition transition{"mirror", "center", false};
	transition.ObservedInputs = observed;
	Document result;
	GroupReplayState changed;
	const auto status =
		ToggleSourceAxes(document, replay, 1, transition, Request(replay), result, changed, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Keyframes.size() == 3);
	CHECK(GetFrameTime(result.Keyframes[0]) == FrameTime{2, .25, true});
	CHECK(GetFrameTime(result.Keyframes[1]) == FrameTime{1, .75, true});
	CHECK(GetFrameTime(result.Keyframes[2]) == FrameTime{3, .5, false});
	for (const auto &key : result.Keyframes)
		CHECK(key.Data == Value{Vector2{11, 22}});
}

TEST_CASE("Axis sample refusal leaves both aliased live owners unchanged", "[source_axis_transition]") {
	auto document = MirrorDocument(true);
	document.Nodes[1].SourceInputExpressions = {{"center", "self.missing", true}};
	Diagnostic diagnostic;
	auto replay = BoundReplay(document, {}, diagnostic);
	const auto before = document;
	const auto beforeBytes = replay.RetainedBytes();
	const auto status = ToggleSourceAxes(
		document, replay, 1, {"mirror", "center", false}, Request(replay), document, replay, diagnostic
	);
	CHECK(status != Status::Ok);
	CHECK(document == before);
	CHECK(replay.RetainedBytes() == beforeBytes);
	CHECK(replay.AuthoringRevision() == 1);
}

TEST_CASE(
	"Axis combine retains numeric source tuples and refuses malformed getter shapes",
	"[source_axis_transition]"
) {
	auto document = MirrorDocument(true);
	const char *expression = "value + self.center";
	bool malformed = false;
	bool nonfinite = false;
	SECTION("Numeric tuple") {}
	SECTION("Extra component") {
		expression = "[1, 2, 3]";
		malformed = true;
	}
	SECTION("Nested component") {
		expression = "[[1, 2], 3]";
		malformed = true;
	}
	SECTION("Nonfinite component") {
		expression = "[10 ** 1000, 2]";
		malformed = nonfinite = true;
	}
	document.Nodes[1].SourceInputExpressions = {{"center", expression, true}};
	Diagnostic diagnostic;
	auto replay = BoundReplay(document, {}, diagnostic);
	const auto before = document;
	const auto replayBytes = replay.RetainedBytes();
	const std::array observed{AuthoredValue{"center", Vector2{11, 22}}};
	SourceAxisTransition transition{"mirror", "center", false};
	transition.ObservedInputs = observed;
	Document result = document;
	GroupReplayState changed;
	const auto status =
		ToggleSourceAxes(document, replay, 1, transition, Request(replay), result, changed, diagnostic);
	INFO(diagnostic.Message);
	if (malformed) {
		if (nonfinite)
			CHECK(status != Status::Ok);
		else
			CHECK(status == Status::TypeMismatch);
		CHECK(result == before);
		CHECK(replay.RetainedBytes() == replayBytes);
	} else {
		REQUIRE(status == Status::Ok);
		REQUIRE(result.Keyframes.size() == 3);
		const std::array expected{Vector2{11, 24}, Vector2{11.5, 26}, Vector2{12, 28}};
		for (size_t index = 0; index < result.Keyframes.size(); ++index) {
			const auto &key = result.Keyframes[index];
			const auto &tuple = std::get<ArrayValue>(key.Data);
			REQUIRE(tuple.Items.size() == 2);
			CHECK(std::get<double>(std::get<ElementValue>(tuple.Items[0].Data)) == expected[index].X);
			CHECK(std::get<double>(std::get<ElementValue>(tuple.Items[1].Data)) == expected[index].Y);
		}
		Document restored;
		REQUIRE(Read(Write(result), restored, diagnostic) == Status::Ok);
		CHECK(restored == result);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = 2;
		request.Subframe = .5;
		struct CenterObserver : SourceInputProcessingObserver {
			std::optional<Vector2> Center;
			std::string_view NodeId() const noexcept override {
				return "mirror";
			}
			uint64_t RetainedBytes() const noexcept override {
				return 0;
			}
			Status Observe(
				FrameTime,
				std::span<const EvaluationInputValue> values,
				std::span<const EvaluationInputImage>,
				std::span<const SnapshotImageArray>,
				Diagnostic &,
				uint64_t
			) override {
				for (const auto &input : values)
					if (input.Port == "center") Center = std::get<Vector2>(input.Data);
				return Status::Ok;
			}
		} observer;
		request.SourceInputObserver = &observer;
		Image output;
		REQUIRE(Evaluate(restored, plan, "out", request, output, diagnostic) == Status::Ok);
		REQUIRE(observer.Center);
		CHECK(*observer.Center == Vector2{22.5, 50});
		CHECK(output.Width == 1);
		CHECK(output.Height == 1);
		CHECK(restored == result);
		Document mixed = restored;
		mixed.Keyframes[1].Data = expected[1];
		REQUIRE(Compile(mixed, plan, diagnostic) == Status::Ok);
		const auto firstMixed = Evaluate(mixed, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(firstMixed == Status::Ok);
		CHECK(*observer.Center == Vector2{22.5, 50});
		request.Tick = 7;
		const auto secondMixed = Evaluate(mixed, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(secondMixed == Status::Ok);
		CHECK(*observer.Center == Vector2{23.5, 54});
		Document separated;
		GroupReplayState separatedReplay;
		REQUIRE(
			ToggleSourceAxes(
				restored,
				changed,
				1,
				{"mirror", "center", true},
				Request(changed),
				separated,
				separatedReplay,
				diagnostic
			) == Status::Ok
		);
		const auto &axes = separated.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
		REQUIRE(axes.Separated);
		REQUIRE(axes.Axes[0].Keys.size() == expected.size());
		REQUIRE(axes.Axes[1].Keys.size() == expected.size());
		for (size_t index = 0; index < expected.size(); ++index) {
			CHECK(axes.Axes[0].Keys[index].Data == Value{expected[index].X});
			CHECK(axes.Axes[1].Keys[index].Data == Value{expected[index].Y});
		}
		uint64_t refusedBytes = 0, admittedBytes = Limits::MaximumEvaluationBytes;
		while (admittedBytes - refusedBytes > 1) {
			const uint64_t boundary = refusedBytes + (admittedBytes - refusedBytes) / 2;
			Document candidate = document;
			GroupReplayState candidateReplay;
			const auto bounded = ToggleSourceAxes(
				document,
				replay,
				1,
				transition,
				Request(replay),
				candidate,
				candidateReplay,
				diagnostic,
				boundary
			);
			if (bounded == Status::Ok) {
				admittedBytes = boundary;
			} else {
				REQUIRE(bounded == Status::LimitExceeded);
				CHECK(candidate == before);
				CHECK(candidateReplay.RetainedBytes() == 0);
				refusedBytes = boundary;
			}
		}
		Document atBoundary = document;
		GroupReplayState boundaryReplay;
		REQUIRE(
			ToggleSourceAxes(
				document,
				replay,
				1,
				transition,
				Request(replay),
				atBoundary,
				boundaryReplay,
				diagnostic,
				admittedBytes
			) == Status::Ok
		);
		atBoundary = document;
		GroupReplayState refusedReplay;
		CHECK(
			ToggleSourceAxes(
				document,
				replay,
				1,
				transition,
				Request(replay),
				atBoundary,
				refusedReplay,
				diagnostic,
				admittedBytes - 1
			) == Status::LimitExceeded
		);
		CHECK(atBoundary == before);
		CHECK(refusedReplay.RetainedBytes() == 0);
		CHECK(replay.RetainedBytes() == replayBytes);
	}
}

TEST_CASE(
	"Fixed Vec2 axis transitions refuse invalid raw array keys atomically", "[source_axis_transition]"
) {
	auto document = MirrorDocument(false);
	document.Keyframes[0].Data = ArrayValue{ValueType::Scalar, {7.0, 8.0}};
	document.Keyframes[1].Data = ArrayValue{ValueType::Scalar, {9.0}};
	Diagnostic diagnostic;
	auto replay = BoundReplay(document, {}, diagnostic);
	Document result = document;
	const auto before = result;
	GroupReplayState changed;
	const auto beforeBytes = changed.RetainedBytes();
	const auto status = ToggleSourceAxes(
		document, replay, 1, {"mirror", "center", true}, Request(replay), result, changed, diagnostic
	);
	INFO(diagnostic.Message);
	CHECK(status == Status::TypeMismatch);
	CHECK(result == before);
	CHECK(changed.RetainedBytes() == beforeBytes);
}

TEST_CASE(
	"Separate retains an implicit static source row but preserves an explicitly empty animator",
	"[source_axis_transition]"
) {
	for (const bool configured : {false, true}) {
		auto document = MirrorDocument(false);
		document.Keyframes.clear();
		document.Nodes[1].SourceAnimatedInputs.clear();
		document.Nodes[1].SourceStaticInputs = {"center"};
		if (!configured) document.Tracks.clear();
		Diagnostic diagnostic;
		auto replay = BoundReplay(document, {}, diagnostic);
		Document result;
		GroupReplayState changed;
		const auto status = ToggleSourceAxes(
			document, replay, 1, {"mirror", "center", true}, Request(replay), result, changed, diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto &axes = result.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes;
		if (configured) {
			CHECK(axes[0].Keys.empty());
			CHECK(axes[1].Keys.empty());
		} else {
			REQUIRE(axes[0].Keys.size() == 1);
			REQUIRE(axes[1].Keys.size() == 1);
			CHECK(axes[0].Keys[0].Data == Value{.9});
			CHECK(axes[1].Keys[0].Data == Value{.9});
		}
		CHECK(result.Keyframes.empty());
	}
}
