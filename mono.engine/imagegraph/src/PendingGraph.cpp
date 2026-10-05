#include "PendingGraph.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>

namespace engine::imagegraph::detail {
	PendingGraph::PendingGraph(EvaluationBudget &budget)
		: Budget(budget), Upstream(EvaluationAllocator<Sources>(budget)),
		  Needed(EvaluationAllocator<uint8_t>(budget)),
		  RemainingConsumers(EvaluationAllocator<size_t>(budget)) {}

	Status PendingGraph::Rebuild(
		const Document &document,
		const Plan &plan,
		std::span<const SourceFrameCacheInputReads> reads,
		std::span<const CacheGroupReplayNode *const> frozen,
		std::span<const size_t> roots,
		std::span<const PcxNamedDependency> dynamicRoutes,
		std::span<const uint8_t> completed,
		bool cutInputs,
		uint64_t &work,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.pending.rebuild");
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		const size_t count = document.Nodes.size();
		if (count > Limits::MaximumNodes || (!reads.empty() && reads.size() != count) ||
			frozen.size() != count || (!completed.empty() && completed.size() != count) ||
			roots.size() > Limits::MaximumOutputs + Limits::MaximumNodes + 1 ||
			dynamicRoutes.size() > Limits::MaximumLinks)
			return fail(Status::InvalidValue, "pending graph masks and roots do not match the document");
		const auto visit = [&](uint64_t units = 1) -> bool {
			if (work > 64'000'000 || units > 64'000'000 - work) return false;
			work += units;
			return true;
		};
		if (!visit(count)) return fail(Status::LimitExceeded, "pending graph exceeds its work budget");
		using Index = std::pair<std::string_view, size_t>;
		EvaluationVector<Index> indices{EvaluationAllocator<Index>(Budget)};
		indices.reserve(count);
		for (size_t index = 0; index < count; ++index)
			indices.emplace_back(document.Nodes[index].Id, index);
		std::sort(indices.begin(), indices.end());
		const auto indexOf = [&](std::string_view id) -> size_t {
			const auto found = std::lower_bound(
				indices.begin(), indices.end(), id, [](const Index &row, std::string_view value) {
					return row.first < value;
				}
			);
			return found != indices.end() && found->first == id ? found->second : count;
		};
		PendingGraph candidate(Budget);
		candidate.Upstream.reserve(count);
		candidate.Needed.resize(count, 0);
		candidate.RemainingConsumers.resize(count, 0);
		for (size_t index = 0; index < count; ++index)
			candidate.Upstream.emplace_back(EvaluationAllocator<size_t>(Budget));
		const auto policy = [&](size_t consumer) {
			return reads.empty() ? SourceFrameCacheInputReads::All : reads[consumer];
		};
		const auto append = [&](size_t producer, size_t consumer, bool surface) -> Status {
			if (!visit()) return Status::LimitExceeded;
			if (producer >= count || consumer >= count) return Status::UnknownNode;
			if (cutInputs && (frozen[consumer] || policy(consumer) == SourceFrameCacheInputReads::None ||
							  (surface && policy(consumer) == SourceFrameCacheInputReads::ControlsOnly)))
				return Status::Ok;
			auto &sources = candidate.Upstream[consumer];
			if (!visit(sources.size())) return Status::LimitExceeded;
			if (std::find(sources.begin(), sources.end(), producer) == sources.end())
				sources.push_back(producer);
			return Status::Ok;
		};
		const auto edgeFailure = [&](Status code) {
			return fail(
				code,
				code == Status::LimitExceeded ? "pending graph edges exceed their work budget"
											  : "pending graph edge names an unknown node"
			);
		};
		for (const auto &link : plan.EffectiveLinks) {
			const auto status =
				append(indexOf(link.FromNode), indexOf(link.ToNode), link.ToPort == "surface_in");
			if (status != Status::Ok) return edgeFailure(status);
		}
		for (const auto &route : plan.GroupSurfaceDependencies) {
			const auto status = append(route.Producer, route.Consumer, false);
			if (status != Status::Ok) return edgeFailure(status);
		}
		for (const auto &route : plan.InlineOwnerDependencies) {
			if (route.ControlsOnly) continue;
			const auto status = append(route.Owner, route.Consumer, false);
			if (status != Status::Ok) return edgeFailure(status);
		}
		const auto routes = [&](const auto &entries) -> Status {
			for (const auto &route : entries) {
				const auto status = append(route.Producer, route.Consumer, false);
				if (status != Status::Ok) return status;
			}
			return Status::Ok;
		};
		auto status = routes(plan.InlineControlDependencies);
		if (status != Status::Ok) return edgeFailure(status);
		status = routes(plan.PcxNamedDependencies);
		if (status != Status::Ok) return edgeFailure(status);
		status = routes(dynamicRoutes);
		if (status != Status::Ok) return edgeFailure(status);
		Sources pending{EvaluationAllocator<size_t>(Budget)};
		pending.assign(roots.begin(), roots.end());
		while (!pending.empty()) {
			if (!visit())
				return fail(Status::LimitExceeded, "pending graph reachability exceeds its work budget");
			const auto current = pending.back();
			pending.pop_back();
			if (current >= count)
				return fail(Status::UnknownNode, "pending graph root names an unknown node");
			if (candidate.Needed[current]) continue;
			candidate.Needed[current] = 1;
			if (!completed.empty() && completed[current]) continue;
			for (const auto source : candidate.Upstream[current])
				pending.push_back(source);
		}
		for (size_t consumer = 0; consumer < count; ++consumer) {
			if (!candidate.Needed[consumer] || (!completed.empty() && completed[consumer])) continue;
			for (const auto source : candidate.Upstream[consumer])
				++candidate.RemainingConsumers[source];
		}
		Upstream.swap(candidate.Upstream);
		Needed.swap(candidate.Needed);
		RemainingConsumers.swap(candidate.RemainingConsumers);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "pending graph allocation was refused"};
		return diagnostic.Code;
	}
}
