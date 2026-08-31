#include "CustomNode.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/render/Renderer.hpp>

#include <utility>

namespace engine::render::examples {

	std::shared_ptr<CustomNodeState> InstallCustomNode(Renderer &renderer) {
		const core::Name kind("example-marker");
		graph::NodeKindSpec spec;
		spec.Kind = kind;
		spec.Label = "Example marker";
		spec.Summary = "Runs native game code at a frame graph boundary.";
		spec.Category = graph::NodeCategory::Output;
		spec.Scope = graph::NodeScope::Frame;
		spec.Queue = graph::ExecutionQueue::Cpu;
		spec.Repeatable = true;
		spec.Inputs.push_back(
			graph::PortSpec{
				.Name = core::Name("image"),
				.Kind = graph::ResourceKind::Texture,
				.Format = graph::ResourceFormat::RGBA8,
				.Required = true,
				.Summary = "The completed image whose boundary is being observed.",
			}
		);
		if (!graph::RegisterNodeKind(std::move(spec))) {
			return {};
		}

		auto state = std::make_shared<CustomNodeState>();
		NodeHandlerLifecycle lifecycle;
		lifecycle.Reinstall = [state](BackendHandles handles) {
			state->DeviceReady = handles.Device != nullptr;
			return state->DeviceReady;
		};
		lifecycle.Release = [state](BackendHandles) { state->DeviceReady = false; };

		if (!renderer.InstallNodeHandler(
				kind,
				[state](const graph::RunContext &) {
					state->Executions++;
					return true;
				},
				std::move(lifecycle)
			)) {
			return {};
		}
		return state;
	}
}
