#pragma once

// Device features used to accept or refuse a render graph.
//
// This is deliberately free of SDL types. The adapter probes SDL once during
// renderer initialisation, then graph validation and Studio read this immutable
// value without reaching back into the device.
//
// @tier L12 · client

#include <engine/graph/PipelineCatalogue.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::render {

	// The portable capabilities of one initialised rendering device.
	//
	// @since v0.20
	struct DeviceCaps {
		// Probed device features and backend shape.
		//@{
		bool HasCompute = false;
		bool HasStorageTextures = false;
		bool HasIndirectDraws = false;
		bool HasTimestamps = false;
		bool UnifiedQueue = true;
		bool PrefersMSL = false;
		//@}

		// Portable limits used by built-in pipelines.
		//@{
		uint32_t MaxSamplersPerDraw = 0;
		uint32_t MaxColourTargets = 0;
		//@}

		// Graph formats supported for their ordinary sampled target use.
		std::vector<graph::ResourceFormat> Formats;
	};

	// Why one node's requirements cannot run on a device.
	//
	// @since v0.20
	enum class CapabilityStatus : uint8_t {
		Ok,
		MissingCompute,
		MissingStorageTextures,
		MissingIndirectDraws,
		MissingFormat,
	};

	// The result of checking one node kind against a device.
	//
	// Format is meaningful only for MissingFormat.
	//
	// @since v0.20
	struct CapabilityCheck {
		// First missing feature and its associated format when applicable.
		//@{
		CapabilityStatus Status = CapabilityStatus::Ok;
		graph::ResourceFormat Format = graph::ResourceFormat::RGBA8;
		//@}

		// Whether the node may run.
		bool Accepted() const {
			return Status == CapabilityStatus::Ok;
		}
	};

	// Checks all mandatory requirements of one node kind.
	//
	// TimestampsUseful is advisory and never causes refusal.
	//
	// @param caps The immutable device capability snapshot.
	// @param needs The catalogue declaration to check.
	// @return The first missing mandatory feature.
	CapabilityCheck CheckCapabilities(const DeviceCaps &caps, const graph::NodeRequirements &needs);

	// A stable refusal phrase for a capability result.
	//
	// @param status The status to describe.
	// @return A process-lifetime string.
	const char *Describe(CapabilityStatus status);

	// The built-in document family, ordered from full to reduced.
	enum class DefaultPipelineTier : uint8_t { A, B, C, Unavailable };

	// Why one richer built-in document was skipped.
	struct PipelineTierRejection {
		// Tier considered and its first unsupported requirement.
		//@{
		DefaultPipelineTier Tier = DefaultPipelineTier::A;
		CapabilityCheck Cause{};
		//@}
	};

	// The capability-driven built-in document choice.
	struct PipelineTierDecision {
		// Selected tier and richer tiers rejected before it.
		//@{
		DefaultPipelineTier Tier = DefaultPipelineTier::Unavailable;
		std::vector<PipelineTierRejection> Fallthrough;
		//@}
	};

	// Chooses the richest built-in document the device can execute.
	PipelineTierDecision ChooseDefaultPipeline(const DeviceCaps &caps);

	// Names a built-in tier for diagnostics and tests.
	const char *Describe(DefaultPipelineTier tier);
}
