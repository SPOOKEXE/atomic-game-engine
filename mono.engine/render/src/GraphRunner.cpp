#include <engine/core/Profiling.hpp>
#include <engine/render/GraphRunner.hpp>

#include <algorithm>
#include <utility>

namespace engine::render {
	namespace {
		// The catalogue currently has 33 built-in handlers. Three spare slots keep
		// the common custom-node case in the same allocation.
		constexpr size_t BUILT_IN_NODE_CAPACITY = 36;
	}

	NodeTable::NodeTable() {
		Handlers.reserve(BUILT_IN_NODE_CAPACITY);
	}

	bool NodeTable::Set(core::Name kind, NodeHandler handler) {
		if (!kind.IsValid() || !handler) {
			return false;
		}
		for (auto &[identifier, registered] : Handlers) {
			if (identifier == kind.Id()) {
				registered = std::move(handler);
				return true;
			}
		}
		Handlers.emplace_back(kind.Id(), std::move(handler));
		return true;
	}

	bool NodeTable::Has(core::Name kind) const {
		return Find(kind) != nullptr;
	}

	const NodeHandler *NodeTable::Find(core::Name kind) const {
		for (const auto &[identifier, handler] : Handlers) {
			if (identifier == kind.Id()) {
				return &handler;
			}
		}
		return nullptr;
	}

	std::vector<core::Name> NodeTable::Missing(const graph::RenderGraph &graph) const {
		std::vector<core::Name> missing;
		for (size_t index = 0; index < graph.Count(); index++) {
			const graph::Node *node = graph.Find(graph::NodeId{static_cast<uint32_t>(index + 1)});
			if (node == nullptr || !node->Enabled || Has(node->Kind)) {
				continue;
			}
			if (std::find(missing.begin(), missing.end(), node->Kind) == missing.end()) {
				missing.push_back(node->Kind);
			}
		}
		std::sort(missing.begin(), missing.end(), [](core::Name left, core::Name right) {
			return left.Text() < right.Text();
		});
		return missing;
	}

	size_t NodeTable::Count() const {
		return Handlers.size();
	}

	void NodeTable::Clear() {
		Handlers.clear();
	}

	bool GraphRunner::Run(const graph::RunContext &context) {
		const NodeHandler *handler = Table.Find(context.Kind);
		if (handler == nullptr) {
			Missing = context.Kind;
			return false;
		}

		const bool profile = Tier != ProfilingTier::Off && (!Profile.Enabled || Profile.Enabled(context));
		const auto submit = [&] {
			if (profile && Profile.Begin) {
				Profile.Begin(context);
			}
			SubmittedCount++;
			const bool accepted = (*handler)(context);
			if (profile && Profile.End) {
				DroppedMarks += Profile.End(context);
			}
			return accepted;
		};

		if (!profile) {
			return submit();
		}

		// **One span per node, here rather than in each handler.** The handlers
		// live in `Renderer.cpp` as two dozen lambdas and most of them had no
		// span at all - `gbuffer`, which records every opaque draw call in the
		// frame, among them. Everything they did therefore landed inside
		// `Renderer::RenderView` with nothing naming it, which is what the wide
		// blanks in the frame graph were. Naming them at the one point they are
		// all called cannot be forgotten by the next handler somebody adds.
		//
		// **The authored node name, not its kind**, because a pipeline with two
		// `raster` nodes is asking which of them is expensive, and the kind
		// would answer with one bar for both. `core::Name` interns for the life
		// of the process, so the stable form is right and nothing is copied.
		ENGINE_PROFILE_DYNAMIC_STABLE("graph node", context.Name.Text(), core::ProfileCategory::Render);
		return submit();
	}
}
