#pragma once

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <unordered_map>

namespace engine::render::detail {
	// A pipeline-owned lookup for declaration-order node resolution.
	class EnabledNodeIndex {
	  public:
		void Rebuild(const graph::RenderGraph &graph);

		// Includes disabled declarations, as used when resolving authored extents.
		const graph::Node *First(const graph::RenderGraph &graph, core::Name kind) const;

		// Returns the first enabled declaration, with capture-kind fallbacks.
		const graph::Node *Enabled(const graph::RenderGraph &graph, core::Name kind) const;

	  private:
		std::unordered_map<core::Name, graph::NodeId> FirstByKind;
		std::unordered_map<core::Name, graph::NodeId> FirstEnabledByKind;
	};

	// Out-of-line scan oracles keep parity benchmarks on the renderer's build preset.
	const graph::Node *FindFirstNodeByScan(const graph::RenderGraph &graph, core::Name kind);
	const graph::Node *FindEnabledNodeByScan(const graph::RenderGraph &graph, core::Name kind);

}
