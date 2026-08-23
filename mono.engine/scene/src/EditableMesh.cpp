#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine::scene {

	namespace {
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
		bool dirty = heldBaked == nullptr;
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

			if (known == nullptr || known->Revision != mesh.Revision) {
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
			shapes.SetMesh(name, collision::BuildTriangleMesh(mesh.Positions, mesh.Indices));
			if (WantsHull(store, wanted, name)) {
				shapes.SetHull(name, collision::BuildConvexHull(mesh.Positions));
			}

			seen.push_back(EditableMeshCollision::Baked{instance.Id, mesh.Revision});
			changed++;
		});

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
		mesh->Positions[vertex] = position;
		mesh->Revision++;
		return true;
	}

	bool
	SetVertexNormal(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &normal) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Normals.size()) {
			return false;
		}
		mesh->Normals[vertex] = normal;
		mesh->Revision++;
		return true;
	}

	bool SetVertexUV(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector2 &uv) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->UVs.size()) {
			return false;
		}
		mesh->UVs[vertex] = uv;
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
		mesh->Colours[vertex] = colour;
		mesh->Alphas[vertex] = alpha;
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
		mesh->Revision++;
		return true;
	}

	ecs::ClassId EditableMeshClass() {
		static const ecs::ClassId editableMesh = RegisterEditableMeshClass();
		return editableMesh;
	}
}
