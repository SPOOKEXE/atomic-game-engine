#pragma once

// The immutable pipeline package and diagnostics produced at admission.

#include <engine/core/Name.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Capabilities.hpp>
#include <engine/render/PipelineAdmission.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::render {
	// Built-in handler families represented in the retained-node plan.
	enum RetainedNodeFamily : uint16_t {
		RetainedUpload = 1u << 0,
		RetainedShadow = 1u << 1,
		RetainedMirror = 1u << 2,
		RetainedPortal = 1u << 3,
		RetainedSurface = 1u << 4,
		RetainedAuthored = 1u << 5,
		RetainedShading = 1u << 6,
		RetainedGeometry = 1u << 7,
	};

	// One complete renderer execution package. Compile before replacing a registry entry.
	struct InstalledPipeline {
		core::Name Name;
		graph::RenderGraph Graph;
		graph::CompiledGraph Compiled;
		std::vector<graph::NodeId> EntityNodes;
		// Nodes that must still execute while scene inputs are retained. This
		// includes output/custom nodes and the forward closure from history.
		std::vector<uint8_t> RetainedNodes;
		uint16_t RetainedFamilies = 0;
		graph::ExecutionSchedule Schedule;
		graph::ResourceAliasPlan Aliases;
		// SDL currently submits these buffers in order on one unified queue.
		std::vector<graph::PlannedCommandBuffer> Buffers;
		uint64_t Revision = 0;
	};

	struct PipelineCompilation {
		std::optional<InstalledPipeline> Package;
		PipelineFailure Failure;

		explicit operator bool() const {
			return Package.has_value();
		}
	};

	PipelineCompilation CompilePipeline(
		core::Name name,
		const graph::RenderGraph &pipeline,
		const DeviceCaps *caps,
		std::span<const core::Name> customKinds
	);

}
