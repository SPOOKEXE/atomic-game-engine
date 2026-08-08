#include <engine/render/GraphRunner.hpp>

#include <algorithm>

namespace engine::render {

	bool PassTable::Set(core::Name kind, PassHandler handler) {
		if (!kind.IsValid() || !handler) {
			return false;
		}
		Handlers[kind.Id()] = std::move(handler);
		return true;
	}

	bool PassTable::Has(core::Name kind) const {
		return Find(kind) != nullptr;
	}

	const PassHandler *PassTable::Find(core::Name kind) const {
		const auto found = Handlers.find(kind.Id());
		return found == Handlers.end() ? nullptr : &found->second;
	}

	void PassTable::Clear() {
		Handlers.clear();
	}

	std::vector<core::Name> PassTable::Missing(const graph::RenderGraph &graph) const {
		std::vector<core::Name> missing;

		for (size_t index = 0; index < graph.Count(); index++) {
			const graph::Node *node = graph.Find(graph::NodeId{static_cast<uint32_t>(index + 1)});

			// **Disabled nodes are not missing.** A pass somebody switched off
			// is not one the renderer cannot draw, and listing it would make the
			// answer to "what can this renderer not do" depend on what happened
			// to be turned on.
			if (node == nullptr || !node->Enabled || Has(node->Kind)) {
				continue;
			}
			if (std::find(missing.begin(), missing.end(), node->Kind) == missing.end()) {
				missing.push_back(node->Kind);
			}
		}

		// Sorted, so a diagnostic reads the same way twice and a test can assert
		// on the list rather than on a set.
		std::sort(missing.begin(), missing.end(), [](core::Name a, core::Name b) {
			return a.Text() < b.Text();
		});
		return missing;
	}

	bool GraphRunner::Run(const graph::RunContext &context) {
		const PassHandler *handler = Table.Find(context.Kind);
		if (handler == nullptr) {
			// **Recorded before returning**, so a caller has something to name.
			// See `Unhandled`: a bare `false` makes "nothing knows how to do
			// this" and "the device refused" look identical.
			Missing_ = context.Kind;
			return false;
		}

		Count_++;
		return (*handler)(context);
	}
}
