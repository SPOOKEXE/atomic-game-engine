#pragma once

// The device-free dispatch seam between a compiled render graph and its GPU
// backend. The table is assembled by the renderer, then RenderGraph owns the
// traversal. A missing implementation is rejected before recording starts.

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <functional>
#include <unordered_map>
#include <vector>

namespace engine::render {

	using NodeHandler = std::function<bool(const graph::RunContext &)>;

	class NodeTable {
	  public:
		bool Set(core::Name kind, NodeHandler handler);
		bool Has(core::Name kind) const;
		const NodeHandler *Find(core::Name kind) const;
		std::vector<core::Name> Missing(const graph::RenderGraph &graph) const;
		size_t Count() const;
		void Clear();

	  private:
		std::unordered_map<uint32_t, NodeHandler> Handlers;
	};

	class GraphRunner final : public graph::NodeRunner {
	  public:
		explicit GraphRunner(const NodeTable &table) : Table(table) {}

		bool Run(const graph::RunContext &context) override;

		core::Name Unhandled() const {
			return Missing;
		}

		size_t Submitted() const {
			return SubmittedCount;
		}

	  private:
		const NodeTable &Table;
		core::Name Missing;
		size_t SubmittedCount = 0;
	};
}
