#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <array>
#include <string>

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
		ecs::PropertyDescriptor ContentIdProperty() {
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
			ecs::Classes::Computed(editableMesh, ContentIdProperty());
			return editableMesh;
		}
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

	bool SetVertexNormal(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &normal) {
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
