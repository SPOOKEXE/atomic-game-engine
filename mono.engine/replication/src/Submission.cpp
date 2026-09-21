#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Submission.hpp>

#include <cstring>
#include <vector>

namespace engine::replication {
	namespace {
		uint64_t HashBytes(const void *bytes, size_t count) {
			constexpr uint64_t OFFSET = 1469598103934665603ull;
			constexpr uint64_t PRIME = 1099511628211ull;

			const auto *cursor = static_cast<const unsigned char *>(bytes);
			uint64_t hash = OFFSET;

			// Match authority signatures without assuming component alignment.
			size_t index = 0;
			for (; index + sizeof(uint64_t) <= count; index += sizeof(uint64_t)) {
				uint64_t word = 0;
				std::memcpy(&word, cursor + index, sizeof(word));
				hash = (hash ^ word) * PRIME;
			}

			if (index < count) {
				uint64_t word = 0;
				std::memcpy(&word, cursor + index, count - index);
				hash = (hash ^ word) * PRIME;
			}

			return hash;
		}

	}

	WriteOutcome WriteComponents(
		ecs::Store &store, const Delta &delta, const std::function<bool(core::Name, ecs::Entity)> &allow
	) {
		WriteOutcome outcome;
		static const core::LogCategory rowTrace("replication-row");
		const bool traceRows = rowTrace.Enabled(core::LogLevel::Trace);

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

		// **The one component that is a structure rather than a value.** See
		// `ecs.Hierarchy`'s registration: only `Parent` crosses, and the tree it
		// belongs to is rebuilt here through `SetParent` rather than written
		// over. A store's sibling links are its own.
		const ecs::ComponentId hierarchy = ecs::Components::Of<ecs::Hierarchy>();

		for (size_t index = 0; index < delta.Components.size(); index++) {
			const ComponentDelta &component = delta.Components[index];
			const ecs::ComponentId id = resolved[index];
			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);

			const bool traceComponent = traceRows && component.Component.Text() == "scene.Humanoid";
			core::ByteReader values(component.Values);
			std::vector<std::byte> scratch(descriptor.Size);

			for (const ecs::Entity entity : component.Entities) {
				const bool permitted = !allow || allow(component.Component, entity);

				// A tag carries no value, so there is no stream to keep in step
				// and a refusal is simply a write that does not happen.
				if (descriptor.Size == 0) {
					if (!permitted) {
						outcome.Refused++;
					} else if (!store.Alive(entity)) {
						outcome.Whole = false;
					} else {
						store.SetComponent(entity, id, nullptr);
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

				uint64_t beforeHash = 0;
				uint64_t incomingHash = 0;
				if (traceComponent) {
					const auto *before = store.GetComponent(entity, id);
					beforeHash = before ? HashBytes(before, descriptor.Size) : 0;
					incomingHash = HashBytes(scratch.data(), descriptor.Size);
				}

				// **Read first, then decide.** The read above is what keeps the
				// stream in step; skipping it for a refused value would put
				// every value after this one on the wrong entity. See the
				// header.
				if (!permitted) {
					outcome.Refused++;
				} else if (!store.Alive(entity)) {
					outcome.Whole = false;
				} else if (id == hierarchy) {
					// **Linked, not written.** `SetParent` unlinks from wherever
					// this row currently hangs and appends it where the sender
					// says, keeping every list in this store consistent by
					// construction. Writing the component instead would put the
					// sender's `FirstChild` and `NextSibling` into a tree this
					// store maintains, which is how a sibling list comes to
					// contain itself.
					const auto *node =
						static_cast<const ecs::Hierarchy *>(static_cast<const void *>(scratch.data()));

					// **A node first, and a blank one.** `SetParent` refuses a
					// row that has none, and an entity a `Structure` message
					// just created is bare - so without this every instance
					// arriving after a join stayed a root. It unlinks from
					// whatever the row currently names, too, so the node it is
					// given has to be this store's or nobody's. A row that
					// already has one keeps it.
					static const ecs::Hierarchy BLANK{};
					if (!store.Has<ecs::Hierarchy>(entity)) {
						store.SetComponent(entity, id, &BLANK);
					}

					// **Deferred on the answer rather than on a guess at it.**
					// `SetParent` refuses for more reasons than a parent that
					// is not here: one that is here but has no node yet, and one
					// that is already below this row. Asking it and keeping
					// what it would not take covers every reason at once.
					if (!store.SetParent(entity, node->Parent) && node->Parent != ecs::NULL_ENTITY) {
						outcome.Deferred.push_back(DeferredParent{entity, node->Parent});
					}
				} else {
					store.SetComponent(entity, id, scratch.data());
				}

				if (traceComponent && (beforeHash != incomingHash || !permitted || !store.Alive(entity))) {
					const auto *after = store.GetComponent(entity, id);
					ENGINE_LOG(
						core::LogLevel::Trace,
						"replication-row",
						"stage=apply store={} identity={} entity={} tick={} permitted={} alive={} before={} "
						"incoming={} after={}",
						store.Name(),
						store.Identity(),
						entity.Id,
						delta.Tick,
						permitted,
						store.Alive(entity),
						beforeHash,
						incomingHash,
						after ? HashBytes(after, descriptor.Size) : 0
					);
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
