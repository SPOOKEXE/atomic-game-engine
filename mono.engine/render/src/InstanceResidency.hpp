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

	// Per-mesh residency and the rows staged in the current renderer frame.
	// The renderer merges one of these from each world for the asset profiler.
	struct AssetInstanceRows {
		core::Name Mesh;
		uint32_t Resident = 0;
		uint32_t Staged = 0;
	};

	class InstanceResidency {
	  public:
		void BeginFrame();
		void BeginFrame(uint64_t token);

		uint32_t Upsert(const InstanceKey &key, const GpuInstance &row);
		uint32_t Upsert(
			const InstanceKey &key,
			const GpuInstance &row,
			const scene::DrawInstance &source,
			const MeshEntry &mesh
		);

		// Reuses a packed slot when every input to `ToGpu` matches exactly.
		//
		// @param key    Stable row identity.
		// @param source Current scene row.
		// @param mesh   Current resolved mesh.
		// @param slot   Receives the resident slot whenever the key exists.
		// @return Whether the packed row is already current.
		bool Reuse(
			const InstanceKey &key, const scene::DrawInstance &source, const MeshEntry &mesh, uint32_t &slot
		);

		bool Probe(
			const InstanceKey &key, const scene::DrawInstance &source, const MeshEntry &mesh, uint32_t &slot
		) const;

		// Probes a caller-retained slot without repeating the stable-key hash.
		bool ProbeSlot(
			uint32_t slot, const InstanceKey &key, const scene::DrawInstance &source, const MeshEntry &mesh
		) const;

		// Updates a known slot, falling back to key lookup when its identity changed.
		uint32_t UpsertSlot(
			uint32_t slot,
			const InstanceKey &key,
			const GpuInstance &row,
			const scene::DrawInstance &source,
			const MeshEntry &mesh
		);

		void Touch(uint32_t slot);

		void EndFrame();

		void MarkAllDirty();

		void AcknowledgeDirty();

		std::span<const InstanceUploadRange> DirtyRanges();

		const GpuInstance &Row(uint32_t slot) const;

		std::span<const GpuInstance> PackedRows() const {
			return Packed;
		}

		uint32_t SlotCount() const {
			return static_cast<uint32_t>(Entries.size());
		}

		uint32_t LiveCount() const {
			return Live;
		}

		uint32_t DirtyCount() const {
			return static_cast<uint32_t>(Dirty.size());
		}

		std::vector<AssetInstanceRows> AssetRows() const;

	  private:
		// The contiguous prefix of DrawInstance consumed by ToGpu. The layout
		// assertion in InstanceResidency.cpp keeps the bulk comparison honest.
		struct PackingSource {
			core::CFrame Frame;
			core::Vector3 HalfExtent;
			core::Color3 Tint;
			core::Color3 SurfaceColour;
			core::Color3 EmissiveTint;
			float EmissiveStrength = 1.0f;
		};

		struct Entry {
			InstanceKey Key;
			PackingSource Source;
			float Transparency = 0.0f;
			float AlphaCutoff = 0.5f;
			core::Vector3 MeshCentre;
			core::Vector3 MeshExtent;
			uint64_t Seen = 0;
			scene::AlphaMode Alpha = scene::AlphaMode::Opaque;
			scene::SurfaceResampleMode Resample = scene::SurfaceResampleMode::Default;
			bool Occupied = false;
			bool Dirty = false;
			bool SourceKnown = false;
			core::Name Mesh;
		};

		uint32_t Upsert(
			const InstanceKey &key,
			const GpuInstance &row,
			const scene::DrawInstance *source,
			const MeshEntry *mesh
		);
		bool
		SourceCurrent(const Entry &entry, const scene::DrawInstance &source, const MeshEntry &mesh) const;
		uint32_t UpdateSlot(
			uint32_t slot, const GpuInstance &row, const scene::DrawInstance *source, const MeshEntry *mesh
		);
		void MarkDirty(uint32_t slot);
		void ReleaseUnseen();

		std::unordered_map<InstanceKey, uint32_t, InstanceKeyHash> Slots;
		std::vector<Entry> Entries;
		std::vector<GpuInstance> Packed;
		std::vector<uint32_t> Free;
		std::vector<uint32_t> Dirty;
		std::vector<InstanceUploadRange> Ranges;
		std::unordered_map<uint32_t, uint32_t> StagedByMesh;
		uint64_t Frame = 0;
		uint64_t Token = 0;
		uint32_t Live = 0;
		bool TokenMode = false;
	};
}
