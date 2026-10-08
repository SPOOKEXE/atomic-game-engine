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
		Diagnostic &diagnostic,
		SourceInputSelection selection
	) try {
		ENGINE_PROFILE("imagegraph.pending.rebuild");
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		const auto visit = [&](uint64_t units = 1) -> bool {
			if (work > 64'000'000 || units > 64'000'000 - work) return false;
			work += units;
			return true;
		};
		const size_t count = document.Nodes.size();
		if (count > Limits::MaximumNodes || (!reads.empty() && reads.size() != count) ||
			frozen.size() != count || (!completed.empty() && completed.size() != count) ||
			roots.size() > Limits::MaximumOutputs + Limits::MaximumNodes + 1 ||
			dynamicRoutes.size() > Limits::MaximumLinks ||
			(Active(selection) && (selection.NodeIndex >= count ||
								   selection.Ports.size() > Limits::MaximumSourceInputExpressionsPerNode)))
			return fail(Status::InvalidValue, "pending graph masks and roots do not match the document");
		if (Active(selection)) {
			for (size_t index = 0; index < selection.Ports.size(); ++index) {
				const auto port = selection.Ports[index];
				if (port.empty() || port.size() > Limits::MaximumTextBytes)
					return fail(Status::InvalidValue, "selected source input ports are invalid");
				if (!visit(1 + port.size()))
					return fail(
						Status::LimitExceeded, "selected source input validation exceeds its work budget"
					);
				for (size_t prior = 0; prior < index; ++prior) {
					const auto earlier = selection.Ports[prior];
					if (!visit(1 + std::min(port.size(), earlier.size())))
						return fail(
							Status::LimitExceeded, "selected source input validation exceeds its work budget"
						);
					if (earlier == port)
						return fail(Status::InvalidValue, "selected source input ports are invalid");
				}
			}
		}
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
		const auto frozenTunnelGetter = [&](size_t consumer) {
			if (!frozen[consumer] || document.Nodes[consumer].Type != "pc.tunnel_in") return false;
			const auto demanded = [&](const auto &routes) {
				return std::any_of(routes.begin(), routes.end(), [&](const PcxNamedDependency &route) {
					return route.Producer == consumer && route.Name == "$source.tunnel";
				});
			};
			return demanded(plan.PcxNamedDependencies) || demanded(dynamicRoutes);
		};
		const auto policy = [&](size_t consumer) {
			return frozenTunnelGetter(consumer) || reads.empty() ? SourceFrameCacheInputReads::All
																 : reads[consumer];
		};
		const auto append = [&](size_t producer, size_t consumer, bool surface) -> Status {
			if (!visit()) return Status::LimitExceeded;
			if (producer >= count || consumer >= count) return Status::UnknownNode;
			if (cutInputs && ((frozen[consumer] && !frozenTunnelGetter(consumer)) ||
							  policy(consumer) == SourceFrameCacheInputReads::None ||
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
			const auto consumer = indexOf(link.ToNode);
			if (!ReadsSourceInput(selection, consumer, link.ToPort)) continue;
			if (cutInputs && frozenTunnelGetter(consumer) && link.ToPort != "value_in") continue;
			const auto status = append(indexOf(link.FromNode), consumer, link.ToPort == "surface_in");
			if (status != Status::Ok) return edgeFailure(status);
		}
		for (const auto &route : plan.GroupSurfaceDependencies) {
			if (Active(selection) && route.Consumer == selection.NodeIndex) continue;
			const auto status = append(route.Producer, route.Consumer, false);
			if (status != Status::Ok) return edgeFailure(status);
		}
		for (const auto &route : plan.InlineOwnerDependencies) {
			if (route.ControlsOnly || (Active(selection) && route.Consumer == selection.NodeIndex)) continue;
			const auto status = append(route.Owner, route.Consumer, false);
			if (status != Status::Ok) return edgeFailure(status);
		}
		const auto routes = [&](const auto &entries, bool processorDependencies) -> Status {
			for (const auto &route : entries) {
				if (processorDependencies && Active(selection) && route.Consumer == selection.NodeIndex)
					continue;
				const auto status = append(route.Producer, route.Consumer, false);
				if (status != Status::Ok) return status;
			}
			return Status::Ok;
		};
		auto status = routes(plan.InlineControlDependencies, true);
		if (status != Status::Ok) return edgeFailure(status);
		status = routes(plan.PcxNamedDependencies, true);
		if (status != Status::Ok) return edgeFailure(status);
		status = routes(dynamicRoutes, false);
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
