#pragma once

// Identity publication may keep a source alive after its declared final read.
// The install-time contract pins that source before transient targets are pooled.

#include <engine/graph/RenderGraph.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace engine::render::detail {

	struct IdentityContract {
		graph::NodeId Node;
		graph::ResourceId Source;
		graph::ResourceId Target;
	};

	inline bool IdentityCompatible(const graph::ResourceDesc &source, const graph::ResourceDesc &target) {
		return !source.External && !target.External &&
			   source.Lifetime == graph::ResourceLifetime::Transient &&
			   target.Lifetime == graph::ResourceLifetime::Transient &&
			   source.Kind == graph::ResourceKind::Colour && target.Kind == source.Kind &&
			   source.Format == target.Format && source.Width == target.Width &&
			   source.Height == target.Height && source.Divisor == target.Divisor &&
			   source.Access == target.Access && source.Samples == 1 && target.Samples == 1 &&
			   source.Depth == 1 && target.Depth == 1 && source.Layers == 1 && target.Layers == 1 &&
			   source.FirstMip == 0 && target.FirstMip == 0 && source.MipCount == 1 && target.MipCount == 1 &&
			   source.ColourSpace == target.ColourSpace && source.AlphaSpace == target.AlphaSpace &&
			   source.Owner == target.Owner;
	}

	inline graph::ResourceId IdentityPort(const graph::Node &node, bool write) {
		const auto &resources = write ? node.Writes : node.Reads;
		if (resources.empty()) return {};
		if (node.Kind != core::Name("shader-lenses")) return resources.front();
		const auto &ports = write ? node.WritePorts : node.ReadPorts;
		const auto found = std::find(ports.begin(), ports.end(), core::Name("colour"));
		const size_t index = static_cast<size_t>(found - ports.begin());
		return found != ports.end() && index < resources.size() ? resources[index] : graph::ResourceId{};
	}

	// A fixed output also publishes through other logical names on that texture.
	// Those readers need a materialized image even when this output can forward.
	template <typename IsAlias>
	bool IdentityAliasReadAfter(
		const graph::RenderGraph &graph,
		std::span<const graph::NodeId> ordered,
		graph::NodeId publication,
		graph::ResourceId target,
		const IsAlias &isAlias
	) {
		bool after = !publication.IsValid();
		for (const auto id : ordered) {
			if (id == publication) {
				after = true;
				continue;
			}
			const auto *node = graph.Find(id);
			if (!after || !node || !node->Enabled) continue;
			for (const auto read : node->Reads)
				if (read != target && isAlias(read)) return true;
		}
		return false;
	}

	inline std::vector<IdentityContract>
	BuildIdentityContracts(const graph::RenderGraph &graph, std::span<const graph::NodeId> ordered) {
		std::vector<IdentityContract> contracts;
		for (size_t index = 0; index < ordered.size(); ++index) {
			const auto *node = graph.Find(ordered[index]);
			if (!node || node->Scope != graph::NodeScope::View) continue;
			const auto kind = node->Kind;
			if (kind != core::Name("sky") && kind != core::Name("fog") &&
				kind != core::Name("shader-lenses") && kind != core::Name("dof") &&
				kind != core::Name("god-rays") && kind != core::Name("portal-overlay") &&
				kind != core::Name("mirror-overlay") && kind != core::Name("transparent"))
				continue;
			const auto source = IdentityPort(*node, false), target = IdentityPort(*node, true);
			const auto *from = graph.FindResource(source), *to = graph.FindResource(target);
			if (!from || !to || !IdentityCompatible(*from, *to)) continue;
			bool sourceView = false;
			bool rewritten = false;
			for (size_t writer = 0; writer < ordered.size(); ++writer) {
				const auto *candidate = graph.Find(ordered[writer]);
				if (!candidate || std::find(candidate->Writes.begin(), candidate->Writes.end(), source) ==
									  candidate->Writes.end())
					continue;
				if (writer < index && candidate->Scope == graph::NodeScope::View) sourceView = true;
				if (writer >= index) rewritten = true;
			}
			if (sourceView && !rewritten) contracts.push_back({ordered[index], source, target});
		}
		return contracts;
	}
}
