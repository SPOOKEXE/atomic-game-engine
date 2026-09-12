#include "GpuHeap.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/render/MeshTable.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::render {

	namespace {
		uint64_t MeshKey(const core::Name &name, core::Name owner) {
			return (uint64_t(owner.Id()) << 32) | name.Id();
		}

		size_t PackedValueBytes(PackedMeshFormat format, size_t values) {
			switch (format) {
			case PackedMeshFormat::Float32:
				return values * 4;
			case PackedMeshFormat::Float16:
			case PackedMeshFormat::Signed16:
			case PackedMeshFormat::Unsigned16:
				return values * 2;
			case PackedMeshFormat::Float8E4M3FN:
			case PackedMeshFormat::Signed8:
			case PackedMeshFormat::Unsigned8:
				return values;
			case PackedMeshFormat::Signed4:
			case PackedMeshFormat::Unsigned4:
				return (values + 1) / 2;
			case PackedMeshFormat::Boolean:
				return (values + 7) / 8;
			}
			return 0;
		}

		constexpr uint32_t CLUSTER_TRIANGLES = 64;

		float TriangleArea(const assets::MeshData &mesh, uint32_t first) {
			const auto &a = mesh.Vertices[mesh.Indices[first]].Position;
			const auto &b = mesh.Vertices[mesh.Indices[first + 1]].Position;
			const auto &c = mesh.Vertices[mesh.Indices[first + 2]].Position;
			const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
			const float acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
			const float x = aby * acz - abz * acy;
			const float y = abz * acx - abx * acz;
			const float z = abx * acy - aby * acx;
			return 0.5f * std::sqrt(x * x + y * y + z * z);
		}

		void AddClusters(MeshEntry &entry, const assets::MeshData &mesh) {
			const auto addRange = [&](uint32_t first, uint32_t count, uint32_t material) {
				for (uint32_t offset = 0; offset < count; offset += CLUSTER_TRIANGLES * 3u) {
					const uint32_t indices = std::min(count - offset, CLUSTER_TRIANGLES * 3u);
					const float *const firstPoint = mesh.Vertices[mesh.Indices[first + offset]].Position;
					core::Vector3 minimum{firstPoint[0], firstPoint[1], firstPoint[2]};
					core::Vector3 maximum = minimum;
					float area = 0.0f;
					for (uint32_t index = first + offset; index < first + offset + indices; ++index) {
						const float *const source = mesh.Vertices[mesh.Indices[index]].Position;
						const core::Vector3 point{source[0], source[1], source[2]};
						minimum.X = std::min(minimum.X, point.X);
						minimum.Y = std::min(minimum.Y, point.Y);
						minimum.Z = std::min(minimum.Z, point.Z);
						maximum.X = std::max(maximum.X, point.X);
						maximum.Y = std::max(maximum.Y, point.Y);
						maximum.Z = std::max(maximum.Z, point.Z);
					}
					for (uint32_t index = first + offset; index < first + offset + indices; index += 3) {
						area += TriangleArea(mesh, index);
					}
					entry.Clusters.push_back({
						{entry.Whole.FirstIndex + first + offset, indices, entry.Whole.VertexOffset},
						(minimum + maximum) * 0.5f,
						(maximum - minimum) * 0.5f,
						area,
						material,
					});
				}
			};
			if (mesh.Submeshes.empty()) {
				addRange(0, static_cast<uint32_t>(mesh.Indices.size()), std::numeric_limits<uint32_t>::max());
				return;
			}
			for (uint32_t material = 0; material < mesh.Submeshes.size(); material++) {
				const assets::Submesh &run = mesh.Submeshes[material];
				addRange(run.FirstIndex, run.IndexCount, material);
			}
		}
	}

	bool PackedMeshData::IsValid() const {
		if (VertexCount == 0 || Indices.empty() || Indices.size() % 3 != 0 || Vertices.empty() ||
			Vertices.size() % 4 != 0)
			return false;
		size_t previousEnd = 0;
		for (size_t index = 0; index < Streams.size(); ++index) {
			const PackedMeshStream &stream = Streams[index];
			const uint32_t components = index == 2 ? 2u : 3u;
			if (static_cast<uint32_t>(stream.Format) > static_cast<uint32_t>(PackedMeshFormat::Boolean) ||
				stream.Components != components || stream.ValueCount != VertexCount * components ||
				stream.ByteOffset % 4 != 0 ||
				stream.ByteCount != PackedValueBytes(stream.Format, stream.ValueCount) ||
				static_cast<size_t>(stream.ByteOffset) + stream.ByteCount > Vertices.size() ||
				!std::isfinite(stream.Minimum) || !std::isfinite(stream.Maximum) ||
				stream.Maximum < stream.Minimum)
				return false;
			if (stream.ByteOffset < previousEnd) return false;
			previousEnd = static_cast<size_t>(stream.ByteOffset) + stream.ByteCount;
		}
		for (const uint32_t index : Indices)
			if (index >= VertexCount) return false;
		for (const assets::Submesh &submesh : Submeshes)
			if (submesh.IndexCount % 3 != 0 ||
				static_cast<size_t>(submesh.FirstIndex) + submesh.IndexCount > Indices.size())
				return false;
		return std::isfinite(Minimum.X) && std::isfinite(Minimum.Y) && std::isfinite(Minimum.Z) &&
			   std::isfinite(Maximum.X) && std::isfinite(Maximum.Y) && std::isfinite(Maximum.Z);
	}

	MeshTable::~MeshTable() {
		Shutdown();
	}

	bool MeshTable::Initialise(SDL_GPUDevice *device) {
		Device = device;
		if (Device == nullptr) {
			return false;
		}

		// Register all built-ins before content arrives.
		for (uint8_t index = 0; index < assets::BUILTIN_MESH_COUNT; index++) {
			const auto builtin = static_cast<assets::BuiltinMesh>(index);
			if (!Add(core::Name(assets::BuiltinName(builtin)), assets::MakeBuiltin(builtin))) {
				ENGINE_ERROR("mesh table: built-in {} was refused", assets::BuiltinName(builtin));
				return false;
			}
		}

		if (!Flush()) {
			return false;
		}

		// Resolve the fallback once; the draw loop never hashes unknown names.
		const auto found =
			Entries.find(MeshKey(core::Name(assets::BuiltinName(assets::BuiltinMesh::Cube)), {}));
		if (found == Entries.end()) {
			return false;
		}
		Fallback = found->second;
		return true;
	}

	void MeshTable::Shutdown() {
		if (Device != nullptr) {
			if (VertexBuffer != nullptr) {
				gpu::ReleaseBuffer(Device, VertexBuffer);
			}
			if (IndexBuffer != nullptr) {
				gpu::ReleaseBuffer(Device, IndexBuffer);
			}
			if (PackedVertexBuffer != nullptr) gpu::ReleaseBuffer(Device, PackedVertexBuffer);
		}

		VertexBuffer = nullptr;
		IndexBuffer = nullptr;
		PackedVertexBuffer = nullptr;
		VertexCapacity = 0;
		IndexCapacity = 0;
		PackedCapacity = 0;
		PackedUploadBytes = 0;
		Uploads = 0;
		Generation = 0;
		FreeVertices.clear();
		FreeIndices.clear();
		FreePackedBytes.clear();
		DirtyVertices.clear();
		DirtyIndices.clear();
		DirtyPackedBytes.clear();
		HostVertices.clear();
		HostIndices.clear();
		HostPackedVertices.clear();
		Entries.clear();
		Fallback = MeshEntry{};
		Device = nullptr;
		Dirty = false;
	}

	size_t MeshTable::Claim(std::vector<FreeBlock> &blocks, size_t count, size_t generation) {
		if (count == 0) {
			return NOWHERE;
		}

		for (size_t at = 0; at < blocks.size(); at++) {
			FreeBlock &block = blocks[at];
			if (block.Count < count) {
				continue;
			}

			// **Old enough, not merely big enough.** A run freed this frame may
			// still be under a draw the device has not finished; see
			// `DEFERRED_FRAMES`. Skipped rather than refused, because a younger
			// block sitting at the front must not hide an older one behind it.
			if (generation < block.FreedAt + DEFERRED_FRAMES) {
				continue;
			}

			const size_t offset = block.Offset;
			if (block.Count == count) {
				blocks.erase(blocks.begin() + static_cast<ptrdiff_t>(at));
			} else {
				// The remainder keeps the block's age: it has been free at
				// least as long as the part just taken from it.
				block.Offset += count;
				block.Count -= count;
			}
			return offset;
		}

		return NOWHERE;
	}

	void MeshTable::Release(std::vector<FreeBlock> &blocks, size_t offset, size_t count, size_t generation) {
		if (count == 0) {
			return;
		}

		const auto after =
			std::lower_bound(blocks.begin(), blocks.end(), offset, [](const FreeBlock &block, size_t at) {
				return block.Offset < at;
			});
		const auto inserted = blocks.insert(after, FreeBlock{offset, count, generation});
		const size_t at = static_cast<size_t>(inserted - blocks.begin());

		// **Merged with the block after it first, then the one before**, so a
		// run that closes a gap between two free blocks becomes one block rather
		// than three. Doing the far side first keeps the index of the near one
		// valid.
		//
		// **Only blocks freed on the same frame merge, and that restriction is
		// load-bearing.** A merged block can only be handed out when every part
		// of it is old enough, so merging two different ages means taking the
		// newer - and a mesh rewritten every frame frees a run beside its own
		// last one every frame, which would keep resetting the age of one
		// ever-growing block and reclaim nothing at all. That is the exact bug
		// this whole change exists to remove, reintroduced by the tidying meant
		// to help it.
		//
		// What is given up is merging neighbours of different ages. They stay
		// separate blocks and each becomes reusable on its own, which costs a
		// larger mesh the chance to span them and costs nothing else. Runs freed
		// together - a chunk replacing several meshes at once - still merge,
		// which is the case that fragments.
		if (at + 1 < blocks.size() && blocks[at].Offset + blocks[at].Count == blocks[at + 1].Offset &&
			blocks[at].FreedAt == blocks[at + 1].FreedAt) {
			blocks[at].Count += blocks[at + 1].Count;
			blocks.erase(blocks.begin() + static_cast<ptrdiff_t>(at) + 1);
		}
		if (at > 0 && blocks[at - 1].Offset + blocks[at - 1].Count == blocks[at].Offset &&
			blocks[at - 1].FreedAt == blocks[at].FreedAt) {
			blocks[at - 1].Count += blocks[at].Count;
			blocks.erase(blocks.begin() + static_cast<ptrdiff_t>(at));
		}
	}

	void MeshTable::MarkDirty(std::vector<Span> &spans, size_t offset, size_t count) {
		if (count == 0) {
			return;
		}

		const auto after =
			std::lower_bound(spans.begin(), spans.end(), offset, [](const Span &span, size_t at) {
				return span.Offset < at;
			});
		const auto inserted = spans.insert(after, Span{offset, count});
		size_t at = static_cast<size_t>(inserted - spans.begin());

		// Touching or overlapping spans become one, so an append that lands
		// against the previous one is a single copy rather than two.
		if (at > 0 && spans[at - 1].Offset + spans[at - 1].Count >= spans[at].Offset) {
			at--;
		}
		while (at + 1 < spans.size() && spans[at].Offset + spans[at].Count >= spans[at + 1].Offset) {
			const size_t end =
				std::max(spans[at].Offset + spans[at].Count, spans[at + 1].Offset + spans[at + 1].Count);
			spans[at].Count = end - spans[at].Offset;
			spans.erase(spans.begin() + static_cast<ptrdiff_t>(at) + 1);
		}
	}

	size_t MeshTable::Total(const std::vector<Span> &spans) {
		size_t total = 0;
		for (const Span &span : spans) {
			total += span.Count;
		}
		return total;
	}

	size_t MeshTable::FreeVertexCount() const {
		size_t total = 0;
		for (const FreeBlock &block : FreeVertices) {
			total += block.Count;
		}
		return total;
	}

	size_t MeshTable::FreeIndexCount() const {
		size_t total = 0;
		for (const FreeBlock &block : FreeIndices) {
			total += block.Count;
		}
		return total;
	}

	bool MeshTable::Add(const core::Name &name, const assets::MeshData &mesh, core::Name owner) {
		if (!name.IsValid() || !mesh.IsValid()) {
			return false;
		}

		// **The outgoing entry's storage is given back before the new one asks
		// for storage**, so a mesh rewritten at the same size takes its own run
		// back rather than growing the table. It cannot take it back *this*
		// frame - `Claim` refuses a run younger than `DEFERRED_FRAMES` - which
		// is what keeps the range a frame in flight is drawing from intact.
		if (const auto outgoing = Entries.find(MeshKey(name, owner)); outgoing != Entries.end()) {
			if (outgoing->second.Packed)
				Release(
					FreePackedBytes,
					outgoing->second.PackedByteOffset,
					outgoing->second.PackedByteCount,
					Generation
				);
			else
				Release(
					FreeVertices,
					static_cast<size_t>(outgoing->second.Whole.VertexOffset),
					outgoing->second.VertexCount,
					Generation
				);
			Release(
				FreeIndices, outgoing->second.Whole.FirstIndex, outgoing->second.Whole.IndexCount, Generation
			);
		}

		// Reused where a run will have it, appended where none will. Both are
		// claimed before anything is written, so a table that cannot hold the
		// mesh is refused without having half-taken it.
		size_t vertexAt = Claim(FreeVertices, mesh.Vertices.size(), Generation);
		size_t indexAt = Claim(FreeIndices, mesh.Indices.size(), Generation);

		const bool growVertices = vertexAt == NOWHERE;
		const bool growIndices = indexAt == NOWHERE;

		if ((growVertices && HostVertices.size() + mesh.Vertices.size() > MAXIMUM_VERTICES) ||
			(growIndices && HostIndices.size() + mesh.Indices.size() > MAXIMUM_INDICES)) {
			// **Whatever was claimed goes straight back**, or a refusal leaks
			// the run it had already taken.
			if (!growVertices) {
				Release(FreeVertices, vertexAt, mesh.Vertices.size(), Generation);
			}
			if (!growIndices) {
				Release(FreeIndices, indexAt, mesh.Indices.size(), Generation);
			}
			ENGINE_WARN("mesh table: full, refusing {}", name.Text());
			return false;
		}

		if (growVertices) {
			vertexAt = HostVertices.size();
			HostVertices.resize(vertexAt + mesh.Vertices.size());
		}
		if (growIndices) {
			indexAt = HostIndices.size();
			HostIndices.resize(indexAt + mesh.Indices.size());
		}

		MeshEntry entry;

		// **The mesh's own box, which is what turns `Size` into a size.**
		// `Mesh::Read` derives these from the vertices - nothing on disk states
		// them - so they are the true bounds and a file that lied about its own
		// could not make a part cull wrong. See `MeshEntry::Extent`.
		entry.Centre = (mesh.Minimum + mesh.Maximum) * 0.5f;
		entry.Extent = (mesh.Maximum - mesh.Minimum) * 0.5f;

		entry.Whole.FirstIndex = static_cast<uint32_t>(indexAt);
		entry.Whole.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
		entry.Whole.VertexOffset = static_cast<int32_t>(vertexAt);
		entry.VertexCount = static_cast<uint32_t>(mesh.Vertices.size());
		entry.JointCount = mesh.JointCount;

		for (const assets::Submesh &submesh : mesh.Submeshes) {
			MeshRange run;
			run.FirstIndex = entry.Whole.FirstIndex + submesh.FirstIndex;
			run.IndexCount = submesh.IndexCount;
			run.VertexOffset = entry.Whole.VertexOffset;
			entry.Runs.push_back(run);

			// Intern asset names at the renderer boundary.
			entry.Textures.push_back(submesh.Texture.empty() ? core::Name() : core::Name(submesh.Texture));
			entry.Colours.push_back({
				submesh.BaseColour[0],
				submesh.BaseColour[1],
				submesh.BaseColour[2],
				submesh.BaseColour[3],
			});
		}
		AddClusters(entry, mesh);

		std::copy(
			mesh.Vertices.begin(),
			mesh.Vertices.end(),
			HostVertices.begin() + static_cast<ptrdiff_t>(vertexAt)
		);
		std::copy(
			mesh.Indices.begin(), mesh.Indices.end(), HostIndices.begin() + static_cast<ptrdiff_t>(indexAt)
		);

		MarkDirty(DirtyVertices, vertexAt, mesh.Vertices.size());
		MarkDirty(DirtyIndices, indexAt, mesh.Indices.size());

		Entries[MeshKey(name, owner)] = std::move(entry);
		Dirty = true;
		return true;
	}

	bool MeshTable::AddPacked(const core::Name &name, const PackedMeshData &mesh, core::Name owner) {
		if (!name.IsValid() || !mesh.IsValid()) return false;

		if (const auto outgoing = Entries.find(MeshKey(name, owner)); outgoing != Entries.end()) {
			if (outgoing->second.Packed)
				Release(
					FreePackedBytes,
					outgoing->second.PackedByteOffset,
					outgoing->second.PackedByteCount,
					Generation
				);
			else
				Release(
					FreeVertices,
					static_cast<size_t>(outgoing->second.Whole.VertexOffset),
					outgoing->second.VertexCount,
					Generation
				);
			Release(
				FreeIndices, outgoing->second.Whole.FirstIndex, outgoing->second.Whole.IndexCount, Generation
			);
		}

		size_t packedAt = Claim(FreePackedBytes, mesh.Vertices.size(), Generation);
		size_t indexAt = Claim(FreeIndices, mesh.Indices.size(), Generation);
		const bool growPacked = packedAt == NOWHERE;
		const bool growIndices = indexAt == NOWHERE;
		constexpr size_t maximumPackedBytes = MAXIMUM_VERTICES * sizeof(assets::MeshVertex);
		if ((growPacked && HostPackedVertices.size() + mesh.Vertices.size() > maximumPackedBytes) ||
			(growIndices && HostIndices.size() + mesh.Indices.size() > MAXIMUM_INDICES)) {
			if (!growPacked) Release(FreePackedBytes, packedAt, mesh.Vertices.size(), Generation);
			if (!growIndices) Release(FreeIndices, indexAt, mesh.Indices.size(), Generation);
			return false;
		}
		if (growPacked) {
			packedAt = HostPackedVertices.size();
			HostPackedVertices.resize(packedAt + mesh.Vertices.size());
		}
		if (growIndices) {
			indexAt = HostIndices.size();
			HostIndices.resize(indexAt + mesh.Indices.size());
		}

		MeshEntry entry;
		entry.Centre = (mesh.Minimum + mesh.Maximum) * 0.5f;
		entry.Extent = (mesh.Maximum - mesh.Minimum) * 0.5f;
		entry.Whole.FirstIndex = static_cast<uint32_t>(indexAt);
		entry.Whole.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
		entry.Whole.VertexOffset = 0;
		entry.VertexCount = mesh.VertexCount;
		entry.Packed = true;
		entry.PackedByteOffset = static_cast<uint32_t>(packedAt);
		entry.PackedByteCount = static_cast<uint32_t>(mesh.Vertices.size());
		entry.PackedStreams = mesh.Streams;
		for (PackedMeshStream &stream : entry.PackedStreams)
			stream.ByteOffset += entry.PackedByteOffset;

		for (const assets::Submesh &submesh : mesh.Submeshes) {
			entry.Runs.push_back({
				entry.Whole.FirstIndex + submesh.FirstIndex,
				submesh.IndexCount,
				0,
			});
			entry.Textures.push_back(submesh.Texture.empty() ? core::Name() : core::Name(submesh.Texture));
			entry.Colours.push_back({
				submesh.BaseColour[0],
				submesh.BaseColour[1],
				submesh.BaseColour[2],
				submesh.BaseColour[3],
			});
		}
		std::copy(mesh.Vertices.begin(), mesh.Vertices.end(), HostPackedVertices.begin() + packedAt);
		std::copy(mesh.Indices.begin(), mesh.Indices.end(), HostIndices.begin() + indexAt);
		MarkDirty(DirtyPackedBytes, packedAt, mesh.Vertices.size());
		MarkDirty(DirtyIndices, indexAt, mesh.Indices.size());
		Entries[MeshKey(name, owner)] = std::move(entry);
		Dirty = true;
		return true;
	}

	bool MeshTable::Flush() {
		// **Counted before the early return, because a quiet frame is still a
		// frame.** This is what `Claim` measures a freed run's age in, and a
		// counter that only moved when something was sent would leave a table
		// nobody is writing to unable to ever reuse anything.
		Generation++;
		if (!Dirty) return true;
		if (Device == nullptr) return false;
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		if (command == nullptr) {
			ENGINE_ERROR("mesh table: acquire command buffer: {}", SDL_GetError());
			return false;
		}
		if (!Upload(command)) {
			SDL_CancelGPUCommandBuffer(command);
			return false;
		}
		SDL_SubmitGPUCommandBuffer(command);
		return true;
	}

	bool MeshTable::Record(SDL_GPUCommandBuffer *command) {
		Generation++;
		if (!Dirty) return true;
		return Upload(command);
	}

	bool MeshTable::Upload(SDL_GPUCommandBuffer *command) {
		if (Device == nullptr || command == nullptr || HostVertices.empty() || HostIndices.empty()) {
			return false;
		}

		// Grow geometrically to avoid recreating buffers for every arrival.
		//
		// **A growth is the one case that re-sends everything.** The new buffers
		// are empty, so the tail alone would leave the meshes already registered
		// pointing at nothing.
		bool recreated = false;
		bool packedRecreated = false;
		if (VertexBuffer == nullptr || HostVertices.size() > VertexCapacity ||
			HostIndices.size() > IndexCapacity) {
			size_t vertices = VertexCapacity == 0 ? HostVertices.size() : VertexCapacity;
			while (vertices < HostVertices.size()) {
				vertices *= 2;
			}
			size_t indices = IndexCapacity == 0 ? HostIndices.size() : IndexCapacity;
			while (indices < HostIndices.size()) {
				indices *= 2;
			}

			if (VertexBuffer != nullptr) {
				gpu::ReleaseBuffer(Device, VertexBuffer);
			}
			if (IndexBuffer != nullptr) {
				gpu::ReleaseBuffer(Device, IndexBuffer);
			}

			SDL_GPUBufferCreateInfo vertexInfo{};
			vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
			vertexInfo.size = static_cast<uint32_t>(vertices * sizeof(assets::MeshVertex));
			VertexBuffer = gpu::CreateBuffer(Device, &vertexInfo);

			SDL_GPUBufferCreateInfo indexInfo{};
			indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
			indexInfo.size = static_cast<uint32_t>(indices * sizeof(uint32_t));
			IndexBuffer = gpu::CreateBuffer(Device, &indexInfo);

			if (VertexBuffer == nullptr || IndexBuffer == nullptr) {
				ENGINE_ERROR("mesh table: buffers: {}", SDL_GetError());
				VertexCapacity = 0;
				IndexCapacity = 0;
				return false;
			}

			VertexCapacity = vertices;
			IndexCapacity = indices;
			recreated = true;
		}
		if (!HostPackedVertices.empty() &&
			(PackedVertexBuffer == nullptr || HostPackedVertices.size() > PackedCapacity)) {
			size_t bytes = PackedCapacity == 0 ? HostPackedVertices.size() : PackedCapacity;
			while (bytes < HostPackedVertices.size())
				bytes *= 2;
			if (PackedVertexBuffer != nullptr) gpu::ReleaseBuffer(Device, PackedVertexBuffer);
			SDL_GPUBufferCreateInfo packedInfo{};
			packedInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			packedInfo.size = static_cast<uint32_t>(bytes);
			PackedVertexBuffer = gpu::CreateBuffer(Device, &packedInfo);
			if (PackedVertexBuffer == nullptr) {
				PackedCapacity = 0;
				return false;
			}
			PackedCapacity = bytes;
			core::Metrics::SetGauge("render.mesh.packed_resident_bytes", static_cast<double>(bytes));
			packedRecreated = true;
		}

		// **Only the runs `Add` wrote since the last upload.** Everything else
		// is already on the device and identical. Re-sending it made registering
		// N meshes cost O(N^2) bytes of memcpy and PCIe traffic, and a table near
		// its eight-million-vertex ceiling made that a 384 MB transfer for one
		// arriving mesh. That is the stall a game shows as a freeze while its
		// meshes load.
		//
		// **A list of runs rather than the single high-water mark this kept.**
		// A mark describes a delta only while every write is an append; a
		// replacement that reuses a freed run writes into the middle, and one
		// line cannot say so. The runs are coalesced as they are recorded, so
		// the ordinary case - a burst of arrivals landing end to end - is still
		// exactly one region.
		// **A recreate sends everything, because the new buffers hold nothing.**
		// Anything the spans did not name is on the old buffer, which has just
		// been released.
		if (recreated) {
			DirtyVertices.assign(1, Span{0, HostVertices.size()});
			DirtyIndices.assign(1, Span{0, HostIndices.size()});
		}
		if (packedRecreated) DirtyPackedBytes.assign(1, Span{0, HostPackedVertices.size()});

		const size_t vertexCount = Total(DirtyVertices);
		const size_t indexCount = Total(DirtyIndices);
		const size_t packedBytes = Total(DirtyPackedBytes);
		if (vertexCount == 0 && indexCount == 0 && packedBytes == 0) {
			Dirty = false;
			return true;
		}

		const size_t vertexBytes = vertexCount * sizeof(assets::MeshVertex);
		const size_t indexBytes = indexCount * sizeof(uint32_t);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(vertexBytes + indexBytes + packedBytes);

		SDL_GPUTransferBuffer *transfer = gpu::CreateTransferBuffer(Device, &transferInfo);
		if (transfer == nullptr) {
			ENGINE_ERROR("mesh table: transfer buffer: {}", SDL_GetError());
			return false;
		}

		// The spans laid end to end, vertices then indices, so one transfer
		// buffer carries a frame's worth of scattered edits.
		auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(Device, transfer, false));
		size_t written = 0;
		for (const Span &span : DirtyVertices) {
			const size_t bytes = span.Count * sizeof(assets::MeshVertex);
			std::memcpy(mapped + written, HostVertices.data() + span.Offset, bytes);
			written += bytes;
		}
		for (const Span &span : DirtyIndices) {
			const size_t bytes = span.Count * sizeof(uint32_t);
			std::memcpy(mapped + written, HostIndices.data() + span.Offset, bytes);
			written += bytes;
		}
		for (const Span &span : DirtyPackedBytes) {
			std::memcpy(mapped + written, HostPackedVertices.data() + span.Offset, span.Count);
			written += span.Count;
		}
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		if (copy == nullptr) {
			ENGINE_ERROR("mesh table: begin copy pass: {}", SDL_GetError());
			gpu::ReleaseTransferBuffer(Device, transfer);
			return false;
		}

		// **Never cycled, which is the price of writing part of the buffer.**
		// Cycling hands back a fresh allocation whose contents are undefined, so
		// a partial write into a cycled buffer would discard every mesh it did
		// not name.
		//
		// **What makes that safe is `DEFERRED_FRAMES`, not the offsets.** It
		// used to be the offsets: every write was an append, so the copy landed
		// past anything a frame in flight could be reading. A reused run is not
		// past anything - it is storage that was in use three frames ago - and
		// the deferral is the whole of the argument that nothing is still
		// reading it. Batching the flush to once per frame is what keeps this to
		// one barrier.
		size_t read = 0;
		for (const Span &span : DirtyVertices) {
			const size_t bytes = span.Count * sizeof(assets::MeshVertex);
			SDL_GPUTransferBufferLocation source{transfer, static_cast<uint32_t>(read)};
			SDL_GPUBufferRegion destination{
				VertexBuffer,
				static_cast<uint32_t>(span.Offset * sizeof(assets::MeshVertex)),
				static_cast<uint32_t>(bytes)
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
			read += bytes;
		}

		for (const Span &span : DirtyIndices) {
			const size_t bytes = span.Count * sizeof(uint32_t);
			SDL_GPUTransferBufferLocation source{transfer, static_cast<uint32_t>(read)};
			SDL_GPUBufferRegion destination{
				IndexBuffer,
				static_cast<uint32_t>(span.Offset * sizeof(uint32_t)),
				static_cast<uint32_t>(bytes)
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
			read += bytes;
		}

		for (const Span &span : DirtyPackedBytes) {
			SDL_GPUTransferBufferLocation source{transfer, static_cast<uint32_t>(read)};
			SDL_GPUBufferRegion destination{
				PackedVertexBuffer, static_cast<uint32_t>(span.Offset), static_cast<uint32_t>(span.Count)
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
			read += span.Count;
		}

		SDL_EndGPUCopyPass(copy);
		gpu::ReleaseTransferBuffer(Device, transfer);

		DirtyVertices.clear();
		DirtyIndices.clear();
		DirtyPackedBytes.clear();
		PackedUploadBytes += packedBytes;
		core::Metrics::Count("render.mesh.packed_upload_bytes", packedBytes);
		Uploads++;
		Dirty = false;
		return true;
	}

	const MeshEntry &MeshTable::Resolve(const core::Name &name, core::Name owner) const {
		if (name.IsValid()) {
			const auto found = Entries.find(MeshKey(name, owner));
			if (found != Entries.end()) {
				return found->second;
			}
		}
		return Fallback;
	}

	bool MeshTable::Has(const core::Name &name, core::Name owner) const {
		return name.IsValid() && Entries.find(MeshKey(name, owner)) != Entries.end();
	}
	size_t MeshTable::DropOwner(core::Name owner) {
		if (!owner.IsValid()) return 0;
		return std::erase_if(Entries, [&](const auto &pair) {
			if (uint32_t(pair.first >> 32) != owner.Id()) return false;
			const MeshEntry &entry = pair.second;
			if (entry.Packed)
				Release(FreePackedBytes, entry.PackedByteOffset, entry.PackedByteCount, Generation);
			else
				Release(
					FreeVertices, static_cast<size_t>(entry.Whole.VertexOffset), entry.VertexCount, Generation
				);
			Release(FreeIndices, entry.Whole.FirstIndex, entry.Whole.IndexCount, Generation);
			return true;
		});
	}

}
