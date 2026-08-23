#pragma once

// Per-camera draw-order residency across the device's in-flight frame window.
//
// Instance payloads live in stable world slots. A view consumes only this uint
// whitelist, so keeping one acknowledged copy per device buffer lets a still
// camera submit no index traffic and a membership edit copy only the changed
// entries.

#include "InstanceResidency.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::render {

	class IndexResidency {
	  public:
		static constexpr uint32_t VERSIONS = 3;

		void Plan(uint32_t version, std::span<const uint32_t> indices, bool bufferReplaced);
		void Acknowledge();

		std::span<const InstanceUploadRange> DirtyRanges() const {
			return Ranges;
		}

		uint32_t DirtyCount() const {
			return Dirty;
		}

		uint32_t Version() const {
			return PendingVersion;
		}

	  private:
		struct VersionState {
			std::vector<uint32_t> Indices;
			bool Initialised = false;
		};
		struct RangeScratch {
			std::vector<InstanceUploadRange> Ranges;
		};

		void AddRange(uint32_t first, uint32_t count);

		std::array<VersionState, VERSIONS> States;
		std::vector<RangeScratch> Chunks;
		std::vector<InstanceUploadRange> Ranges;
		std::span<const uint32_t> Pending;
		uint32_t PendingVersion = 0;
		uint32_t Dirty = 0;
	};
}
