#pragma once

// A mesh a script builds and edits while the engine runs, rather than one
// that arrived from content.
//
// **The class `MeshTable::Add` was written for and that nothing minted.**
// `render::MeshTable` has taken a name and an `assets::MeshData` since v0.9 -
// the content pump has always been able to hand it one built by an importer.
// What did not exist was a second producer: a mesh a *script* builds a
// triangle at a time, in a world with no importer anywhere near it. This is
// that producer's storage.
//
// ## What is here and what is not
//
// Roblox's `EditableMesh` is close to eighty methods: bones, FACS poses,
// batched attribute arrays, per-face-per-vertex normals and UVs distinct from
// the position they sit on. This is the core a mesh needs to exist and be
// correct - one array per attribute, one shared index into all of them, and
// the operations that add, edit and remove a triangle - and it stops there
// deliberately. A vertex is one position, one normal and one UV; two faces
// sharing a corner share all three, which is the ordinary case for a mesh
// built in code. A hard edge or a UV seam is two vertices at one position,
// and the optional `Welds` stream says they are one point, so recalculated
// normals and the automatic LOD builder treat them as one.
//
// **No vertex removal.** A triangle can be removed - it is three indices,
// and removing a run of three is a `swap-and-pop` that only ever invalidates
// the *other* triangle's id, which `RemoveTriangle` documents. A vertex may
// be referenced by any number of triangles, so removing one would mean
// walking every triangle to fix up or refuse the reference - real work this
// pass does not do. `Clear` is the door for "start over."
//
// **Skin and free channels are optional arrays.** Four joint slots and weights
// per vertex make the mesh drawable by a `SkinnedMeshPart`; four float4
// channels carry shader data. An unskinned mesh stores neither.
//
// ## Where the geometry goes
//
// **Never converted here.** `scene` may not link `assets` - the format
// `render::MeshTable::Add` takes - so this module holds the raw arrays and a
// revision counter, exactly as `scene::ShaderSource` holds GLSL text and a
// revision rather than a compiled module. `client::UpdateEditableMeshes` is
// where the conversion and the upload happen, on the same two-tier split
// `render::ShaderLibrary` already draws between "a world's own words" and
// "a device's own pipeline."
//
// **Named so a part can find it.** `EditableMeshContentName` derives a
// stable `core::Name` from the entity itself, and a script assigns that name
// to `MeshPart.MeshId` exactly as it would assign a published mesh's asset
// name - the render side does not know or care which kind of name resolved.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/ClassId.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/EditablePacking.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Four palette slots influencing one vertex.
	using EditableJoints = std::array<uint16_t, 4>;
	// The weight of each slot in `EditableJoints`, in the same order.
	using EditableWeights = std::array<float, 4>;
	// One free float4 shader attribute.
	using EditableAttribute = std::array<float, 4>;

	// How many free float4 attribute channels a mesh may carry.
	inline constexpr size_t EDITABLE_ATTRIBUTE_CHANNELS = 4;

	// The most vertices and indices one editable mesh may hold through a bulk
	// call. The same ceilings `assets::Mesh` reads with, so anything built here
	// is also something the mesh format could carry.
	inline constexpr uint32_t MAXIMUM_EDITABLE_MESH_VERTICES = 4u * 1024u * 1024u;
	inline constexpr uint32_t MAXIMUM_EDITABLE_MESH_INDICES = 16u * 1024u * 1024u;

	// The per-vertex streams `SetEditableVertexStream` writes.
	//
	// @since v0.26
	enum class EditableVertexStream : uint8_t {
		Position,	// f32 x3
		Normal,		// f32 x3
		UV,			// f32 x2
		Colour,		// f32 x4: red, green, blue, then transparency as `Alphas` holds it
		Attribute0, // f32 x4, through `Attribute3`
		Attribute1,
		Attribute2,
		Attribute3,
		Weld, // u32 x1, see `EditableMesh::Welds`
	};

	// Little-endian scalars per vertex record of one stream.
	uint32_t EditableVertexStreamComponents(EditableVertexStream stream);

	// The bit `EditableStreamChanges::Streams` sets for one stream.
	constexpr uint16_t EditableStreamBit(EditableVertexStream stream) {
		return static_cast<uint16_t>(1u << static_cast<uint8_t>(stream));
	}

	// What moved across consecutive revisions `Since` to `Through` when every
	// one of them only rewrote existing vertex streams: no count, topology,
	// skin, colour run or channel presence changed.
	//
	// **How the uploader avoids a rebuild.** A consumer that last saw revision
	// `R` may patch only `[First, End)` of `Streams` when `Through` is the
	// mesh's current revision and `Since <= R + 1`; anything else is a full
	// rebuild. One record rather than a log, because a range that is a
	// superset of what changed is still a correct patch.
	struct EditableStreamChanges {
		uint32_t Since = 0;
		uint32_t Through = 0;
		uint16_t Streams = 0;
		uint32_t First = 0;
		uint32_t End = 0;

		// Whether a consumer that last saw `seen` can patch to `current`.
		bool Covers(uint32_t seen, uint32_t current) const {
			return Streams != 0 && seen < current && Through == current && Since <= seen + 1;
		}
	};

	// One vertex's own attributes, parallel across every array below by
	// index - vertex `i` is `Positions[i]`, `Normals[i]`, `UVs[i]` and
	// `Colours[i]`.
	//
	// @since v0.18
	struct EditableMesh {
		// Object-space positions. What `AddVertex` appends to.
		std::vector<core::Vector3> Positions;

		// Per-vertex normals, need not be unit length - `render::MeshVertex`
		// carries the same relaxation. Defaults to straight up for a vertex
		// nothing has set one for, which reads as flat-lit rather than as a
		// missing normal.
		std::vector<core::Vector3> Normals;

		// Per-vertex texture coordinates.
		std::vector<core::Vector2> UVs;

		// Per-vertex colour, multiplied into whatever the part's own texture
		// samples - `assets::Submesh::BaseColour` one level down, spread
		// across the vertices instead of held once for the whole run.
		std::vector<core::Color3> Colours;

		// Per-vertex alpha, parallel to `Colours` and kept apart from it for
		// `Visual::Transparency`'s reason: a colour and how much of it shows
		// are two different questions with two different consumers.
		std::vector<float> Alphas;

		// The triangle list. Always a multiple of three; three consecutive
		// entries are one triangle's vertex ids, counter-clockwise seen from
		// outside - `render/AGENTS.md`'s winding rule, unchanged for a mesh
		// built in code.
		std::vector<uint32_t> Indices;

		// Per-vertex skin: four palette slots and their weights. Both empty
		// for an unskinned mesh, otherwise both parallel to `Positions`.
		// Weights need not sum to one; the render adapter normalises them, and
		// a vertex whose weights are all zero is drawn unskinned.
		//@{
		std::vector<EditableJoints> Joints;
		std::vector<EditableWeights> Weights;
		//@}

		// Up to `EDITABLE_ATTRIBUTE_CHANNELS` free float4 channels a shader can
		// read. Each is empty or parallel to `Positions`.
		std::array<std::vector<EditableAttribute>, EDITABLE_ATTRIBUTE_CHANNELS> Attributes;

		// Optional canonical point per vertex: empty, or one entry per vertex
		// naming the vertex whose position it shares. Render vertices split
		// along a UV seam name the same canonical index, so normal
		// recalculation and the automatic LOD builder treat them as one point.
		// Cleared by every whole-geometry replacement.
		std::vector<uint32_t> Welds;

		// Presentation-only attribute encoding. The canonical arrays above remain
		// authoritative for editing, saves and collision; the render adapter makes
		// a compact copy and decodes it at its boundary.
		EditablePacking Packing;

		// A content signature computed by a bulk geometry commit. Zero means the
		// incremental editing API changed an array and the signature has not been
		// recomputed yet. It is derived state and is never a content identifier.
		uint64_t Signature = 0;

		// Which vertex streams moved over a run of consecutive revisions in
		// which nothing else changed. Derived state, never saved.
		EditableStreamChanges StreamChanges;

		// Bumped by every call that changes what this describes.
		//
		// **The whole of how `client::UpdateEditableMeshes` knows to
		// re-upload**, matching `ShaderSource::Revision`'s exact reason: a
		// per-frame walk that diffed the arrays themselves would cost the
		// comparison it exists to avoid, so an integer that only moves
		// forward is compared instead.
		uint32_t Revision = 0;
	};

	// All semantic arrays needed to replace one editable mesh in one operation.
	// This is independent of the renderer's packed vertex layout, so a renderer
	// layout change does not change the scene or script contract.
	//
	// @since v0.19
	struct EditableMeshGeometry {
		// Object-space positions, one per vertex.
		std::vector<core::Vector3> Positions;

		// Object-space normals, one per vertex.
		std::vector<core::Vector3> Normals;

		// Texture coordinates, one per vertex.
		std::vector<core::Vector2> UVs;

		// Linear colours, one per vertex.
		std::vector<core::Color3> Colours;

		// Transparency values, one per vertex.
		std::vector<float> Alphas;

		// Triangle vertex indices, three consecutive values per triangle.
		std::vector<uint32_t> Indices;

		// Skin slots and weights, both empty or both one per vertex.
		std::vector<EditableJoints> Joints;
		std::vector<EditableWeights> Weights;

		// Free shader channels, each empty or one per vertex.
		std::array<std::vector<EditableAttribute>, EDITABLE_ATTRIBUTE_CHANNELS> Attributes;
	};

	// Where one attribute sits in a packed vertex buffer: element `i` starts
	// at `Offset + i * Stride` bytes. A zero stride means tightly packed.
	struct EditableBufferField {
		bool Present = false;
		uint32_t Offset = 0;
		uint32_t Stride = 0;
	};

	// How `DecodeEditableMeshBuffers` reads packed bytes. Every float field is
	// little-endian f32, `Joints` is four u16 and indices are u16 or u32.
	// Only `Position` is required; an absent field takes the value a new
	// vertex would have.
	struct EditableBufferLayout {
		uint32_t VertexCount = 0;
		bool WideIndices = true;
		EditableBufferField Position;											 // f32 x3
		EditableBufferField Normal;												 // f32 x3
		EditableBufferField UV;													 // f32 x2
		EditableBufferField Colour;												 // f32 x3
		EditableBufferField Alpha;												 // f32 x1
		EditableBufferField Joints;												 // u16 x4
		EditableBufferField Weights;											 // f32 x4
		std::array<EditableBufferField, EDITABLE_ATTRIBUTE_CHANNELS> Attributes; // f32 x4
	};

	// Decodes packed vertex and index bytes into semantic geometry.
	//
	// Refuses a count past the mesh ceilings, a field that reads past the end
	// of its buffer, joints without weights or the reverse, and an index byte
	// count that is not whole indices. Index ranges and finiteness are checked
	// later by `PrepareEditableMesh`, which every commit runs.
	//
	// @param layout   Where each field sits.
	// @param vertices The packed vertex bytes.
	// @param indices  The packed index bytes.
	// @param out      Filled on success.
	// @return Empty on success, otherwise why it was refused.
	std::string DecodeEditableMeshBuffers(
		const EditableBufferLayout &layout,
		std::span<const std::byte> vertices,
		std::span<const std::byte> indices,
		EditableMeshGeometry &out
	);

	// The result of preparing or committing a complete geometry transaction.
	//
	// @since v0.19
	enum class EditableMeshCommit : uint8_t {
		Applied,
		Unchanged,
		Missing,
		Stale,
		Invalid,
	};

	// A validated geometry value and its content signature. Preparation is pure
	// and may run on a worker. Committing it must run on the world's owner thread.
	//
	// @since v0.19
	struct PreparedEditableMesh {
		// The owned geometry to publish.
		EditableMeshGeometry Geometry;

		// Exact content signature of `Geometry`.
		uint64_t Signature = 0;

		// Whether every array and index satisfies the geometry contract.
		bool Valid = false;
	};

	// One complete geometry replacement at a captured target revision.
	struct EditableMeshUpdate {
		ecs::Entity Instance{};			 // Complete generation-bearing target handle.
		uint32_t ExpectedRevision = 0;	 // Revision observed before preparation.
		PreparedEditableMesh Prepared{}; // Owned scene-validated geometry.
	};

	// Bounds preflight storage and duplicate checks for one atomic publication.
	inline constexpr size_t MAXIMUM_EDITABLE_MESH_BATCH = 64;

	// Checks every target and revision, obtains every mutable destination, then
	// publishes all geometry with nonthrowing swaps. A refusal changes no mesh.
	// No structural operation, observer callback, allocation or lookup runs after
	// the first geometry write, so callers may retain other row pointers on their
	// own stack while committing related values immediately after success.
	// Consumes prepared geometry only on success. Empty batches are Unchanged.
	EditableMeshCommit CommitEditableMeshBatch(ecs::Store &store, std::span<EditableMeshUpdate> updates);

	// Validates and signs a geometry transaction without touching a world.
	//
	// Attribute arrays must have the same length, the index count must be a
	// multiple of three, and every index must name a vertex. Skin arrays are
	// both empty or both one per vertex, with slots below `MAX_JOINTS` and
	// finite non-negative weights; each attribute channel is empty or one per
	// vertex and finite.
	//
	// @param geometry Geometry owned by the returned value.
	// @return The prepared transaction. `Valid` says whether it may be committed.
	// @since v0.19
	PreparedEditableMesh PrepareEditableMesh(EditableMeshGeometry geometry);

	// Commits a prepared transaction if the target still has the revision it had
	// when the request was submitted. Identical content is a no-op, so neither
	// collision nor GPU residency is disturbed by a redundant authoring call.
	//
	// @param store            The target world.
	// @param instance         The editable mesh.
	// @param prepared         A value returned by `PrepareEditableMesh`.
	// @param expectedRevision The revision observed when work was submitted.
	// @return What happened.
	// @since v0.19
	EditableMeshCommit CommitEditableMesh(
		ecs::Store &store, ecs::Entity instance, PreparedEditableMesh prepared, uint32_t expectedRevision
	);

	// Replaces all geometry immediately. Script adapters use the prepared form
	// so several requests can be computed as one batch before their owner-thread
	// commits.
	//
	// @param store    The target world.
	// @param instance The editable mesh.
	// @param geometry Complete semantic geometry.
	// @return What happened.
	// @since v0.19
	EditableMeshCommit
	ReplaceEditableMesh(ecs::Store &store, ecs::Entity instance, EditableMeshGeometry geometry);

	// Computes the same non-zero process-local signature used by a bulk commit.
	// Invalid geometry still has a stable signature. Validity is a separate
	// question answered by `PrepareEditableMesh`.
	//
	// @param geometry Geometry to sign.
	// @return A non-zero signature.
	// @since v0.19
	uint64_t EditableMeshSignature(const EditableMeshGeometry &geometry);

	// The `core::Name` a part's `MeshId` names this mesh by.
	//
	// **Derived from the entity, not authored** - two `EditableMesh`
	// instances never collide and neither survives being confused with the
	// other, because the id folded into the name is the entity's own.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @return The name, or an invalid one for anything but an `EditableMesh`.
	// @since v0.18
	core::Name EditableMeshContentName(const ecs::Store &store, ecs::Entity instance);

	// Appends a vertex and returns its id.
	//
	// **The id is the index**, so it is stable until `Clear` and never
	// reused by an ordinary edit - `RemoveTriangle` never removes a vertex,
	// so nothing shifts underneath one.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @param position Object-space position.
	// @param normal   The normal, or straight up when omitted.
	// @param uv       The texture coordinate, or the origin when omitted.
	// @return The vertex id, or nothing for anything but an `EditableMesh`.
	// @since v0.18
	std::optional<uint32_t> AddVertex(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector3 &position,
		const core::Vector3 &normal = core::Vector3{0.0f, 1.0f, 0.0f},
		const core::Vector2 &uv = core::Vector2{}
	);

	// Appends a triangle over three existing vertex ids and returns its own
	// id.
	//
	// **The triangle id is not the vertex id's kind of stable** -
	// `RemoveTriangle` explains why the last triangle's id changes when an
	// earlier one is removed.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @param a        The first vertex id, counter-clockwise seen from
	//        outside.
	// @param b        The second.
	// @param c        The third.
	// @return The triangle id, or nothing when the instance is not an
	//         `EditableMesh` or any id names no vertex.
	// @since v0.18
	std::optional<uint32_t>
	AddTriangle(ecs::Store &store, ecs::Entity instance, uint32_t a, uint32_t b, uint32_t c);

	// Removes one triangle.
	//
	// **`swap-and-pop`, not a shift.** The removed triangle's three indices
	// are replaced by whatever was last in the list, so every triangle id
	// but the one moved keeps meaning what it meant - and the one that moved
	// is the id `TriangleCount() - 1` named a moment before this call. A
	// script removing several by id in descending order sees exactly that
	// and nothing surprising; removing them in ascending order does not,
	// which is the same rule `physics::Publish`'s own row-swap gives for the
	// identical reason.
	//
	// @param store    The world.
	// @param instance The `EditableMesh` instance.
	// @param triangle The triangle id.
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool RemoveTriangle(ecs::Store &store, ecs::Entity instance, uint32_t triangle);

	// Sets one vertex's position.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool SetVertexPosition(
		ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &position
	);

	// Sets one vertex's normal.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool
	SetVertexNormal(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &normal);

	// Sets one vertex's texture coordinate.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool SetVertexUV(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector2 &uv);

	// Sets one vertex's colour and alpha.
	//
	// @return `false` when the instance is not an `EditableMesh` or the id
	//         is out of range.
	// @since v0.18
	bool SetVertexColor(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t vertex,
		const core::Color3 &colour,
		float alpha = 0.0f
	);

	// Sets one vertex's skin influences. Unused slots are zero weight. The
	// first call on an unskinned mesh gives every vertex an empty skin.
	//
	// @param joints  Up to four palette slots, each below `MAX_JOINTS`.
	// @param weights One finite non-negative weight per slot.
	// @return `false` when the instance is not an `EditableMesh`, the id is
	//         out of range or the influences are malformed.
	// @since v0.26
	bool SetVertexSkin(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t vertex,
		std::span<const uint16_t> joints,
		std::span<const float> weights
	);

	// Sets one vertex's value in a free attribute channel, filling the rest of
	// a channel that had none with zeros.
	//
	// @return `false` for a bad instance, vertex, channel or non-finite value.
	// @since v0.26
	bool SetVertexAttribute(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t vertex,
		size_t channel,
		const EditableAttribute &value
	);

	// Overwrites `count` records of one vertex stream from packed
	// little-endian bytes, starting at vertex `first`.
	//
	// Vertex count, triangles and skin are untouched. Writing a free channel
	// the mesh does not carry yet creates it, zero elsewhere; writing `Weld`
	// on a mesh without one creates it with every vertex its own point.
	//
	// @param first First vertex written.
	// @param count Records written, or every vertex from `first` when empty.
	// @param bytes Exactly `count` records of the stream's width.
	// @return `false`, changing nothing, for anything but an `EditableMesh`, a
	//         range past the vertex count, a byte length that is not exactly
	//         `count` records, a non-finite float or a weld naming no vertex.
	// @since v0.26
	bool SetEditableVertexStream(
		ecs::Store &store,
		ecs::Entity instance,
		EditableVertexStream stream,
		std::span<const std::byte> bytes,
		uint32_t first = 0,
		std::optional<uint32_t> count = std::nullopt
	);

	// Recomputes normals from the current positions and triangles.
	//
	// Each triangle adds its unnormalised face normal, so larger faces weigh
	// more, to every corner's point. With `Welds` the point is the canonical
	// vertex, so seam duplicates get one shared normal. Only vertices in
	// `[first, first + count)` are written, but every triangle contributes.
	// A vertex no triangle touches keeps its normal.
	//
	// @return `false` for anything but an `EditableMesh` or a range past the
	//         vertex count.
	// @since v0.26
	bool RecalculateEditableMeshNormals(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t first = 0,
		std::optional<uint32_t> count = std::nullopt
	);

	// Empties every array and bumps the revision once.
	//
	// @return `false` for anything but an `EditableMesh`.
	// @since v0.18
	bool ClearEditableMesh(ecs::Store &store, ecs::Entity instance);

	// Updates mesh export packing after validating supported vertex attributes.
	// Leaves editable geometry unchanged and advances both packing and mesh revisions.
	bool SetEditableMeshPacking(ecs::Store &store, ecs::Entity instance, const EditablePacking &packing);

	// The `EditableMesh` class id, registering the tree if nobody has yet.
	//
	// @return The class id.
	// @since v0.18
	ecs::ClassId EditableMeshClass();

	// Which revision of each `EditableMesh` has a collision shape baked for it.
	//
	// **A resource and not a field on the presentation host, which the render
	// side's `engine::render::EditableMeshUploader` is.** An uploader belongs to
	// the renderer because its device does; a shape table belongs to a *world*,
	// and a server holds many at once. A map on the host keyed by entity id
	// would have two worlds' meshes collide on the same key the first time two
	// of them minted the same id, which they do constantly.
	//
	// Derived state: it is registered with a reader that clears, so a world
	// restored from a snapshot bakes everything again rather than inheriting a
	// claim about shapes it does not have. See `CollisionShapes`, which is
	// cleared on load for the same reason.
	//
	// @since v0.19
	struct EditableMeshCollision {
		// One mesh that has been baked, and at which revision.
		struct Baked {
			// The `EditableMesh` instance, by complete entity id.
			uint64_t Instance = 0;

			// `EditableMesh::Revision` at the time it was baked.
			uint32_t Revision = 0;
		};

		// Sorted by complete entity id. A streamed terrain can retain hundreds
		// of editable chunks, so `RefreshEditableMeshCollision` uses binary
		// lookup and a merge sweep rather than turning its steady-state ledger
		// maintenance into quadratic work.
		std::vector<Baked> Rows;
	};

	// Bakes collision geometry only for `EditableMesh` values a `Collider` names,
	// and forgets shapes whose mesh is gone or no longer named.
	//
	// **The engine gap this closes**: a script that built geometry built
	// something that could be seen and not touched. `client::
	// EditableMeshUploader` hands the mesh to the renderer and registered
	// nothing with `CollisionShapes`, so a `MeshPart` naming a run-time mesh
	// fell back to colliding as its own bound - a box the size of the whole
	// thing. A character standing on a script-built heightfield was standing
	// *inside* that box and could not move in any direction.
	//
	// **A free function on a store rather than a class on a host**, so a
	// dedicated server can call it too. That is the half of the gap that
	// matters most: the server is the machine that decides where anybody is
	// standing, and it has no uploader at all.
	//
	// Revision-tracked and demand-driven, because baking is quickhull plus a
	// triangle soup and a streamed visual world can build a mesh a frame. A
	// mesh with no collider naming it allocates no collision storage; adding a
	// matching `Mesh` or `Hull` collider makes the next refresh bake it.
	//
	// Call it wherever the geometry is settled and before physics reads it -
	// which for every host in this repository is once a tick.
	//
	// @param store The world.
	// @param deferred Sorted instance ids not to bake this call; a baked
	//        shape they already have stays until a later call replaces it.
	// @return How many meshes were baked or forgotten this call.
	// @since v0.19
	size_t RefreshEditableMeshCollision(ecs::Store &store, std::span<const uint64_t> deferred = {});

	// One editable mesh some collider needs a baked shape for this tick.
	struct EditableMeshCollisionDemand {
		// The `EditableMesh` instance, by complete entity id.
		uint64_t Instance = 0;

		// Whether a convex hull is wanted as well as the triangle soup.
		bool Hull = false;
	};

	// Which editable meshes need a baked collision shape now, sorted by
	// instance with one row per mesh. A `Mesh` or `Hull` collider naming one
	// demands it when:
	//
	// - `CanQuery` is on: queries run on a const store and cannot build a
	//   shape on first use, so these are baked up front.
	// - Otherwise it can only meet bodies, so it is baked once it moves itself
	//   or a moving collider's bounds come within a margin of its own.
	//
	// A collider with `CanCollide`, `CanTouch` and `CanQuery` all off demands
	// nothing, and the refresh forgets any shape it already had.
	// @param store The world.
	// @param out Receives the demand; cleared first.
	void CollectEditableMeshCollisionDemand(
		const ecs::Store &store, std::vector<EditableMeshCollisionDemand> &out
	);

	// Mesh colliders over this many triangles collide through cells instead.
	inline constexpr size_t SPLIT_COLLISION_TRIANGLES = 8'192;
	// Side of one collision cell, in the mesh's own units.
	inline constexpr float SPLIT_COLLISION_CELL = 512.0f;

	// One cell of a split mesh collider: a bare row (no instance class, never
	// saved or replicated) carrying a `Transform`, a copy of the owner's
	// `Collider` naming the cell's own `EditableMesh`, and this.
	//
	// **Why cells.** A terrain mesh of millions of triangles is too much to
	// bake in one tick, and every body near it would pay for the whole tree.
	// Each cell bakes on its own, only when something is near, and over as
	// many ticks as the work budget needs. The owner's own mesh then bakes as
	// an empty shape, so it collides with nothing and costs nothing.
	//
	// @since v0.26
	struct SplitCollisionCell {
		// The part whose collider was split.
		ecs::Entity Owner;
		// The `EditableMesh` it names, and the revision the cell was cut from.
		ecs::Entity Source;
		uint32_t Revision = 0;
		// The cell's centre in the owner's space. The cell's vertices are
		// relative to it and its `Transform` is the owner's moved by it, so
		// its collider's `Extent` bounds the cell alone.
		core::Vector3 Centre;
	};

	// Whether `mesh` on `instance` collides through cells: more than
	// `SPLIT_COLLISION_TRIANGLES` and not itself a cell.
	bool EditableMeshSplitsCollision(const ecs::Store &store, ecs::Entity instance, const EditableMesh &mesh);

	// Keeps every split mesh collider's cells current: cuts them for a static
	// `Mesh` collider naming a mesh that splits, recuts them when the mesh's
	// revision moves, follows the owner's `Transform` and collider settings,
	// and destroys cells whose owner is gone or no longer splits. A collider
	// with `Motion` is not split. Runs before the collision refresh.
	//
	// @return Whether any cell was created, destroyed or changed.
	// @since v0.26
	bool SyncSplitCollision(ecs::Store &store);
}
