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

		// The built-ins, every one of them, so a scene may name any of the six
		// without anything having been published. They cost about two thousand
		// vertices between them — the sphere is most of it.
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

		// **The fallback is the cube, resolved once.** Looking it up per
		// unknown name would be a hash lookup in the draw loop for an answer
		// that never changes.
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

		// **Appended rather than replacing in place, even when the name is
		// already known.** A replacement mesh is almost never the same size,
		// and moving every mesh after it to close the gap would invalidate
		// every `MeshRange` already handed out. The old vertices become dead
		// space until something evicts — which nothing does yet, and the
		// header says so.
		MeshEntry entry;
		entry.Whole.FirstIndex = static_cast<uint32_t>(HostIndices.size());
		entry.Whole.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
		entry.Whole.VertexOffset = static_cast<int32_t>(HostVertices.size());

		for (const assets::Submesh &submesh : mesh.Submeshes) {
			MeshRange run;
			run.FirstIndex = entry.Whole.FirstIndex + submesh.FirstIndex;
			run.IndexCount = submesh.IndexCount;
			run.VertexOffset = entry.Whole.VertexOffset;
			entry.Runs.push_back(run);

			// Interned here and nowhere else on this path. `assets::Submesh`
			// holds a `std::string` precisely so that content cannot grow the
			// name registry; this is the point where a name has been accepted
			// and becomes something to compare with an integer.
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

		const size_t vertexBytes = HostVertices.size() * sizeof(assets::MeshVertex);
		const size_t indexBytes = HostIndices.size() * sizeof(uint32_t);

		// Grown in doubling steps rather than to the exact size, so a game
		// streaming meshes in one at a time does not recreate two device
		// buffers on every arrival.
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
				return false;
			}

			VertexCapacity = vertices;
			IndexCapacity = indices;
		}

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(vertexBytes + indexBytes);

		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);
		if (transfer == nullptr) {
			ENGINE_ERROR("mesh table: transfer buffer: {}", SDL_GetError());
			return false;
		}

		auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(Device, transfer, false));
		std::memcpy(mapped, HostVertices.data(), vertexBytes);
		std::memcpy(mapped + vertexBytes, HostIndices.data(), indexBytes);
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		SDL_GPUTransferBufferLocation source{transfer, 0};
		SDL_GPUBufferRegion destination{VertexBuffer, 0, static_cast<uint32_t>(vertexBytes)};

		// Cycled, because a re-upload may land while the previous frame is
		// still reading the buffer — the same reason the instance upload
		// cycles.
		SDL_UploadToGPUBuffer(copy, &source, &destination, true);

		source.offset = static_cast<uint32_t>(vertexBytes);
		destination = SDL_GPUBufferRegion{IndexBuffer, 0, static_cast<uint32_t>(indexBytes)};
		SDL_UploadToGPUBuffer(copy, &source, &destination, true);

		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		SDL_ReleaseGPUTransferBuffer(Device, transfer);

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
