#include "PendingGraph.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>

TEST_SUITE_ID("engine.imagegraph.pending_graph")
using namespace engine::imagegraph;

namespace {
	Document Scene() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"surface",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}},
			{"control", "value.boolean", "", {}, {{"value", true}}},
			{"cache", "pc.cache", "", {}, {{"animated", false}}},
			{"computed", "value.boolean", "", {}, {{"value", false}}}
		};
		document.Links = {
			{"surface", "image", "cache", "surface_in"}, {"control", "boolean", "cache", "animated"}
		};
		document.Outputs = {{"out", "cache", "cache_surface"}};
		return document;
	}
	Plan Compiled(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
}

TEST_CASE(
	"Pending graph rebuild restores edges across hit freeze and wake transitions",
	"[imagegraph][pending_graph]"
) {
	const auto document = Scene();
	auto plan = Compiled(document);
	const auto structural = plan;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::PendingGraph graph(budget);
	std::array<detail::SourceFrameCacheInputReads, 4> reads;
	reads.fill(detail::SourceFrameCacheInputReads::All);
	std::array<const CacheGroupReplayNode *, 4> frozen{};
	const std::array<size_t, 1> roots{2};
	const std::array<PcxNamedDependency, 1> dynamic{{{2, 3, "computed", "boolean", false}}};
	Diagnostic diagnostic;
	uint64_t work = 0;
	const auto rebuild = [&] {
		return graph.Rebuild(document, plan, reads, frozen, roots, dynamic, {}, true, work, diagnostic);
	};
	REQUIRE(rebuild() == Status::Ok);
	CHECK(
		graph.Needed ==
		detail::EvaluationVector<uint8_t>({1, 1, 1, 1}, detail::EvaluationAllocator<uint8_t>(budget))
	);
	CHECK(graph.Upstream[2].size() == 3);
	reads[2] = detail::SourceFrameCacheInputReads::ControlsOnly;
	REQUIRE(rebuild() == Status::Ok);
	CHECK_FALSE(graph.Needed[0]);
	CHECK(graph.Needed[1]);
	CHECK(graph.Needed[3]);
	plan.GroupSurfaceDependencies.push_back({2, 0, "format"});
	REQUIRE(rebuild() == Status::Ok);
	CHECK(graph.Needed[0]);
	CHECK(graph.RemainingConsumers[0] == 1);
	reads[2] = detail::SourceFrameCacheInputReads::None;
	REQUIRE(rebuild() == Status::Ok);
	CHECK(graph.Upstream[2].empty());
	CHECK_FALSE(graph.Needed[0]);
	CHECK_FALSE(graph.Needed[3]);
	reads[2] = detail::SourceFrameCacheInputReads::All;
	CacheGroupReplayNode inactive;
	frozen[2] = &inactive;
	REQUIRE(rebuild() == Status::Ok);
	CHECK(graph.Upstream[2].empty());
	frozen[2] = nullptr;
	REQUIRE(rebuild() == Status::Ok);
	CHECK(graph.Upstream[2].size() == 3);
	CHECK(graph.RemainingConsumers[0] == 1);
	CHECK(structural.EffectiveLinks == plan.EffectiveLinks);
}

TEST_CASE(
	"Pending graph rebuild retains completed producers without re-admitting their inputs",
	"[imagegraph][pending_graph]"
) {
	const auto document = Scene();
	const auto plan = Compiled(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::PendingGraph graph(budget);
	std::array<const CacheGroupReplayNode *, 4> frozen{};
	const std::array<size_t, 2> roots{2, 3};
	const std::array<PcxNamedDependency, 1> dynamic{{{3, 2, "cache", "cache_surface", false}}};
	std::array<uint8_t, 4> completed{0, 0, 1, 0};
	Diagnostic diagnostic;
	uint64_t work = 0;
	REQUIRE(
		graph.Rebuild(document, plan, {}, frozen, roots, dynamic, completed, true, work, diagnostic) ==
		Status::Ok
	);
	CHECK_FALSE(graph.Needed[0]);
	CHECK_FALSE(graph.Needed[1]);
	CHECK(graph.Needed[2]);
	CHECK(graph.Needed[3]);
	CHECK(graph.RemainingConsumers[2] == 1);
	CHECK(graph.RemainingConsumers[0] == 0);
	completed[2] = 0;
	REQUIRE(
		graph.Rebuild(document, plan, {}, frozen, roots, dynamic, completed, true, work, diagnostic) ==
		Status::Ok
	);
	CHECK(graph.Needed[0]);
	CHECK(graph.Needed[1]);
}

TEST_CASE(
	"Pending graph replacements admit overlapping tables and refuse atomically",
	"[imagegraph][pending_graph][evaluation_budget]"
) {
	const auto document = Scene();
	const auto plan = Compiled(document);
	const std::array<size_t, 1> roots{2};
	std::array<const CacheGroupReplayNode *, 4> frozen{};
	const std::array<PcxNamedDependency, 1> dynamic{{{2, 3, "computed", "boolean", false}}};
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		detail::EvaluationBudget budget(
			attempt == 0   ? Limits::MaximumEvaluationBytes
			: attempt == 1 ? peak - 1
						   : peak
		);
		detail::PendingGraph graph(budget);
		Diagnostic diagnostic;
		uint64_t work = 0;
		REQUIRE(
			graph.Rebuild(document, plan, {}, frozen, roots, {}, {}, true, work, diagnostic) == Status::Ok
		);
		const auto oldBytes = budget.Used();
		const auto status =
			graph.Rebuild(document, plan, {}, frozen, roots, dynamic, {}, true, work, diagnostic);
		if (attempt == 1) {
			CHECK(status == Status::LimitExceeded);
			CHECK(graph.Upstream[2].size() == 2);
			CHECK_FALSE(graph.Needed[3]);
			CHECK(budget.Used() == oldBytes);
		} else {
			REQUIRE(status == Status::Ok);
			CHECK(graph.Upstream[2].size() == 3);
			CHECK(graph.Needed[3]);
			if (attempt == 0) peak = budget.Peak();
			CHECK(budget.Peak() == peak);
			const auto retained = budget.Used();
			uint64_t exhausted = 64'000'000;
			CHECK(
				graph.Rebuild(document, plan, {}, frozen, roots, {}, {}, true, exhausted, diagnostic) ==
				Status::LimitExceeded
			);
			CHECK(graph.Upstream[2].size() == 3);
			CHECK(budget.Used() == retained);
			const std::array<size_t, 1> invalidRoots{document.Nodes.size()};
			CHECK(
				graph.Rebuild(document, plan, {}, frozen, invalidRoots, {}, {}, true, work, diagnostic) ==
				Status::UnknownNode
			);
			CHECK(graph.Needed[3]);
			CHECK(budget.Used() == retained);
		}
	}
}

TEST_CASE(
	"Pending graph selected inputs skip unrelated producers and processor routes",
	"[imagegraph][pending_graph][source_input]"
) {
	const auto document = Scene();
	auto plan = Compiled(document);
	plan.EffectiveLinks = {
		{"unsupported", "out", "cache", "unused"}, {"control", "boolean", "cache", "animated"}
	};
	plan.PcxNamedDependencies.push_back({2, 3, "static", "boolean", false});
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::PendingGraph graph(budget);
	std::array<const CacheGroupReplayNode *, 4> frozen{};
	const std::array<size_t, 1> roots{2};
	const std::array<std::string_view, 1> ports{"animated"};
	const std::array<PcxNamedDependency, 1> dynamic{{{2, 0, "dynamic", "image", false}}};
	Diagnostic diagnostic;
	uint64_t work = 0;
	REQUIRE(
		graph.Rebuild(document, plan, {}, frozen, roots, dynamic, {}, false, work, diagnostic, {2, ports}) ==
		Status::Ok
	);
	CHECK(graph.Upstream[2].size() == 2);
	CHECK(std::find(graph.Upstream[2].begin(), graph.Upstream[2].end(), 1) != graph.Upstream[2].end());
	CHECK(std::find(graph.Upstream[2].begin(), graph.Upstream[2].end(), 0) != graph.Upstream[2].end());
	CHECK(std::find(graph.Upstream[2].begin(), graph.Upstream[2].end(), 3) == graph.Upstream[2].end());
	CHECK_FALSE(graph.Needed[3]);
}

TEST_CASE(
	"Pending graph rejects invalid selected inputs without replacing its graph",
	"[imagegraph][pending_graph][source_input][evaluation_budget]"
) {
	const auto document = Scene();
	const auto plan = Compiled(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::PendingGraph graph(budget);
	std::array<const CacheGroupReplayNode *, 4> frozen{};
	const std::array<size_t, 1> roots{2};
	Diagnostic diagnostic;
	uint64_t work = 0;
	REQUIRE(graph.Rebuild(document, plan, {}, frozen, roots, {}, {}, true, work, diagnostic) == Status::Ok);
	const auto retained = budget.Used();
	const auto oldUpstreamSize = graph.Upstream[2].size();
	const std::array<std::string_view, 2> duplicate{"animated", "animated"};
	CHECK(
		graph.Rebuild(document, plan, {}, frozen, roots, {}, {}, true, work, diagnostic, {2, duplicate}) ==
		Status::InvalidValue
	);
	CHECK(graph.Upstream[2].size() == oldUpstreamSize);
	CHECK(budget.Used() == retained);
	CHECK(
		graph.Rebuild(
			document, plan, {}, frozen, roots, {}, {}, true, work, diagnostic, {document.Nodes.size(), {}}
		) == Status::InvalidValue
	);
	CHECK(graph.Upstream[2].size() == oldUpstreamSize);
	CHECK(budget.Used() == retained);
	uint64_t exhausted = 64'000'000;
	CHECK(
		graph.Rebuild(document, plan, {}, frozen, roots, {}, {}, true, exhausted, diagnostic, {2, {}}) ==
		Status::LimitExceeded
	);
	CHECK(graph.Upstream[2].size() == oldUpstreamSize);
	CHECK(budget.Used() == retained);
}
