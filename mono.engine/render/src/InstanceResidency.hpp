#pragma once

// Stable host-side slots for device-resident instance rows.
//
// A draw order is rebuilt per view, but a row only moves when its identity
// leaves the view entirely. The GPU consumes a separate uint index stream, so
// culling and sorting change four bytes per draw without rewriting the packed
// transform and colour behind it.

#include "InstancePacking.hpp"

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::render {

	struct InstanceKey {
		core::Name World;
		uint64_t Source = 0;
		uint64_t Variant = 0;
		uint32_t Fallback = 0;

		bool operator==(const InstanceKey &) const = default;
	};

	struct InstanceKeyHash {
		size_t operator()(const InstanceKey &key) const noexcept;
	};

	struct InstanceUploadRange {
		uint32_t First = 0;
		uint32_t Count = 0;
	};

	class InstanceResidency {
	  public:
		void BeginFrame();
		void BeginFrame(uint64_t token);

		uint32_t Upsert(const InstanceKey &key, const GpuInstance &row);

		void EndFrame();

		void MarkAllDirty();

		void AcknowledgeDirty();

		std::span<const InstanceUploadRange> DirtyRanges();

		const GpuInstance &Row(uint32_t slot) const;

		uint32_t SlotCount() const {
			return static_cast<uint32_t>(Entries.size());
		}

		uint32_t LiveCount() const {
			return Live;
		}

		uint32_t DirtyCount() const {
			return static_cast<uint32_t>(Dirty.size());
		}

	  private:
		struct Entry {
			InstanceKey Key;
			GpuInstance Packed;
			uint64_t Seen = 0;
			bool Occupied = false;
			bool Dirty = false;
		};

		void MarkDirty(uint32_t slot);
		void ReleaseUnseen();

		std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> Slots;
		std::vector<Entry> Entries;
		std::vector<uint32_t> Free;
		std::vector<uint32_t> Dirty;
		std::vector<InstanceUploadRange> Ranges;
		uint64_t Frame = 0;
		uint64_t Token = 0;
		uint32_t Live = 0;
		bool TokenMode = false;
	};
}
