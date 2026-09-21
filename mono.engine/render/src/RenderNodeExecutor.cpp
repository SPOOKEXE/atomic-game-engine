// Renderer-owned custom backend handler lifecycle.

#include "RenderNodeExecutor.hpp"

#include "RendererState.hpp"

#include <engine/graph/PipelineCatalogue.hpp>

namespace engine::render {
	bool Renderer::InstallNodeHandler(core::Name kind, NodeHandler handler, NodeHandlerLifecycle lifecycle) {
		RequireOwningThread("InstallNodeHandler");
		const graph::NodeKindSpec *spec = graph::NodeCatalogue::Find(kind);
		if (spec == nullptr || spec->BuiltInBackend || !handler) {
			return false;
		}

		const BackendHandles handles = Backend();
		const bool live = handles.Device != nullptr;
		if (live && lifecycle.Reinstall && !lifecycle.Reinstall(handles)) {
			return false;
		}

		for (InstalledNodeHandler &installed : CustomNodeHandlers) {
			if (installed.Kind != kind) {
				continue;
			}
			if (installed.Live && installed.Lifecycle.Release) {
				installed.Lifecycle.Release(handles);
			}
			installed = InstalledNodeHandler{kind, std::move(handler), std::move(lifecycle), live};
			return true;
		}

		CustomNodeHandlers.push_back(
			InstalledNodeHandler{kind, std::move(handler), std::move(lifecycle), live}
		);
		return true;
	}
}
