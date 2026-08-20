#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/render/MeshTable.hpp>

#include <SDL3/SDL_gpu.h>

#include <cstring>

namespace engine::render {

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
		const auto found = Entries.find(core::Name(assets::BuiltinName(assets::BuiltinMesh::Cube)).Id());
		if (found == Entries.end()) {
			return false;
		}
		Fallback = found->second;
		return true;
	}

	void MeshTable::Shutdown() {
		if (Device != nullptr) {
			if (VertexBuffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, VertexBuffer);
			}
			if (IndexBuffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, IndexBuffer);
			}
		}

		VertexBuffer = nullptr;
		IndexBuffer = nullptr;
		VertexCapacity = 0;
		IndexCapacity = 0;
		UploadedVertices = 0;
		UploadedIndices = 0;
		Uploads = 0;
		HostVertices.clear();
		HostIndices.clear();
		Entries.clear();
		Fallback = MeshEntry{};
		Device = nullptr;
		Dirty = false;
	}

	bool MeshTable::Add(const core::Name &name, const assets::MeshData &mesh) {
		if (!name.IsValid() || !mesh.IsValid()) {
			return false;
		}
		if (HostVertices.size() + mesh.Vertices.size() > MAXIMUM_VERTICES ||
			HostIndices.size() + mesh.Indices.size() > MAXIMUM_INDICES) {
			ENGINE_WARN("mesh table: full, refusing {}", name.Text());
			return false;
		}

		// Append replacements so existing ranges remain valid until the next upload.
		MeshEntry entry;

		// **The mesh's own box, which is what turns `Size` into a size.**
		// `Mesh::Read` derives these from the vertices - nothing on disk states
		// them - so they are the true bounds and a file that lied about its own
		// could not make a part cull wrong. See `MeshEntry::Extent`.
		entry.Centre = (mesh.Minimum + mesh.Maximum) * 0.5f;
		entry.Extent = (mesh.Maximum - mesh.Minimum) * 0.5f;

		entry.Whole.FirstIndex = static_cast<uint32_t>(HostIndices.size());
		entry.Whole.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
		entry.Whole.VertexOffset = static_cast<int32_t>(HostVertices.size());

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

		HostVertices.insert(HostVertices.end(), mesh.Vertices.begin(), mesh.Vertices.end());
		HostIndices.insert(HostIndices.end(), mesh.Indices.begin(), mesh.Indices.end());

		Entries[name.Id()] = std::move(entry);
		Dirty = true;
		return true;
	}

	bool MeshTable::Flush() {
		if (!Dirty) {
			return true;
		}
		return Upload();
	}

	bool MeshTable::Upload() {
		if (Device == nullptr || HostVertices.empty() || HostIndices.empty()) {
			return false;
		}

		// Grow geometrically to avoid recreating buffers for every arrival.
		//
		// **A growth is the one case that re-sends everything.** The new buffers
		// are empty, so the tail alone would leave the meshes already registered
		// pointing at nothing.
		bool recreated = false;
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
				SDL_ReleaseGPUBuffer(Device, VertexBuffer);
			}
			if (IndexBuffer != nullptr) {
				SDL_ReleaseGPUBuffer(Device, IndexBuffer);
			}

			SDL_GPUBufferCreateInfo vertexInfo{};
			vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
			vertexInfo.size = static_cast<uint32_t>(vertices * sizeof(assets::MeshVertex));
			VertexBuffer = SDL_CreateGPUBuffer(Device, &vertexInfo);

			SDL_GPUBufferCreateInfo indexInfo{};
			indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
			indexInfo.size = static_cast<uint32_t>(indices * sizeof(uint32_t));
			IndexBuffer = SDL_CreateGPUBuffer(Device, &indexInfo);

			if (VertexBuffer == nullptr || IndexBuffer == nullptr) {
				ENGINE_ERROR("mesh table: buffers: {}", SDL_GetError());
				VertexCapacity = 0;
				IndexCapacity = 0;
				UploadedVertices = 0;
				UploadedIndices = 0;
				return false;
			}

			VertexCapacity = vertices;
			IndexCapacity = indices;
			recreated = true;
		}

		// **Only what `Add` appended since the last upload.** `Add` never
		// rewrites a byte it has already handed to the device - a replacement
		// appends its geometry and repoints the entry, which is what keeps the
		// ranges a frame in flight is drawing from valid - so everything below
		// the mark is already on the device and identical. Re-sending it made
		// registering N meshes cost O(N^2) bytes of memcpy and PCIe traffic, and
		// a table near its eight-million-vertex ceiling made that a 384 MB
		// transfer for one arriving mesh. That is the stall a game shows as a
		// freeze while its meshes load.
		const size_t firstVertex = recreated ? 0 : UploadedVertices;
		const size_t firstIndex = recreated ? 0 : UploadedIndices;
		const size_t vertexCount = HostVertices.size() - firstVertex;
		const size_t indexCount = HostIndices.size() - firstIndex;
		if (vertexCount == 0 && indexCount == 0) {
			Dirty = false;
			return true;
		}

		const size_t vertexBytes = vertexCount * sizeof(assets::MeshVertex);
		const size_t indexBytes = indexCount * sizeof(uint32_t);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(vertexBytes + indexBytes);

		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);
		if (transfer == nullptr) {
			ENGINE_ERROR("mesh table: transfer buffer: {}", SDL_GetError());
			return false;
		}

		auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(Device, transfer, false));
		if (vertexBytes > 0) {
			std::memcpy(mapped, HostVertices.data() + firstVertex, vertexBytes);
		}
		if (indexBytes > 0) {
			std::memcpy(mapped + vertexBytes, HostIndices.data() + firstIndex, indexBytes);
		}
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		// **Never cycled, which is the price of writing only the tail.** Cycling
		// hands back a fresh allocation whose contents are undefined, so a
		// partial write into a cycled buffer would discard every mesh below the
		// mark. The copy is to bytes past everything a frame in flight can be
		// reading, so a barrier is all it costs - and batching the flush to once
		// per frame is what keeps it to one.
		if (vertexBytes > 0) {
			SDL_GPUTransferBufferLocation source{transfer, 0};
			SDL_GPUBufferRegion destination{
				VertexBuffer,
				static_cast<uint32_t>(firstVertex * sizeof(assets::MeshVertex)),
				static_cast<uint32_t>(vertexBytes)
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
		}

		if (indexBytes > 0) {
			SDL_GPUTransferBufferLocation source{transfer, static_cast<uint32_t>(vertexBytes)};
			SDL_GPUBufferRegion destination{
				IndexBuffer,
				static_cast<uint32_t>(firstIndex * sizeof(uint32_t)),
				static_cast<uint32_t>(indexBytes)
			};
			SDL_UploadToGPUBuffer(copy, &source, &destination, false);
		}

		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		SDL_ReleaseGPUTransferBuffer(Device, transfer);

		UploadedVertices = HostVertices.size();
		UploadedIndices = HostIndices.size();
		Uploads++;
		Dirty = false;
		return true;
	}

	const MeshEntry &MeshTable::Resolve(const core::Name &name) const {
		if (name.IsValid()) {
			const auto found = Entries.find(name.Id());
			if (found != Entries.end()) {
				return found->second;
			}
		}
		return Fallback;
	}

	bool MeshTable::Has(const core::Name &name) const {
		return name.IsValid() && Entries.find(name.Id()) != Entries.end();
	}
}
