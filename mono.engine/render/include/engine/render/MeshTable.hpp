#pragma once

// Every mesh the renderer can draw, in one pair of buffers.
//
// **A name resolves to a range, not to a buffer.** `scene::DrawInstance::Mesh`
// has been a `core::Name` since v0.4 and nothing consumed it: the renderer
// replayed the same thirty-six indices for every instance in the list, so a
// sphere collided as a sphere and drew as a cube. This is the table that closes
// that, and the shape it takes is the one decision worth defending.
//
// **One vertex buffer and one index buffer for the whole world.** The
// alternative — a buffer pair per mesh — makes every change of mesh a rebind,
// and a rebind is a state change the driver cannot batch across. With one pair,
// a mesh is `(firstIndex, indexCount, vertexOffset)` and switching meshes costs
// three integers on a draw call that was going to be issued anyway. The cost is
// that adding a mesh re-uploads the whole thing; meshes arrive when content
// does, which is a handful of times over a session and never inside a frame.
//
// **Growth is a full re-upload rather than a suballocator.** A free-list over
// device memory is a real allocator with real fragmentation, and the thing it
// would buy — cheap eviction — is not something this engine does yet: nothing
// unloads a mesh. When something does, this is the file that changes, and its
// suite is what will say whether the change was right.
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
		// **The reason a mesh's own indices are stored unchanged.** A draw call
		// takes a vertex offset, so a mesh appended at vertex ten thousand
		// keeps the indices its author wrote and the offset does the work. The
		// alternative — rewriting every index on upload — would make a re-upload
		// after a growth an O(indices) rewrite rather than a memcpy.
		int32_t VertexOffset = 0;
	};

	// One registered mesh.
	//
	// @client
	// @since v0.9
	struct MeshEntry {
		// The whole mesh, for a pass that does not care about materials — the
		// shadow pass draws this and binds nothing.
		MeshRange Whole;

		// One run per material, in the order the mesh declared them.
		//
		// Empty for a mesh with no submeshes, which is every built-in: a
		// consumer draws `Whole` and uses its own default material.
		std::vector<MeshRange> Runs;

		// What each run samples and what colour it is multiplied by, parallel
		// to `Runs`.
		std::vector<core::Name> Textures;
		std::vector<std::array<float, 4>> Colours;
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
		// **The built-ins are here rather than in the caller**, because "an
		// unnamed mesh draws as a cube" is this table's answer to give and
		// there must be exactly one of it.
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
		// **Cheap and idempotent when nothing changed**, so a caller may call
		// it every frame at the barrier where content becomes visible without
		// thinking about it — which is what `delivery::Client::Pump` shaped
		// every other consumer around.
		//
		// @return `false` when the upload failed. The table keeps whatever it
		//         had, so a failed upload is a frame drawn with the old
		//         geometry rather than with none.
		bool Flush();

		// The entry for a name, or the default when the name is unknown.
		//
		// **Never null, and that is the point.** A mesh that has not arrived
		// yet is the ordinary state of a streaming game, and a renderer that
		// had to branch on null at every instance would branch on it in the
		// inner loop. An unknown name draws as a cube, visibly, which is what
		// "the consumer's default" has meant since `DrawInstance` was written.
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
		SDL_GPUBuffer *Vertices() const {
			return VertexBuffer;
		}
		SDL_GPUBuffer *Indices() const {
			return IndexBuffer;
		}

	  private:
		bool Upload();

		SDL_GPUDevice *Device = nullptr;
		SDL_GPUBuffer *VertexBuffer = nullptr;
		SDL_GPUBuffer *IndexBuffer = nullptr;

		// What is on the device, so a growth is a re-create rather than a
		// re-create every time.
		size_t VertexCapacity = 0;
		size_t IndexCapacity = 0;

		std::vector<assets::MeshVertex> HostVertices;
		std::vector<uint32_t> HostIndices;
		std::unordered_map<uint32_t, MeshEntry> Entries;
		MeshEntry Fallback;
		bool Dirty = false;
	};
}
