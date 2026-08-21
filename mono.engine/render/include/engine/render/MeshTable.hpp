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

#include <array>
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

		// How many vertices this entry owns, starting at `Whole.VertexOffset`.
		//
		// **Not drawn with, and here so the range can be given back.** A draw
		// needs an index count and a base vertex and nothing else; reclaiming
		// needs to know how much of the buffer stops being anybody's when this
		// entry is replaced. `Whole.IndexCount` was already the other half.
		uint32_t VertexCount = 0;
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

		// How many flushes a freed run waits before it may be handed out again.
		//
		// **Three, because three is the most frames this engine can have in
		// flight.** `SDL_SetGPUAllowedFramesInFlight` takes 1 to 3 and
		// `Renderer` clamps to that, so a run freed three flushes ago cannot
		// still be read by anything the device has not finished. That is exactly
		// what lets `Upload` keep writing with `cycle` false: a reused run is
		// past nothing, but it is provably nobody's.
		static constexpr size_t DEFERRED_FRAMES = 3;

		// What `Claim` answers when no free run will do.
		static constexpr size_t NOWHERE = static_cast<size_t>(-1);

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
		// **Replacing a name gives its old run back**, to be handed out again
		// once no frame in flight can still be reading it - see
		// `DEFERRED_FRAMES`. That is what makes a mesh a script rewrites every
		// frame cost a bounded amount of the table rather than a copy per frame.
		//
		// @param name The name a `DrawInstance` will ask for.
		// @param mesh The geometry. An invalid one is refused.
		// @return `false` for an invalid mesh or a table that would overflow.
		bool Add(const core::Name &name, const assets::MeshData &mesh);

		// Uploads whatever `Add` has accumulated.
		//
		// Safe to call when no mesh has changed, and cheap: only the runs
		// written since the last flush are sent, so a hundred meshes admitted in
		// one frame cost one transfer of their own size rather than a hundred
		// transfers of the whole table.
		//
		// **Also the table's clock.** Every call counts as a frame whether or
		// not anything was sent, which is what ages a freed run towards being
		// reusable. A caller that stops flushing stops reclaiming.
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
			return Total(DirtyVertices);
		}
		size_t PendingIndexCount() const {
			return Total(DirtyIndices);
		}

		// How many host slots are owned by nobody, waiting to be reused.
		//
		// A table whose meshes are rewritten in place holds this steady; one
		// that only ever grows leaves it at zero. See `FreeVertices`.
		//@{
		size_t FreeVertexCount() const;
		size_t FreeIndexCount() const;
		//@}

		// How much host storage the table holds, reachable or not.
		//
		// **The number that used to climb forever.** A mesh rewritten every
		// frame settles at `DEFERRED_FRAMES + 1` copies of itself and stays
		// there; before reclamation it added one copy a frame until the table
		// refused it.
		//@{
		size_t HostVertexCount() const {
			return HostVertices.size();
		}
		size_t HostIndexCount() const {
			return HostIndices.size();
		}
		//@}
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

		// Device uploads performed. See `UploadCount`.
		size_t Uploads = 0;

		// Flushes performed, which is frames the table was asked to settle.
		//
		// **Counted separately from `Uploads` because reuse is timed in frames,
		// not in transfers.** A flush with nothing to send performs no upload
		// and still means a frame has passed, and a table with no device
		// performs no upload at all - so timing reclamation off `Uploads` would
		// never release anything in a headless build and could not be tested
		// without a GPU.
		size_t Generation = 0;

		// A run of host storage nobody owns, and the generation it stopped
		// being owned at.
		struct FreeBlock {
			size_t Offset = 0;
			size_t Count = 0;
			size_t FreedAt = 0;
		};

		// A run of host storage the device has not been told about.
		struct Span {
			size_t Offset = 0;
			size_t Count = 0;
		};

		// Storage given back by a replaced entry, in offset order.
		//
		// **The whole of what makes a mesh edited every frame sustainable.**
		// `Add` used to append every replacement and reclaim nothing, so an
		// `EditableMesh` a script rewrites per frame walked the table towards
		// `MAXIMUM_VERTICES` and was eventually refused - a mesh that silently
		// stopped following its own instance.
		//@{
		std::vector<FreeBlock> FreeVertices;
		std::vector<FreeBlock> FreeIndices;
		//@}

		// What `Add` has written that `Upload` has not sent, in offset order.
		//
		// **A list rather than the single mark this used to keep.** A mark works
		// only while every write is an append; reusing a freed block writes into
		// the middle, and one high-water line cannot describe that.
		//@{
		std::vector<Span> DirtyVertices;
		std::vector<Span> DirtyIndices;
		//@}

		// Takes `count` contiguous slots from a free list, or `NOWHERE`.
		//
		// First fit over blocks old enough to reuse, splitting what is left. See
		// `DEFERRED_FRAMES` for what "old enough" means and why.
		static size_t Claim(std::vector<FreeBlock> &blocks, size_t count, size_t generation);

		// Gives `count` slots at `offset` back, merging with either neighbour.
		//
		// **Merged, or a mesh that shrinks and grows fragments its own hole into
		// rubble** - a run of small frees either side of a live entry is
		// unusable for anything bigger than the largest of them.
		static void Release(std::vector<FreeBlock> &blocks, size_t offset, size_t count, size_t generation);

		// Records that `count` slots at `offset` need sending, merging with
		// anything adjacent or overlapping.
		static void MarkDirty(std::vector<Span> &spans, size_t offset, size_t count);

		// How many slots a list of spans covers.
		static size_t Total(const std::vector<Span> &spans);

		std::vector<assets::MeshVertex> HostVertices;
		std::vector<uint32_t> HostIndices;
		std::unordered_map<uint32_t, MeshEntry> Entries;
		MeshEntry Fallback;
		bool Dirty = false;
	};
}
