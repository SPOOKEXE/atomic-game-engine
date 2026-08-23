#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine::scene {

	namespace {
		uint64_t MixMeshSignature(uint64_t hash, uint64_t word) {
			hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}

		uint64_t PairFloats(float first, float second) {
			return static_cast<uint64_t>(std::bit_cast<uint32_t>(first)) |
				   (static_cast<uint64_t>(std::bit_cast<uint32_t>(second)) << 32);
		}

		template <class Geometry> uint64_t SignGeometry(const Geometry &geometry) {
			// Four independent lanes keep the walk throughput-bound instead of
			// making every word wait on one dependency chain. The final fold and
			// tagged lengths keep array order and field identity in the contract.
			uint64_t lanes[4]{
				0xcbf29ce484222325ull,
				0x84222325cbf29ce4ull,
				0x9e3779b97f4a7c15ull,
				0x6a09e667f3bcc909ull,
			};
			size_t lane = 0;
			const auto word = [&](uint64_t value) {
				lanes[lane] = MixMeshSignature(lanes[lane], value);
				lane = (lane + 1) & 3u;
			};
			const auto field = [&](uint64_t tag, size_t count) {
				word(tag);
				word(static_cast<uint64_t>(count));
			};

			field(1, geometry.Positions.size());
			for (const core::Vector3 &value : geometry.Positions) {
				word(PairFloats(value.X, value.Y));
				word(PairFloats(value.Z, 0.0f));
			}
			field(2, geometry.Normals.size());
			for (const core::Vector3 &value : geometry.Normals) {
				word(PairFloats(value.X, value.Y));
				word(PairFloats(value.Z, 0.0f));
			}
			field(3, geometry.UVs.size());
			for (const core::Vector2 &value : geometry.UVs) {
				word(PairFloats(value.X, value.Y));
			}
			field(4, geometry.Colours.size());
			for (const core::Color3 &value : geometry.Colours) {
				word(PairFloats(value.R, value.G));
				word(PairFloats(value.B, 0.0f));
			}
			field(5, geometry.Alphas.size());
			for (const float value : geometry.Alphas) {
				word(PairFloats(value, 0.0f));
			}
			field(6, geometry.Indices.size());
			for (size_t at = 0; at < geometry.Indices.size(); at += 2) {
				const uint64_t first = geometry.Indices[at];
				const uint64_t second = at + 1 < geometry.Indices.size() ? geometry.Indices[at + 1] : 0;
				word(first | (second << 32));
			}

			uint64_t signature = MixMeshSignature(lanes[0], lanes[1]);
			signature = MixMeshSignature(signature, lanes[2]);
			signature = MixMeshSignature(signature, lanes[3]);
			return signature == 0 ? 1 : signature;
		}

		bool SameGeometry(const EditableMesh &mesh, const EditableMeshGeometry &geometry) {
			return mesh.Positions == geometry.Positions && mesh.Normals == geometry.Normals &&
				   mesh.UVs == geometry.UVs && mesh.Colours == geometry.Colours &&
				   mesh.Alphas == geometry.Alphas && mesh.Indices == geometry.Indices;
		}

		// How many vertices are in the mesh, read-only.
		ecs::PropertyDescriptor VertexCountProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("VertexCount");
			property.Type = ecs::PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const EditableMesh *held = store.Get<EditableMesh>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = static_cast<int32_t>(held->Positions.size());
				return true;
			};
			return property;
		}

		// How many triangles are in the mesh, read-only.
		ecs::PropertyDescriptor TriangleCountProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("TriangleCount");
			property.Type = ecs::PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const EditableMesh *held = store.Get<EditableMesh>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = static_cast<int32_t>(held->Indices.size() / 3);
				return true;
			};
			return property;
		}

		// The content name a `MeshId` names this mesh by, read-only - see
		// `EditableMeshContentName`.
		ecs::PropertyDescriptor MeshContentIdProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("ContentId");
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				if (store.Get<EditableMesh>(instance) == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) = EditableMeshContentName(store, instance);
				return true;
			};
			return property;
		}

		ecs::ClassId RegisterEditableMeshClass() {
			// Through `EnsureClassTree`, for `ShaderScriptClass`'s exact reason:
			// this registers the module's components on the way, and a class is
			// a set of component ids that must exist first.
			EnsureClassTree();

			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));

			// **An `Instance` and not a `PVInstance`**, `ShaderScript`'s reason
			// applied here: an `EditableMesh` has no place of its own in the
			// world. What draws it is a `MeshPart` naming its content id.
			const std::array components{ecs::Components::Of<EditableMesh>()};
			const ecs::ClassId editableMesh = ecs::Classes::Register("EditableMesh", instance, components);

			ecs::Classes::Computed(editableMesh, VertexCountProperty());
			ecs::Classes::Computed(editableMesh, TriangleCountProperty());
			ecs::Classes::Computed(editableMesh, MeshContentIdProperty());
			return editableMesh;
		}
	}

	namespace {
		// Whether any collider in the world names `geometry` as a convex hull.
		//
		// The list is gathered once per refresh and only when something asks,
		// which is what `wanted` being an optional says. A world holds a
		// handful of distinct shapes however many parts use them - the whole
		// point of naming them is that they are shared - so the search is over
		// single digits.
		bool
		WantsHull(ecs::Store &store, std::optional<std::vector<core::Name>> &wanted, core::Name geometry) {
			if (!wanted.has_value()) {
				wanted.emplace();
				store.Each<const Collider>([&](ecs::Entity, const Collider &collider) {
					if (collider.Shape != ShapeKind::Hull || !collider.Geometry.IsValid()) {
						return;
					}
					for (const core::Name &already : *wanted) {
						if (already == collider.Geometry) {
							return;
						}
					}
					wanted->push_back(collider.Geometry);
				});
			}

			for (const core::Name &name : *wanted) {
				if (name == geometry) {
					return true;
				}
			}
			return false;
		}
	}

	size_t RefreshEditableMeshCollision(ecs::Store &store) {
		// Refuse the expensive path before copying either resource. Both can hold
		// whole terrain chunks, and copying them merely to discover that every
		// revision is already resident made a stopped editable scene cost
		// milliseconds on every presentation.
		const EditableMeshCollision *heldBaked = store.Resource<EditableMeshCollision>();
		const CollisionShapes *heldShapes = CollisionShapesOf(store);
		std::optional<std::vector<core::Name>> fastWanted;
		size_t live = 0;
		bool dirty = false;
		store.Each<const EditableMesh>([&](ecs::Entity instance, const EditableMesh &mesh) {
			const core::Name name = EditableMeshContentName(store, instance);
			if (!name.IsValid()) {
				return;
			}
			live++;

			const EditableMeshCollision::Baked *known = nullptr;
			if (heldBaked != nullptr) {
				for (const EditableMeshCollision::Baked &row : heldBaked->Rows) {
					if (row.Instance == instance.Id) {
						known = &row;
						break;
					}
				}
			}

			const bool hasTriangles = !mesh.Positions.empty() && mesh.Indices.size() >= 3;
			if (known == nullptr) {
				// A newly created mesh is commonly incomplete for several edits.
				// There is no resident shape to replace or remove until it has a
				// triangle, so copying the catalogue cannot change the answer.
				dirty = dirty || hasTriangles;
				return;
			}
			if (known->Revision != mesh.Revision) {
				dirty = true;
				return;
			}
			if (WantsHull(store, fastWanted, name) &&
				(heldShapes == nullptr || heldShapes->FindHull(name) == nullptr)) {
				dirty = true;
			}
		});
		if (heldBaked != nullptr && heldBaked->Rows.size() != live) {
			dirty = true;
		}
		if (!dirty) {
			return 0;
		}

		// **Both tables are read and written whole, which is what a resource
		// is.** `Store::SetResource` replaces, so the work below builds the two
		// and writes them back once - and a world with no `EditableMesh` in it
		// touches neither.
		EditableMeshCollision baked;
		if (const EditableMeshCollision *held = store.Resource<EditableMeshCollision>()) {
			baked = *held;
		}

		CollisionShapes shapes;
		if (const CollisionShapes *held = CollisionShapesOf(store)) {
			shapes = *held;
		}

		size_t changed = 0;
		struct CollisionBuild {
			core::Name Name;
			const EditableMesh *Source = nullptr;
			bool WantsHull = false;
			collision::TriangleMesh Mesh;
			collision::ConvexHull Hull;
		};
		std::vector<CollisionBuild> builds;

		// Which geometry names a collider asks for as a hull, gathered on the
		// first mesh that needs the answer and not before - a world with no
		// script-built geometry in it must not pay a walk of every collider.
		std::optional<std::vector<core::Name>> wanted;

		// What is in the world this call, so the sweep below can tell a mesh
		// that was destroyed from one that simply did not change.
		std::vector<EditableMeshCollision::Baked> seen;

		store.Each<const EditableMesh>([&](ecs::Entity instance, const EditableMesh &mesh) {
			const core::Name name = EditableMeshContentName(store, instance);
			if (!name.IsValid()) {
				return;
			}

			uint32_t *known = nullptr;
			for (EditableMeshCollision::Baked &row : baked.Rows) {
				if (row.Instance == instance.Id) {
					known = &row.Revision;
					break;
				}
			}

			// The steady state: an integer compare, which is
			// `EditableMeshUploader::Refresh`'s decision and its reason.
			//
			// **Unless a hull is wanted and is not there**, which is a part
			// switched to `ShapeKind::Hull` after its mesh was baked. See the
			// bake below for why a hull is not built until it is asked for.
			if (known != nullptr && *known == mesh.Revision &&
				(shapes.FindHull(name) != nullptr || !WantsHull(store, wanted, name))) {
				seen.push_back(EditableMeshCollision::Baked{instance.Id, mesh.Revision});
				return;
			}

			// **A mesh mid-edit registers nothing rather than an empty shape.**
			// Vertices added and no triangle yet is the ordinary state right
			// after `Instance.new("EditableMesh")`, and a hull of three points
			// with no soup behind it is a collider that stops nothing. The
			// revision is left unrecorded so the next call tries again.
			if (mesh.Positions.empty() || mesh.Indices.size() < 3) {
				// Retried every tick until it becomes valid, which is right and
				// is also what a mesh that never becomes valid looks like: a
				// part with no collision and nothing saying why.
				ENGINE_TRACE_EVERY(
					5.0,
					"an editable mesh has {} position(s) and {} index/indices; no collider yet",
					mesh.Positions.size(),
					mesh.Indices.size()
				);
				return;
			}

			// **The soup always and the hull only when something names one**,
			// which is the difference between this being affordable and not.
			// Measured on a terrain chunk of 4,225 points: the triangle soup
			// costs 1.3 ms and quickhull costs 7.3 ms, and a heightfield's
			// convex hull is a dome over its summit that no collider in that
			// scene will ever ask for. `game::AddCollisionShapes` bakes both
			// because a delivered mesh is baked once at load; this runs on the
			// tick a script builds geometry, and a streamed world builds one a
			// tick for as long as somebody keeps walking.
			//
			// A part that is switched to `Hull` later gets one on the next
			// refresh, because the test below is "wanted and missing" rather
			// than "changed".
			builds.push_back(
				CollisionBuild{
					.Name = name,
					.Source = &mesh,
					.WantsHull = WantsHull(store, wanted, name),
					.Mesh = {},
					.Hull = {},
				}
			);
			seen.push_back(EditableMeshCollision::Baked{instance.Id, mesh.Revision});
		});

		// The store is read-only until this fork-join finishes, so each source
		// pointer remains valid and no world storage reaches a later tick. With
		// several worlds, Universe already runs this same batch concurrently per
		// scene; with one world, several changed meshes use the process pool.
		if (!builds.empty()) {
			parallel::Jobs::For(
				builds.size(),
				1,
				[&builds](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						CollisionBuild &build = builds[index];
						build.Mesh =
							collision::BuildTriangleMesh(build.Source->Positions, build.Source->Indices);
						if (build.WantsHull) {
							build.Hull = collision::BuildConvexHull(build.Source->Positions);
						}
					}
				},
				2
			);
			const parallel::BatchTiming collisionTiming = parallel::Jobs::LastBatch();
			core::FrameGraph::Report(
				"editable collision workers", core::ProfileCategory::Physics, collisionTiming.BusyMilliseconds
			);
		}
		for (CollisionBuild &build : builds) {
			shapes.SetMesh(build.Name, std::move(build.Mesh));
			if (build.WantsHull) {
				shapes.SetHull(build.Name, std::move(build.Hull));
			}
			changed++;
		}

		// **The shapes of meshes that are gone, dropped here.** A streamed
		// world creates and destroys a mesh per chunk, and a table that only
		// grew would hold a hull and a triangle soup for every chunk anybody
		// ever walked past. Nothing can name them again: the content name
		// carries the entity's generation, so even a reused id mints a new one.
		for (const EditableMeshCollision::Baked &was : baked.Rows) {
			bool alive = false;
			for (const EditableMeshCollision::Baked &now : seen) {
				if (now.Instance == was.Instance) {
					alive = true;
					break;
				}
			}
			if (alive) {
				continue;
			}

			shapes.Forget(core::Name("editable-mesh://" + std::to_string(was.Instance)));
			changed++;
		}

		if (changed == 0) {
			return 0;
		}

		baked.Rows = std::move(seen);
		store.SetResource(std::move(baked));
		store.SetResource(std::move(shapes));
		return changed;
	}

	uint64_t EditableMeshSignature(const EditableMeshGeometry &geometry) {
		return SignGeometry(geometry);
	}

	PreparedEditableMesh PrepareEditableMesh(EditableMeshGeometry geometry) {
		PreparedEditableMesh prepared;
		prepared.Valid = geometry.Positions.size() == geometry.Normals.size() &&
						 geometry.Positions.size() == geometry.UVs.size() &&
						 geometry.Positions.size() == geometry.Colours.size() &&
						 geometry.Positions.size() == geometry.Alphas.size() &&
						 geometry.Indices.size() % 3 == 0;

		if (prepared.Valid) {
			for (const uint32_t index : geometry.Indices) {
				if (index >= geometry.Positions.size()) {
					prepared.Valid = false;
					break;
				}
			}
		}

		prepared.Signature = SignGeometry(geometry);
		prepared.Geometry = std::move(geometry);
		return prepared;
	}

	EditableMeshCommit CommitEditableMesh(
		ecs::Store &store, ecs::Entity instance, PreparedEditableMesh prepared, uint32_t expectedRevision
	) {
		if (!prepared.Valid) {
			return EditableMeshCommit::Invalid;
		}

		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return EditableMeshCommit::Missing;
		}
		if (mesh->Revision != expectedRevision) {
			return EditableMeshCommit::Stale;
		}

		const uint64_t currentSignature = mesh->Signature != 0 ? mesh->Signature : SignGeometry(*mesh);
		if (currentSignature == prepared.Signature && SameGeometry(*mesh, prepared.Geometry)) {
			mesh->Signature = currentSignature;
			return EditableMeshCommit::Unchanged;
		}

		mesh->Positions = std::move(prepared.Geometry.Positions);
		mesh->Normals = std::move(prepared.Geometry.Normals);
		mesh->UVs = std::move(prepared.Geometry.UVs);
		mesh->Colours = std::move(prepared.Geometry.Colours);
		mesh->Alphas = std::move(prepared.Geometry.Alphas);
		mesh->Indices = std::move(prepared.Geometry.Indices);
		mesh->Signature = prepared.Signature;
		mesh->Revision++;
		return EditableMeshCommit::Applied;
	}

	EditableMeshCommit
	ReplaceEditableMesh(ecs::Store &store, ecs::Entity instance, EditableMeshGeometry geometry) {
		const EditableMesh *mesh = store.Get<EditableMesh>(instance);
		if (mesh == nullptr) {
			return EditableMeshCommit::Missing;
		}
		return CommitEditableMesh(store, instance, PrepareEditableMesh(std::move(geometry)), mesh->Revision);
	}

	core::Name EditableMeshContentName(const ecs::Store &store, ecs::Entity instance) {
		if (store.Get<EditableMesh>(instance) == nullptr) {
			return {};
		}
		// **The entity's own id, generation included**, so a destroyed
		// `EditableMesh` and whatever the allocator later mints in its place
		// never resolve to the same content name - the exact hazard `rule 4`
		// exists to close everywhere else a handle would otherwise cross.
		return core::Name("editable-mesh://" + std::to_string(instance.Id));
	}

	std::optional<uint32_t> AddVertex(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector3 &position,
		const core::Vector3 &normal,
		const core::Vector2 &uv
	) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return std::nullopt;
		}

		const uint32_t id = static_cast<uint32_t>(mesh->Positions.size());
		mesh->Positions.push_back(position);
		mesh->Normals.push_back(normal);
		mesh->UVs.push_back(uv);
		mesh->Colours.push_back(core::Color3{1.0f, 1.0f, 1.0f});
		mesh->Alphas.push_back(0.0f);
		mesh->Signature = 0;
		mesh->Revision++;
		return id;
	}

	std::optional<uint32_t>
	AddTriangle(ecs::Store &store, ecs::Entity instance, uint32_t a, uint32_t b, uint32_t c) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return std::nullopt;
		}

		const size_t count = mesh->Positions.size();
		if (a >= count || b >= count || c >= count) {
			return std::nullopt;
		}

		const uint32_t id = static_cast<uint32_t>(mesh->Indices.size() / 3);
		mesh->Indices.push_back(a);
		mesh->Indices.push_back(b);
		mesh->Indices.push_back(c);
		mesh->Signature = 0;
		mesh->Revision++;
		return id;
	}

	bool RemoveTriangle(ecs::Store &store, ecs::Entity instance, uint32_t triangle) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return false;
		}

		const size_t triangles = mesh->Indices.size() / 3;
		if (triangle >= triangles) {
			return false;
		}

		// swap-and-pop, three indices at a time - the method's own header
		// carries what this does to the last triangle's id.
		const size_t last = triangles - 1;
		if (triangle != last) {
			mesh->Indices[triangle * 3 + 0] = mesh->Indices[last * 3 + 0];
			mesh->Indices[triangle * 3 + 1] = mesh->Indices[last * 3 + 1];
			mesh->Indices[triangle * 3 + 2] = mesh->Indices[last * 3 + 2];
		}
		mesh->Indices.resize(last * 3);
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool SetVertexPosition(
		ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &position
	) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Positions.size()) {
			return false;
		}
		if (mesh->Positions[vertex] == position) {
			return true;
		}
		mesh->Positions[vertex] = position;
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool
	SetVertexNormal(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &normal) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Normals.size()) {
			return false;
		}
		if (mesh->Normals[vertex] == normal) {
			return true;
		}
		mesh->Normals[vertex] = normal;
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool SetVertexUV(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector2 &uv) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->UVs.size()) {
			return false;
		}
		if (mesh->UVs[vertex] == uv) {
			return true;
		}
		mesh->UVs[vertex] = uv;
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool SetVertexColor(
		ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Color3 &colour, float alpha
	) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Colours.size()) {
			return false;
		}
		if (mesh->Colours[vertex] == colour && mesh->Alphas[vertex] == alpha) {
			return true;
		}
		mesh->Colours[vertex] = colour;
		mesh->Alphas[vertex] = alpha;
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool ClearEditableMesh(ecs::Store &store, ecs::Entity instance) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return false;
		}
		mesh->Positions.clear();
		mesh->Normals.clear();
		mesh->UVs.clear();
		mesh->Colours.clear();
		mesh->Alphas.clear();
		mesh->Indices.clear();
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	ecs::ClassId EditableMeshClass() {
		static const ecs::ClassId editableMesh = RegisterEditableMeshClass();
		return editableMesh;
	}
}
