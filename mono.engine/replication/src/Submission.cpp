#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Submission.hpp>

#include <vector>

namespace engine::replication {

	WriteOutcome WriteComponents(
		ecs::Store &store, const Delta &delta, const std::function<bool(core::Name, ecs::Entity)> &allow
	) {
		WriteOutcome outcome;

		// **Every name resolved before anything is written.** A delta naming one
		// component this build does not have is a delta from a different build,
		// and applying the half of it that resolved would leave the world in a
		// state neither end believes in.
		std::vector<ecs::ComponentId> resolved;
		resolved.reserve(delta.Components.size());
		for (const ComponentDelta &component : delta.Components) {
			const ecs::ComponentId id = ecs::Components::Find(component.Component);
			if (!id.IsValid()) {
				ENGINE_ERROR(
					"replication: a delta names component '{}', which this build has not registered.",
					component.Component.Text()
				);
				outcome.Status = ApplyStatus::UnknownComponent;
				return outcome;
			}
			resolved.push_back(id);
		}

		for (size_t index = 0; index < delta.Components.size(); index++) {
			const ComponentDelta &component = delta.Components[index];
			const ecs::ComponentId id = resolved[index];
			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);

			core::ByteReader values(component.Values);
			std::vector<std::byte> scratch(descriptor.Size);

			for (const ecs::Entity entity : component.Entities) {
				const bool permitted = !allow || allow(component.Component, entity);

				// A tag carries no value, so there is no stream to keep in step
				// and a refusal is simply a write that does not happen.
				if (descriptor.Size == 0) {
					if (permitted) {
						store.SetComponent(entity, id, nullptr);
					} else {
						outcome.Refused++;
					}
					continue;
				}

				descriptor.DefaultConstruct(scratch.data(), 1);
				if (descriptor.Wire.Present()) {
					descriptor.Wire.Read(values, scratch.data(), 1);
				} else {
					descriptor.Read(values, scratch.data(), 1);
				}
				if (values.Failed()) {
					descriptor.Destruct(scratch.data(), 1);
					outcome.Status = ApplyStatus::Malformed;
					return outcome;
				}

				// **Read first, then decide.** The read above is what keeps the
				// stream in step; skipping it for a refused value would put
				// every value after this one on the wrong entity. See the
				// header.
				if (!permitted) {
					outcome.Refused++;
				} else if (store.Alive(entity)) {
					store.SetComponent(entity, id, scratch.data());
				} else {
					outcome.Whole = false;
				}

				descriptor.Destruct(scratch.data(), 1);
			}
		}

		return outcome;
	}

	Delta BuildSubmission(
		const ecs::Store &store,
		uint64_t tick,
		std::span<const ecs::Entity> entities,
		std::span<const core::Name> components
	) {
		Delta delta;
		delta.Tick = tick;

		// **One part, and marked final.** A submission is small by construction
		// - it is what one client owns - so the splitting `Authority` does for a
		// whole world would be machinery for a case that does not arise. If one
		// ever does, it is a client that has been handed more of the world than
		// a client should hold, and the fix is upstream of here.
		delta.Part = 0;
		delta.Final = true;

		for (const core::Name name : components) {
			const ecs::ComponentId id = ecs::Components::Find(name);
			if (!id.IsValid()) {
				continue;
			}

			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);

			ComponentDelta values;
			values.Component = name;

			core::ByteWriter writer;
			for (const ecs::Entity entity : entities) {
				const void *value = store.GetComponent(entity, id);
				if (value == nullptr) {
					continue;
				}

				values.Entities.push_back(entity);

				// A tag has no bytes. Naming the entity is the whole message.
				if (descriptor.Size == 0) {
					continue;
				}

				if (descriptor.Wire.Present()) {
					descriptor.Wire.Write(writer, value, 1);
				} else {
					descriptor.Write(writer, value, 1);
				}
			}

			if (values.Entities.empty()) {
				continue;
			}

			values.Values.assign(writer.Bytes().begin(), writer.Bytes().end());
			delta.Components.push_back(std::move(values));
		}

		return delta;
	}
}
