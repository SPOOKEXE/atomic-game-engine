#include <engine/render/Capabilities.hpp>

#include <algorithm>

namespace engine::render {

	CapabilityCheck CheckCapabilities(const DeviceCaps &caps, const graph::NodeRequirements &needs) {
		if (needs.Compute && !caps.HasCompute) {
			return {CapabilityStatus::MissingCompute};
		}
		if (needs.StorageTextures && !caps.HasStorageTextures) {
			return {CapabilityStatus::MissingStorageTextures};
		}
		if (needs.IndirectDraws && !caps.HasIndirectDraws) {
			return {CapabilityStatus::MissingIndirectDraws};
		}
		for (const graph::ResourceFormat format : needs.Formats) {
			if (std::find(caps.Formats.begin(), caps.Formats.end(), format) == caps.Formats.end()) {
				return {CapabilityStatus::MissingFormat, format};
			}
		}
		return {};
	}

	const char *Describe(CapabilityStatus status) {
		switch (status) {
		case CapabilityStatus::Ok:
			return "supported";
		case CapabilityStatus::MissingCompute:
			return "the device has no compute pipeline support";
		case CapabilityStatus::MissingStorageTextures:
			return "the device cannot write storage textures";
		case CapabilityStatus::MissingIndirectDraws:
			return "the device has no indexed indirect draw support";
		case CapabilityStatus::MissingFormat:
			return "the device does not support a required texture format";
		}
		return "unknown device capability failure";
	}

	PipelineTierDecision ChooseDefaultPipeline(const DeviceCaps &caps) {
		PipelineTierDecision choice;
		const auto tryTier = [&](DefaultPipelineTier tier, graph::NodeRequirements needs) {
			const CapabilityCheck result = CheckCapabilities(caps, needs);
			if (result.Accepted()) {
				choice.Tier = tier;
				return true;
			}
			choice.Fallthrough.push_back({tier, result});
			return false;
		};

		graph::NodeRequirements full;
		full.Compute = true;
		full.StorageTextures = true;
		full.IndirectDraws = true;
		full.Formats = {
			graph::ResourceFormat::RGBA8,
			graph::ResourceFormat::RGBA8_SRGB,
			graph::ResourceFormat::RGB10A2,
			graph::ResourceFormat::RGBA16F,
			graph::ResourceFormat::R32F,
			graph::ResourceFormat::D24S8,
			graph::ResourceFormat::D32F,
		};
		if (tryTier(DefaultPipelineTier::A, full)) {
			return choice;
		}

		graph::NodeRequirements deferred = full;
		deferred.Compute = false;
		deferred.StorageTextures = false;
		if (tryTier(DefaultPipelineTier::B, deferred)) {
			return choice;
		}

		graph::NodeRequirements forward;
		forward.Formats = {
			graph::ResourceFormat::RGBA8_SRGB,
			graph::ResourceFormat::RGB10A2,
			graph::ResourceFormat::D24S8,
		};
		(void)tryTier(DefaultPipelineTier::C, forward);
		return choice;
	}

	const char *Describe(DefaultPipelineTier tier) {
		switch (tier) {
		case DefaultPipelineTier::A:
			return "Tier A";
		case DefaultPipelineTier::B:
			return "Tier B";
		case DefaultPipelineTier::C:
			return "Tier C";
		case DefaultPipelineTier::Unavailable:
			return "unavailable";
		}
		return "unknown tier";
	}
}
