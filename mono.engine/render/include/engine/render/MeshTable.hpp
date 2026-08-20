#pragma once

// Renderer meshes share one vertex buffer and one index buffer.
// Growth rebuilds both buffers; mesh eviction is not supported.
//
// `Add` accumulates on the host and `Flush` sends what is new, so the cost of
// admitting a batch of meshes is one transfer of the batch rather than one
// transfer of the whole table per mesh.
//
// @tier L12 · client

#include <engine/assets/Mesh.hpp>
#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

struct SDL_GPUBuffer;
struct SDL_GPUDevice;

namespace engine::render {

	// Where one run of triangles sits in the shared buffers.
	//
	// @client
	// @since v0.9
	struct MeshRange {
		// The first index of the run, as an offset into the shared index
		// buffer.
		uint32_t FirstIndex = 0;

		// How many indices it covers.
		uint32_t IndexCount = 0;

		// What to add to every index before it names a vertex.
		//
		// Draw calls apply this offset, so uploaded mesh indices stay unchanged.
		int32_t VertexOffset = 0;
	};

	// One registered mesh.
	//
	// @client
	// @since v0.9
	struct MeshEntry {
		// The whole mesh, for a pass that does not care about materials - the
		// shadow pass draws this and binds nothing.
		MeshRange Whole;

		// One run per material, in the order the mesh declared them.
		//
		// Empty for a mesh with no submeshes, which is every built-in: a
		// consumer draws `Whole` and uses its own default material.
		std::vector<MeshRange> Runs;

		// What each run samples and what colour it is multiplied by, parallel
		// to `Runs`.
		//@{
		std::vector<core::Name> Textures;
		std::vector<std::array<float, 4>> Colours;
		//@}

		// The middle of the mesh's own bounding box, in mesh space.
		//
		// **Subtracted before scaling, so a model authored off-centre sits where
		// the part is** rather than at an offset nobody can see the cause of.
		core::Vector3 Centre;

		// Half the mesh's own bounding box, in mesh space.
		//
		// **This is what makes `MeshPart.Size` a box rather than a multiplier.**
		// The renderer scales by `HalfExtent / Extent`, so the mesh's own box is
		// mapped exactly onto the part's - which is Roblox's `MeshPart` semantic
		// and, more usefully here, the thing that makes `scene::Bounds` true.
		// `graph::CullAndBound` tests that box against the frustum, and before
		// this the drawn geometry could be any size at all relative to it: a
		// character baked at authored scale extended ten times further than the
		// box describing it and was culled while still on screen.
		//
		// Half of one on every axis for a built-in, whose geometry is a unit
		// shape about its own origin - so `HalfExtent / 0.5` is `HalfExtent * 2`
		// and every built-in draws exactly as it did before this field existed.
		core::Vector3 Extent{0.5f, 0.5f, 0.5f};
	};

	// The meshes a renderer can draw.
	//
	// @client
	// @since v0.9
	class MeshTable {
	  public:
		// The most vertices the table will hold across every mesh.
		//
		// A ceiling on device memory reachable from content, and the same
		// reasoning `assets::Mesh::MAXIMUM_VERTICES` carries one level down:
		// this one bounds the *sum*, because a game fetching a thousand valid
		// meshes is a thousand valid allocations.
		static constexpr size_t MAXIMUM_VERTICES = 8u * 1024u * 1024u;

		// The same, for indices.
		static constexpr size_t MAXIMUM_INDICES = 32u * 1024u * 1024u;

		MeshTable() = default;
		~MeshTable();

		MeshTable(const MeshTable &) = delete;
		MeshTable &operator=(const MeshTable &) = delete;

		// Takes the device and registers the engine's built-in meshes.
		//
		// Registers the fallback mesh used for unknown names.
		//
		// @param device The GPU device. Kept, not owned.
		// @return `false` when the built-ins could not be uploaded.
		bool Initialise(SDL_GPUDevice *device);

		// Releases the buffers.
		void Shutdown();

		// Registers a mesh, replacing one of the same name.
		//
		// The bytes are kept on the host until `Flush`, so a burst of arrivals
		// costs one upload rather than one each.
		//
		// @param name The name a `DrawInstance` will ask for.
		// @param mesh The geometry. An invalid one is refused.
		// @return `false` for an invalid mesh or a table that would overflow.
		bool Add(const core::Name &name, const assets::MeshData &mesh);

		// Uploads whatever `Add` has accumulated.
		//
		// Safe to call when no mesh has changed, and cheap: only the bytes
		// appended since the last flush are sent, so a hundred meshes admitted
		// in one frame cost one transfer of their own size rather than a hundred
		// transfers of the whole table.
		//
		// @return `false` when the upload failed. The table keeps whatever it
		//         had, so a failed upload is a frame drawn with the old
		//         geometry rather than with none.
		bool Flush();

		// How many device uploads have happened.
		//
		// The number `Flush` exists to keep down, and therefore the number worth
		// asserting: a burst of arrivals that moves this by more than one has
		// lost the batching it is supposed to have.
		size_t UploadCount() const {
			return Uploads;
		}

		// Vertices and indices added but not yet uploaded.
		//
		// What the next `Flush` will send, which is the other half of the same
		// property: a delta that is the size of the whole table means the mark
		// was not kept.
		//@{
		size_t PendingVertexCount() const {
			return HostVertices.size() - UploadedVertices;
		}
		size_t PendingIndexCount() const {
			return HostIndices.size() - UploadedIndices;
		}
		//@}

		// The entry for a name, or the default when the name is unknown.
		//
		// Unknown names resolve to the fallback mesh; this never returns null.
		const MeshEntry &Resolve(const core::Name &name) const;

		// Whether a name has been registered.
		//
		// @param name The name.
		// @return `true` when `Resolve` would return that mesh rather than the
		//         default.
		bool Has(const core::Name &name) const;

		// How many meshes are registered.
		size_t Count() const {
			return Entries.size();
		}

		// The shared buffers, for binding.
		//@{
		SDL_GPUBuffer *Vertices() const {
			return VertexBuffer;
		}
		SDL_GPUBuffer *Indices() const {
			return IndexBuffer;
		}
		//@}

	  private:
		bool Upload();

		SDL_GPUDevice *Device = nullptr;
		SDL_GPUBuffer *VertexBuffer = nullptr;
		SDL_GPUBuffer *IndexBuffer = nullptr;

		// What is on the device, so a growth is a re-create rather than a
		// re-create every time.
		size_t VertexCapacity = 0;
		size_t IndexCapacity = 0;

		// How far into the host arrays the device has been told about.
		//
		// **The mark that makes a flush a delta.** `Add` only ever appends -
		// replacing a mesh appends its geometry and repoints the entry, which is
		// what keeps a range a frame in flight is drawing from valid - so
		// everything below this is already on the device and byte-identical.
		// Reset to zero when the buffers are recreated, because the new ones are
		// empty.
		//@{
		size_t UploadedVertices = 0;
		size_t UploadedIndices = 0;
		//@}

		// Device uploads performed. See `UploadCount`.
		size_t Uploads = 0;

		std::vector<assets::MeshVertex> HostVertices;
		std::vector<uint32_t> HostIndices;
		std::unordered_map<uint32_t, MeshEntry> Entries;
		MeshEntry Fallback;
		bool Dirty = false;
	};
}
