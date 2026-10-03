#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_animator_replacement")
using namespace engine::imagegraph;
namespace {
	Document Graph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"base", "pc.invert", "", {}, {{"mix", .5}}},
			{"copy", "pc.invert", "", {}, {{"mix", .25}}},
			{"sibling", "pc.invert", "", {}, {{"mix", .1}}},
			{"local", "pc.invert", "", {}, {{"mix", .3}}}
		};
		doc.Nodes[1].InstanceBase = "base";
		doc.Nodes[2].InstanceBase = "base";
		for (auto &node : doc.Nodes)
			node.SourceAnimatedInputs = {"mix"};
		doc.Keyframes = {
			{"base", "mix", 0, .5, "source", KeyframeEase{}},
			{"local", "mix", 0, .3, "source", KeyframeEase{}}
		};
		doc.Tracks = {{"base", "mix", "hold", -1}, {"local", "mix", "hold", -1}};
		doc.Outputs = {{"out", "copy", "surface_out"}};
		return doc;
	}
	GroupReplayState Bound(const Document &doc) {
		Diagnostic error;
		GroupReplayState empty, local, bound;
		REQUIRE(RebindGroupReplay(doc, empty, 1, local, error) == Status::Ok);
		const GroupSubtypeBinding bindings[] = {
			{"copy", "base", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"},
			{"sibling", "base", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"}
		};
		REQUIRE(BindGroupReplay(doc, bindings, local, 1, bound, error) == Status::Ok);
		return bound;
	}
	GroupReplayState Edit(
		const Document &doc,
		const GroupReplayState &prior,
		uint64_t revision,
		std::string node,
		uint64_t tick,
		double data
	) {
		GroupRefreshEvent event;
		event.NodeId = std::move(node);
		event.Reason = GroupRefreshReason::Edit;
		event.EditedPort = "mix";
		Value value = data;
		event.LocalValue = &value;
		event.LocalAnimated = true;
		event.At.Tick = tick;
		GroupReplayState next;
		Diagnostic error;
		REQUIRE(ReplayGroupAnimatorEdits(doc, {&event, 1}, prior, revision, next, error) == Status::Ok);
		return next;
	}
	Document Stage(Document doc, std::string_view node, double value) {
		auto found = std::find_if(doc.Nodes.begin(), doc.Nodes.end(), [&](const auto &item) {
			return item.Id == node;
		});
		REQUIRE(found != doc.Nodes.end());
		found->Values[0].Data = value;
		std::erase_if(doc.Keyframes, [&](const auto &key) {
			return key.NodeId == node && key.Port == "mix";
		});
		doc.Keyframes.push_back({std::string(node), "mix", 0, value, "source", KeyframeEase{}});
		if (std::none_of(doc.Tracks.begin(), doc.Tracks.end(), [&](const auto &track) {
				return track.NodeId == node && track.Port == "mix";
			}))
			doc.Tracks.push_back({std::string(node), "mix", "hold", -1});
		return doc;
	}
	Value Input(const Document &doc, const GroupReplayState &state, std::string_view node, uint64_t tick) {
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(doc, plan, error) == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		request.GroupReplay = &state;
		request.GroupAuthoringRevision = state.AuthoringRevision();
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(doc, plan, node, request, snapshot, error) == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
				return v.Port == "mix";
			});
		REQUIRE(value != snapshot.Values().end());
		return value->Data;
	}
	Document Project(const Document &doc, const GroupReplayState &state) {
		Diagnostic error;
		Document result;
		REQUIRE(ProjectGroupReplay(doc, state, state.AuthoringRevision(), result, error) == Status::Ok);
		return result;
	}
	size_t Keys(const Document &doc, std::string_view node) {
		return std::count_if(doc.Keyframes.begin(), doc.Keyframes.end(), [&](const auto &key) {
			return key.NodeId == node && key.Port == "mix";
		});
	}
}
TEST_CASE(
	"Fresh physical animator replacement needs no previous overlay",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Stage(Graph(), "base", .2);
	auto bound = Bound(doc);
	GroupReplayState next;
	Diagnostic error;
	const SourceAnimatorReplacement targets[] = {{"base", "mix"}};
	REQUIRE(RebindGroupReplayWithAnimatorReplacements(doc, targets, bound, 2, next, error) == Status::Ok);
	CHECK(next.SharedSubtypes().empty());
	CHECK(next.Bindings().size() == 2);
	CHECK(next.AuthoringRevision() == 2);
	CHECK(Input(doc, next, "copy", 5) == Value{.2});
}
TEST_CASE(
	"Base physical replacement retires only its own overlay and preserves sibling bindings",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Graph();
	auto bound = Bound(doc);
	auto base = Edit(doc, bound, 1, "copy", 5, .75);
	auto previous = Edit(doc, base, 1, "local", 3, .6);
	const auto unchanged = previous.SharedSubtype("local", "mix")->Keys;
	auto staged = Stage(doc, "base", .2);
	GroupReplayState next;
	Diagnostic error;
	const SourceAnimatorReplacement targets[] = {{"base", "mix"}};
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(staged, targets, previous, 2, next, error) == Status::Ok
	);
	CHECK(next.SharedSubtype("base", "mix") == nullptr);
	REQUIRE(next.SharedSubtype("local", "mix"));
	CHECK(next.SharedSubtype("local", "mix")->Keys == unchanged);
	REQUIRE(next.Binding("copy", "mix"));
	REQUIRE(next.Binding("sibling", "mix"));
	CHECK(next.Binding("copy", "mix")->OwnerId == "base");
	CHECK(next.Binding("sibling", "mix")->OwnerId == "base");
	auto projected = Project(staged, next);
	CHECK(Keys(projected, "base") == 1);
	CHECK(Input(projected, next, "copy", 5) == Value{.2});
	CHECK(Input(projected, next, "sibling", 5) == Value{.2});
}
TEST_CASE(
	"Inherited physical replacement leaves the delegated getter and base overlay unchanged",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Graph();
	auto bound = Bound(doc);
	auto previous = Edit(doc, bound, 1, "copy", 5, .75);
	const auto unchanged = previous.SharedSubtype("base", "mix")->Keys;
	auto staged = Stage(doc, "copy", .2);
	GroupReplayState next;
	Diagnostic error;
	const SourceAnimatorReplacement targets[] = {{"copy", "mix"}};
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(staged, targets, previous, 2, next, error) == Status::Ok
	);
	REQUIRE(next.SharedSubtype("base", "mix"));
	CHECK(next.SharedSubtype("base", "mix")->Keys == unchanged);
	CHECK(staged.Nodes[1].InstanceOverrides.empty());
	auto projected = Project(staged, next);
	CHECK(Keys(projected, "copy") == 1);
	CHECK(Input(projected, next, "copy", 5) == Value{.75});
	CHECK(Input(projected, next, "sibling", 5) == Value{.75});
}
TEST_CASE(
	"Local physical owner replacement preserves unrelated base effects",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Graph();
	auto bound = Bound(doc);
	auto base = Edit(doc, bound, 1, "copy", 5, .75);
	auto previous = Edit(doc, base, 1, "local", 3, .6);
	const auto unchanged = previous.SharedSubtype("base", "mix")->Keys;
	auto staged = Stage(doc, "local", .2);
	GroupReplayState next;
	Diagnostic error;
	const SourceAnimatorReplacement targets[] = {{"local", "mix"}};
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(staged, targets, previous, 2, next, error) == Status::Ok
	);
	CHECK(next.SharedSubtype("local", "mix") == nullptr);
	REQUIRE(next.SharedSubtype("base", "mix"));
	CHECK(next.SharedSubtype("base", "mix")->Keys == unchanged);
	auto projected = Project(staged, next);
	CHECK(Keys(projected, "local") == 1);
	CHECK(Input(projected, next, "local", 5) == Value{.2});
}
TEST_CASE(
	"Later ordinary edits retain the original writer after physical replacement",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Graph();
	auto bound = Bound(doc);
	auto previous = Edit(doc, bound, 1, "copy", 5, .75);
	auto staged = Stage(doc, "copy", .2);
	GroupReplayState next;
	Diagnostic error;
	const SourceAnimatorReplacement targets[] = {{"copy", "mix"}};
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(staged, targets, previous, 2, next, error) == Status::Ok
	);
	auto projected = Project(staged, next);
	auto later = Edit(projected, next, 2, "copy", 7, .9);
	REQUIRE(later.SharedSubtype("base", "mix"));
	CHECK(later.SharedSubtype("base", "mix")->Keys.size() == 3);
	CHECK(later.SharedSubtype("copy", "mix") == nullptr);
	auto finalDoc = Project(projected, later);
	CHECK(Keys(finalDoc, "copy") == 1);
	CHECK(Input(finalDoc, later, "copy", 7) == Value{.9});
	CHECK(Input(finalDoc, later, "sibling", 7) == Value{.9});
}
TEST_CASE(
	"Duplicate unknown and malformed physical targets preserve replacement state",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Graph();
	auto bound = Bound(doc);
	auto previous = Edit(doc, bound, 1, "copy", 5, .75);
	auto staged = Stage(doc, "base", .2);
	auto destination = Edit(doc, previous, 1, "local", 3, .6);
	const auto bytes = destination.RetainedBytes();
	const auto keys = destination.SharedSubtype("base", "mix")->Keys;
	Diagnostic error;
	const SourceAnimatorReplacement duplicate[] = {{"base", "mix"}, {"base", "mix"}};
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, duplicate, previous, 2, destination, error) ==
		Status::DuplicateId
	);
	const SourceAnimatorReplacement unknown[] = {{"missing", "mix"}};
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, unknown, previous, 2, destination, error) ==
		Status::UnknownPort
	);
	const SourceAnimatorReplacement target[] = {{"base", "mix"}};
	staged.Keyframes.push_back({"base", "mix", 1, .3, "source", KeyframeEase{}});
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, destination, error) ==
		Status::InvalidValue
	);
	staged = Stage(doc, "base", .2);
	staged.Keyframes.back().Ease->InType = "invalid";
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, destination, error) ==
		Status::InvalidValue
	);
	staged.Keyframes.back().Ease = KeyframeEase{};
	staged.Keyframes.back().Ease->In.X = std::numeric_limits<double>::quiet_NaN();
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, destination, error) ==
		Status::InvalidValue
	);
	staged.Keyframes.back().Ease = KeyframeEase{};
	staged.Keyframes.back().Data = std::numeric_limits<double>::infinity();
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, destination, error) ==
		Status::LimitExceeded
	);
	CHECK(destination.RetainedBytes() == bytes);
	CHECK(destination.AuthoringRevision() == 1);
	REQUIRE(destination.SharedSubtype("base", "mix"));
	CHECK(destination.SharedSubtype("base", "mix")->Keys == keys);
}
TEST_CASE(
	"Physical animator replacement bounds old new and destination overlap atomically",
	"[imagegraph][groups][animator_replacement]"
) {
	auto doc = Graph();
	auto bound = Bound(doc);
	auto previous = Edit(doc, bound, 1, "copy", 5, .75);
	auto staged = Stage(doc, "base", .2);
	const SourceAnimatorReplacement target[] = {{"base", "mix"}};
	Diagnostic error;
	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	while (low < high) {
		const uint64_t mid = low + (high - low) / 2;
		GroupReplayState candidate;
		const auto status =
			RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, candidate, error, mid);
		if (status == Status::Ok)
			high = mid;
		else {
			REQUIRE(status == Status::LimitExceeded);
			low = mid + 1;
		}
	}
	GroupReplayState admitted;
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, admitted, error, low) ==
		Status::Ok
	);
	auto destination = Edit(doc, previous, 1, "local", 3, .6);
	const auto bytes = destination.RetainedBytes();
	const auto keys = destination.SharedSubtype("base", "mix")->Keys;
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, destination, error, low) ==
		Status::LimitExceeded
	);
	CHECK(destination.RetainedBytes() == bytes);
	CHECK(destination.SharedSubtype("base", "mix")->Keys == keys);
	const auto oldBytes = previous.RetainedBytes();
	CHECK(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, previous, error, low - 1) ==
		Status::LimitExceeded
	);
	CHECK(previous.RetainedBytes() == oldBytes);
	CHECK(previous.AuthoringRevision() == 1);
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(staged, target, previous, 2, previous, error, low) ==
		Status::Ok
	);
	CHECK(previous.AuthoringRevision() == 2);
	CHECK(previous.SharedSubtype("base", "mix") == nullptr);
}

TEST_CASE(
	"Cooked HLSL physical animators support fresh base and inherited replacements",
	"[imagegraph][groups][animator_replacement]"
) {
	Document doc;
	doc.FormatVersion = 9;
	for (const auto *id : {"base", "copy", "sibling"}) {
		Node node{id, "pc.hlsl", "", {}, {}};
		node.DynamicInputs = {
			{"argument_name_0", ValueType::Text, Value{std::string{"x"}}},
			{"argument_type_0", ValueType::Enum, Value{EnumValue{0}}},
			{"argument_value_0", ValueType::Scalar, Value{.5}}
		};
		node.SourceAnimatedInputs = {"argument_value_0"};
		if (node.Id != "base") node.InstanceBase = "base";
		doc.Nodes.push_back(std::move(node));
	}
	doc.Keyframes = {{"base", "argument_value_0", 0, .5, "source", KeyframeEase{}}};
	doc.Tracks = {{"base", "argument_value_0", "hold", -1}};
	Diagnostic error;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(doc, empty, 1, initial, error) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"copy", "base", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "argument_value_0"},
		{"sibling",
		 "base",
		 GroupSubtypeAnimator::Animated,
		 GroupSubtypeAnimator::Animated,
		 "argument_value_0"}
	};
	REQUIRE(BindGroupReplay(doc, bindings, initial, 1, bound, error) == Status::Ok);
	Value data = .9;
	GroupRefreshEvent edit;
	edit.NodeId = "copy";
	edit.Reason = GroupRefreshReason::Edit;
	edit.EditedPort = "argument_value_0";
	edit.LocalValue = &data;
	edit.LocalAnimated = true;
	edit.At.Tick = 2;
	GroupReplayState prior;
	REQUIRE(ReplayGroupAnimatorEdits(doc, {&edit, 1}, bound, 1, prior, error) == Status::Ok);
	REQUIRE(prior.SharedSubtype("base", "argument_value_0"));
	const auto baseKeys = prior.SharedSubtype("base", "argument_value_0")->Keys;
	for (const auto *id : {"base", "copy"}) {
		auto staged = doc;
		std::erase_if(staged.Keyframes, [&](const auto &key) {
			return key.NodeId == id && key.Port == "argument_value_0";
		});
		staged.Keyframes.push_back({id, "argument_value_0", 0, .25, "source", KeyframeEase{}});
		if (std::string_view{id} == "copy") staged.Tracks.push_back({id, "argument_value_0", "hold", -1});
		SourceAnimatorReplacement target{id, "argument_value_0"};
		GroupReplayState result;
		REQUIRE(
			RebindGroupReplayWithAnimatorReplacements(staged, {&target, 1}, prior, 2, result, error) ==
			Status::Ok
		);
		REQUIRE(result.Binding("copy", "argument_value_0"));
		CHECK(result.Binding("copy", "argument_value_0")->OwnerId == "base");
		REQUIRE(result.Binding("sibling", "argument_value_0"));
		if (std::string_view{id} == "base")
			CHECK(result.SharedSubtype("base", "argument_value_0") == nullptr);
		else {
			REQUIRE(result.SharedSubtype("base", "argument_value_0"));
			CHECK(result.SharedSubtype("base", "argument_value_0")->Keys == baseKeys);
		}
		Document projected;
		REQUIRE(ProjectGroupReplay(staged, result, 2, projected, error) == Status::Ok);
		const auto key =
			std::find_if(projected.Keyframes.begin(), projected.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == id && key.Port == "argument_value_0" && key.Tick == 0;
			});
		REQUIRE(key != projected.Keyframes.end());
		CHECK(key->Data == Value{.25});
		CHECK(projected.Nodes[1].InstanceOverrides.empty());
	}
	SourceAnimatorReplacement freshTarget{"base", "argument_value_0"};
	GroupReplayState fresh;
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(doc, {&freshTarget, 1}, empty, 1, fresh, error) ==
		Status::Ok
	);
	CHECK(fresh.SharedSubtypes().empty());
}
