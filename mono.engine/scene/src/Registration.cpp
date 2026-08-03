#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceTable.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::scene {

	namespace {
		// `Surface`, `Visual` and `SurfaceTable` all hold a `core::Name`, and a
		// name's id is a counter this process assigned in first-seen order. The
		// raw object representation would write that counter, and a reading
		// process would resolve it to whatever string happened to take the same
		// number — which is not a corrupt file, it is a file that loads and is
		// wrong. So each of them is written as text.

		void WriteSurfaces(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *surfaces = static_cast<const Surface *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(surfaces[index].Material);
			}
		}

		void ReadSurfaces(core::ByteReader &reader, void *destination, size_t count) {
			auto *surfaces = static_cast<Surface *>(destination);
			for (size_t index = 0; index < count; index++) {
				surfaces[index].Material = reader.ReadName();
			}
		}

		void WriteVisuals(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *visuals = static_cast<const Visual *>(source);
			for (size_t index = 0; index < count; index++) {
				const Visual &visual = visuals[index];
				writer.WriteFloat(visual.Tint.R);
				writer.WriteFloat(visual.Tint.G);
				writer.WriteFloat(visual.Tint.B);
				writer.WriteName(visual.Mesh);
				writer.WriteName(visual.Material);
				writer.WriteBool(visual.Visible);
			}
		}

		void ReadVisuals(core::ByteReader &reader, void *destination, size_t count) {
			auto *visuals = static_cast<Visual *>(destination);
			for (size_t index = 0; index < count; index++) {
				Visual &visual = visuals[index];
				visual.Tint.R = reader.ReadFloat();
				visual.Tint.G = reader.ReadFloat();
				visual.Tint.B = reader.ReadFloat();
				visual.Mesh = reader.ReadName();
				visual.Material = reader.ReadName();
				visual.Visible = reader.ReadBool();
			}
		}

		void WriteSurfaceTables(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *tables = static_cast<const SurfaceTable *>(source);
			for (size_t index = 0; index < count; index++) {
				const SurfaceTable &table = tables[index];
				writer.WriteUInt32(static_cast<uint32_t>(table.Rows.size()));

				// In stored order rather than sorted. The order is program
				// order — the sequence the scene registered materials in — so
				// two runs of one scene write identical bytes, and sorting here
				// would only hide a table that had been built differently.
				for (const SurfaceRow &row : table.Rows) {
					writer.WriteName(row.Material);
					writer.WriteFloat(row.Properties.Friction);
					writer.WriteFloat(row.Properties.Restitution);
				}
			}
		}

		void ReadSurfaceTables(core::ByteReader &reader, void *destination, size_t count) {
			auto *tables = static_cast<SurfaceTable *>(destination);
			for (size_t index = 0; index < count; index++) {
				SurfaceTable &table = tables[index];
				table.Rows.clear();

				const uint32_t rows = reader.ReadUInt32();
				for (uint32_t at = 0; at < rows && !reader.Failed(); at++) {
					SurfaceRow row;
					row.Material = reader.ReadName();
					row.Properties.Friction = reader.ReadFloat();
					row.Properties.Restitution = reader.ReadFloat();
					table.Rows.push_back(row);
				}
			}
		}
	}

	void RegisterSceneComponents() {
		// Order is deliberate and not alphabetical: it is the order the plan
		// lists them in, and it decides component ids, which decide the order
		// archetypes iterate their columns in. Reordering these lines changes
		// the order rows are visited across the whole engine, so a
		// floating-point sum over them can come out differently. Add at the
		// end.
		ecs::Components::Register<Transform>("scene.Transform");
		ecs::Components::Register<PreviousTransform>("scene.PreviousTransform");
		ecs::Components::Register<Bounds>("scene.Bounds");
		ecs::Components::Register<Motion>("scene.Motion");
		ecs::Components::Register<RigidBody>("scene.RigidBody");
		ecs::Components::Register<Collider>("scene.Collider");
		ecs::Components::Register<Surface>("scene.Surface", WriteSurfaces, ReadSurfaces);
		ecs::Components::Register<Visual>("scene.Visual", WriteVisuals, ReadVisuals);
		ecs::Components::Register<Camera>("scene.Camera");
		ecs::Components::Register<QuickHash>("scene.QuickHash");

		// The resources. A resource is keyed by a component id too, so one that
		// is never registered here would be minted by the first
		// `Store::SetResource` — under the compiler's spelling of the type, and
		// aborting outright once the table is sealed.
		ecs::Components::Register<SurfaceTable>("scene.SurfaceTable", WriteSurfaceTables, ReadSurfaceTables);
		ecs::Components::Register<ActiveCamera>("scene.ActiveCamera");
		ecs::Components::Register<WorldBounds>("scene.WorldBounds");
	}

	void RegisterSceneClasses() {
		// `PartClass` registers the components first and the whole tree on its
		// first call, so this is the same registration under the name a caller
		// looks for rather than a second one.
		(void)PartClass();
	}
}
