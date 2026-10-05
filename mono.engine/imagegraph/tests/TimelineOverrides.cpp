#include "TimelineOverrides.hpp"

#include "EvaluationAllocator.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraph.timeline_overrides")
using namespace engine::imagegraph;

namespace {
	const Value &Property(const Node &node, std::string_view port) {
		const auto found = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
			return value.Port == port;
		});
		REQUIRE(found != node.Values.end());
		return found->Data;
	}
	void Checked(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
}

TEST_CASE(
	"Timeline extension merges rich late dependencies under an exact overlapping byte limit",
	"[imagegraph][timeline_overrides][evaluation_budget]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"array", "pc.number", "", {}, {{"value", ArrayValue{ValueType::Scalar, {0., 2., 4.}}}}},
		{"collector", "value.array", "", {}, {{"spread", false}}},
		{"text", "value.text", "", {}, {{"value", std::string(512, 'a')}}}
	};
	document.Nodes[1].DynamicInputs = {{"item", ValueType::Text, std::string(1024, 'b')}};
	document.Keyframes = {
		{"array", "value", 0, ArrayValue{ValueType::Scalar, {5., 7.}}, "source", KeyframeEase{}},
		{"collector", "item", 0, std::string(513, 'c'), "step"},
		{"text", "value", 0, std::string(2048, 'd'), "step"}
	};
	document.Tracks = {{"array", "value", "hold", -1}};
	document.Outputs = {{"out", "collector", "array"}};
	Checked(document);
	const auto original = document;
	const std::array<uint8_t, 3> first{0, 1, 0}, all{1, 1, 1};
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		detail::TimelineOverrides result;
		Diagnostic diagnostic;
		REQUIRE(
			detail::ResolveTimelineOverrides(document, first, {}, budget, result, diagnostic) == Status::Ok
		);
		const Node retained = result.Nodes.front().Authored;
		const auto charge = result.Charge.Bytes();
		const auto observation = result.Observation;
		const auto status = detail::ExtendTimelineOverrides(document, all, {}, budget, result, diagnostic);
		INFO(diagnostic.Message);
		if (attempt == 1) {
			CHECK(status == Status::LimitExceeded);
			REQUIRE(result.Nodes.size() == 1);
			CHECK(result.Nodes.front().NodeIndex == 1);
			CHECK(result.Nodes.front().Authored == retained);
			CHECK(result.Charge.Bytes() == charge);
			CHECK(result.Observation == observation);
		} else {
			REQUIRE(status == Status::Ok);
			REQUIRE(result.Nodes.size() == 3);
			for (size_t index = 0; index < 3; ++index)
				CHECK(result.Nodes[index].NodeIndex == index);
			CHECK(result.Nodes[1].Authored == retained);
			CHECK(
				Property(result.Nodes[0].Authored, "value") == Value{ArrayValue{ValueType::Scalar, {5., 7.}}}
			);
			CHECK(Property(result.Nodes[2].Authored, "value") == Value{std::string(2048, 'd')});
			if (attempt == 0) peak = budget.Peak();
			CHECK(budget.Peak() == peak);
			const auto mergedCharge = result.Charge.Bytes();
			REQUIRE(
				detail::ExtendTimelineOverrides(document, all, {}, budget, result, diagnostic) == Status::Ok
			);
			CHECK(result.Charge.Bytes() == mergedCharge);
			CHECK(result.Nodes[1].Authored == retained);
		}
		CHECK(budget.Used() == result.Charge.Bytes());
		CHECK(document == original);
	}
}

TEST_CASE(
	"Timeline extension refuses changed observations and ledgers without losing prior samples",
	"[imagegraph][timeline_overrides][evaluation_budget]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"first", "value.number", "", {}, {{"value", 0.0}}},
		{"late", "value.number", "", {}, {{"value", 0.0}}}
	};
	document.Keyframes = {{"first", "value", 0, 3.0, "step"}, {"late", "value", 0, 7.0, "step"}};
	document.Outputs = {{"out", "first", "number"}};
	Checked(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides result;
	Diagnostic diagnostic;
	const std::array<uint8_t, 2> first{1, 0}, all{1, 1};
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {0, .25, true}));
	REQUIRE(
		detail::ResolveTimelineOverrides(document, first, request, budget, result, diagnostic) == Status::Ok
	);
	const Node retained = result.Nodes.front().Authored;
	const auto charge = result.Charge.Bytes();
	const auto observation = result.Observation;
	SECTION("changed signed fractional clock") {
		REQUIRE(SetFrameTime(request, {0, .25, false}));
		CHECK(
			detail::ExtendTimelineOverrides(document, all, request, budget, result, diagnostic) ==
			Status::InvalidValue
		);
	}
	SECTION("invalid dependency cone") {
		CHECK(
			detail::ExtendTimelineOverrides(
				document, std::span(first).first(1), request, budget, result, diagnostic
			) == Status::InvalidValue
		);
	}
	SECTION("another live byte ledger") {
		detail::EvaluationBudget other(Limits::MaximumEvaluationBytes);
		CHECK(
			detail::ExtendTimelineOverrides(document, all, request, other, result, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(other.Used() == 0);
	}
	REQUIRE(result.Nodes.size() == 1);
	CHECK(result.Nodes.front().Authored == retained);
	CHECK(result.Charge.Bytes() == charge);
	CHECK(result.Observation == observation);
	CHECK(budget.Used() == charge);
}

TEST_CASE(
	"Timeline overrides sample signed fractions without cloning unrelated nodes",
	"[imagegraph][timeline_overrides]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"number", "value.number", "", {}, {{"value", 0.0}}},
		{"unrelated", "value.text", "", {}, {{"value", std::string(2048, 'x')}}}
	};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {
		{"number", "value", 1, 2.0, "linear"},
		{"number", "value", 2, 10.0, "linear"},
		{"unrelated", "value", 0, std::string(4096, 'y'), "step"}
	};
	REQUIRE(SetFrameTime(document.Keyframes[0], {1, .5, true}));
	REQUIRE(SetFrameTime(document.Keyframes[1], {2, .5, false}));
	Checked(document);
	const auto before = document;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides result;
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {0, .5, false}));
	Diagnostic diagnostic;
	const std::array<uint8_t, 2> needed{1, 0};
	REQUIRE(
		detail::ResolveTimelineOverrides(document, needed, request, budget, result, diagnostic) == Status::Ok
	);
	REQUIRE(result.Nodes.size() == 1);
	CHECK(result.Nodes.front().NodeIndex == 0);
	CHECK(Property(result.Find(0, document.Nodes[0]), "value") == Value{6.0});
	CHECK(&result.Find(1, document.Nodes[1]) == &document.Nodes[1]);
	CHECK(document == before);
	CHECK(budget.Used() == result.Charge.Bytes());
}

TEST_CASE(
	"Timeline overrides preserve old sampled arrays when overlapping replacement is refused",
	"[imagegraph][timeline_overrides][evaluation_budget]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{std::string(128, 'n'), "pc.number", "", {}, {{"value", ArrayValue{ValueType::Scalar, {0., 2., 4.}}}}}
	};
	const auto &id = document.Nodes.front().Id;
	document.Outputs = {{"out", id, "number"}};
	document.Keyframes = {
		{id, "value", 0, ArrayValue{ValueType::Scalar, {0., 2., 4.}}, "source", KeyframeEase{}},
		{id, "value", 4, ArrayValue{ValueType::Scalar, {10., 12.}}, "source", KeyframeEase{}}
	};
	document.Keyframes.front().SourceDriver = KeyframeLinearDriver{2};
	document.Tracks = {{id, "value", "hold", -1}};
	document.Timeline = TimelineSettings{8, 0, 7, "loop", 30};
	Checked(document);
	const auto original = document;
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		detail::TimelineOverrides result;
		Diagnostic diagnostic;
		const std::array<uint8_t, 1> needed{1};
		REQUIRE(
			detail::ResolveTimelineOverrides(document, needed, {}, budget, result, diagnostic) == Status::Ok
		);
		REQUIRE(result.Nodes.size() == 1);
		const Node previous = result.Nodes.front().Authored;
		const uint64_t oldCharge = result.Charge.Bytes();
		EvaluationRequest request;
		request.Tick = 2;
		const Status status =
			detail::ResolveTimelineOverrides(document, needed, request, budget, result, diagnostic);
		INFO(diagnostic.Message);
		if (attempt == 1) {
			CHECK(status == Status::LimitExceeded);
			CHECK(result.Nodes.front().Authored == previous);
			CHECK(result.Charge.Bytes() == oldCharge);
		} else {
			REQUIRE(status == Status::Ok);
			CHECK(
				Property(result.Nodes.front().Authored, "value") ==
				Value{ArrayValue{ValueType::Scalar, {9., 11.}}}
			);
			if (attempt == 0) peak = budget.Peak();
			CHECK(budget.Peak() == peak);
		}
		CHECK(budget.Used() == result.Charge.Bytes());
		CHECK(document == original);
	}
}

TEST_CASE(
	"Timeline overrides insert catalogue defaults and replace dynamic rich payloads",
	"[imagegraph][timeline_overrides]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"fresh", "pc.number", "", {}, {}}, {"collector", "value.array", "", {}, {{"spread", false}}}
	};
	document.Nodes.back().DynamicInputs = {{"item", ValueType::Text, std::string(1024, 'a')}};
	document.Outputs = {{"out", "collector", "array"}};
	document.Keyframes = {
		{"fresh", "value", 0, 3.0, "step"}, {"collector", "item", 0, std::string(513, 'b'), "step"}
	};
	Checked(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides result;
	Diagnostic diagnostic;
	const std::array<uint8_t, 2> needed{1, 1};
	REQUIRE(detail::ResolveTimelineOverrides(document, needed, {}, budget, result, diagnostic) == Status::Ok);
	REQUIRE(result.Nodes.size() == 2);
	CHECK(Property(result.Nodes.front().Authored, "value") == Value{3.0});
	CHECK(
		result.Nodes.back().Authored.DynamicInputs.front().Default ==
		std::optional<Value>{std::string(513, 'b')}
	);
	CHECK(budget.Used() == result.Charge.Bytes());
	CHECK(budget.Peak() > budget.Used());
	CHECK(document.Nodes.front().Values.empty());
	CHECK(
		document.Nodes.back().DynamicInputs.front().Default == std::optional<Value>{std::string(1024, 'a')}
	);
}

TEST_CASE(
	"Timeline map nodes admit metadata with a prior sampled result still live",
	"[imagegraph][timeline_overrides][evaluation_budget]"
) {
	Document previous;
	previous.FormatVersion = 9;
	previous.Nodes = {{"previous", "value.number", "", {}, {{"value", 0.0}}}};
	previous.Outputs = {{"out", "previous", "number"}};
	previous.Keyframes = {{"previous", "value", 0, 19.0, "step"}};
	Checked(previous);
	Document document;
	document.FormatVersion = 9;
	constexpr size_t count = 64;
	for (size_t index = 0; index < count; ++index) {
		const auto id = "number" + std::to_string(index);
		document.Nodes.push_back({id, "value.number", "", {}, {{"value", 0.0}}});
		document.Keyframes.push_back({id, "value", 0, 2.0, "step"});
		document.Tracks.push_back({id, "value", "hold", -1});
	}
	document.Outputs = {{"out", document.Nodes.front().Id, "number"}};
	Checked(document);
	const uint64_t workspace =
		count * (sizeof(std::pair<std::string_view, size_t>) + sizeof(const Keyframe *));
	const std::array<uint8_t, 1> previousNeeded{1};

	SECTION("configured map exact peak includes its rebound nodes") {
		uint64_t peak = 0;
		for (size_t attempt = 0; attempt < 3; ++attempt) {
			const uint64_t limit = attempt == 0	  ? Limits::MaximumEvaluationBytes
								   : attempt == 1 ? peak - 1
												  : peak;
			detail::EvaluationBudget budget(limit);
			detail::TimelineOverrides result;
			Diagnostic diagnostic;
			REQUIRE(
				detail::ResolveTimelineOverrides(previous, previousNeeded, {}, budget, result, diagnostic) ==
				Status::Ok
			);
			const Node retained = result.Nodes.front().Authored;
			const uint64_t previousBytes = result.Charge.Bytes();
			const std::array<uint8_t, count> needed{};
			const Status status =
				detail::ResolveTimelineOverrides(document, needed, {}, budget, result, diagnostic);
			if (attempt == 1) {
				CHECK(status == Status::LimitExceeded);
				CHECK(result.Nodes.front().Authored == retained);
				CHECK(budget.Used() == previousBytes);
			} else {
				REQUIRE(status == Status::Ok);
				CHECK(result.Nodes.empty());
				CHECK(budget.Used() == 0);
				if (attempt == 0) peak = budget.Peak();
				CHECK(budget.Peak() == peak);
				using Key = std::pair<std::string_view, std::string_view>;
				CHECK(
					peak - previousBytes - workspace >
					count * sizeof(std::pair<const Key, const AnimationTrack *>)
				);
			}
		}
	}
	SECTION("track allocation refuses before any sampled node clone") {
		using Key = std::pair<size_t, std::string_view>;
		using Track = std::pair<size_t, std::vector<const Keyframe *>>;
		uint64_t nodeBytes = 0;
		{
			detail::EvaluationBudget probe(Limits::MaximumEvaluationBytes);
			auto map = detail::MakeEvaluationMap<Key, Track>(probe);
			map.emplace(Key{0, "value"}, Track{});
			nodeBytes = probe.Used();
			REQUIRE(nodeBytes > sizeof(std::pair<const Key, Track>));
		}
		uint64_t previousBytes = 0;
		{
			detail::EvaluationBudget probe(Limits::MaximumEvaluationBytes);
			detail::TimelineOverrides retained;
			Diagnostic diagnostic;
			REQUIRE(
				detail::ResolveTimelineOverrides(previous, previousNeeded, {}, probe, retained, diagnostic) ==
				Status::Ok
			);
			previousBytes = retained.Charge.Bytes();
		}
		detail::EvaluationBudget budget(previousBytes + workspace + count * nodeBytes - 1);
		detail::TimelineOverrides result;
		Diagnostic diagnostic;
		REQUIRE(
			detail::ResolveTimelineOverrides(previous, previousNeeded, {}, budget, result, diagnostic) ==
			Status::Ok
		);
		const Node retained = result.Nodes.front().Authored;
		std::array<uint8_t, count> needed;
		needed.fill(1);
		CHECK(
			detail::ResolveTimelineOverrides(document, needed, {}, budget, result, diagnostic) ==
			Status::LimitExceeded
		);
		CHECK(result.Nodes.front().Authored == retained);
		CHECK(budget.Used() == previousBytes);
		CHECK(budget.Peak() == previousBytes + workspace + (count - 1) * nodeBytes);
	}
}

TEST_CASE(
	"sampled nodes preserve source expressions metadata and dynamic ports", "[imagegraph][timeline_overrides]"
) {
	Document document;
	document.FormatVersion = 9;
	Node node{"number", "value.number", "", {}, {{"value", 0.0}}};
	node.SourceDisplayName = std::string(64, 'd');
	node.SourceInternalName = std::string(64, 'n');
	node.DynamicInputs = {{"extra", ValueType::Scalar, 2.0, std::string(64, 'l')}};
	node.DynamicOutputs = {{std::string(64, 'o'), ValueType::Scalar}};
	node.SourceInputExpressions = {{"value", std::string(1024, 'c'), false}};
	node.SourceProperties = {{"caption", std::string(2048, 'p')}};
	document.Nodes = {node};
	document.Keyframes = {{"number", "value", 0, 0.0, "linear"}, {"number", "value", 2, 10.0, "linear"}};
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides result;
	EvaluationRequest request;
	request.Tick = 1;
	Diagnostic diagnostic;
	const std::array<uint8_t, 1> needed{1};
	REQUIRE(
		detail::ResolveTimelineOverrides(document, needed, request, budget, result, diagnostic) == Status::Ok
	);
	auto expected = node;
	expected.Values[0].Data = 5.0;
	CHECK(result.Find(0, document.Nodes[0]) == expected);
	CHECK(document.Nodes[0] == node);
	CHECK(result.Charge.Bytes() >= 3072);
	detail::EvaluationBudget tight(result.Charge.Bytes() - 1);
	detail::TimelineOverrides refused;
	CHECK(
		detail::ResolveTimelineOverrides(document, needed, request, tight, refused, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(refused.Nodes.empty());
	CHECK(tight.Used() == 0);
}
