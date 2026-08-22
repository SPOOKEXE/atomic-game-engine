// The transfer stage, and the CPU stages that resolve before it.
//
// **Every one of these is a node the graph reaches, and none of them opens a
// render pass.** The CPU stages have already run - `ViewRecording::Begin`
// culls, filters and orders before any handler is called - so what they do
// here is close their timing span and hand back the wall time the entity
// stage measured for them. See `ViewRecording::FinishCpuNode`.

#include "ViewRecording.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

namespace engine::render {

	void ViewRecording::RegisterUploadNodes(NodeTable &frameNodes) {
		// **The CPU half of the pipeline, named rather than blank.** Frustum
		// culling, distance culling, tag filtering and the draw-order sort all
		// ran in `Begin`; these are the nodes the frame graph attributes them to,
		// and without them those hundreds of microseconds read as a hole at the
		// top of the frame.
		for (const char *kind :
			 {"world", "camera", "entities", "cull-frustum", "cull-distance", "filter-tag", "order-draw"}) {
			frameNodes.Set(core::Name(kind), [this](const graph::RunContext &context) {
				return FinishCpuNode(context);
			});
		}

		frameNodes.Set(core::Name("upload-instances"), [this](const graph::RunContext &context) {
			ViewRecording &recording = *this;
			const auto enterNamedPass = [&recording](
											core::Name name, SDL_GPUCommandBuffer *recordedCommand = nullptr
										) { recording.EnterNamedPass(name, recordedCommand); };
			const auto recordUploads = [&recording] { return recording.RecordUploads(); };

			enterNamedPass(context.Name);
			return recordUploads();
		});
	}
}
