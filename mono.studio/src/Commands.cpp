#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>

#include <studio/Commands.hpp>

#include <utility>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;
	using engine::game::PropertyValue;
	using engine::world::WorldId;

	namespace {
		// Finds the descriptor for a property on whatever class an instance is.
		//
		// **Looked up at apply time rather than stored in the command**, because
		// a `PropertyDescriptor` is a reference into a class table and a command
		// outlives the frame it was recorded in. The name is the stable thing;
		// everything else is derived from it.
		//
		// @param store    The world's store.
		// @param instance The instance.
		// @param property Which property.
		// @return The descriptor, or null when this class has no such property.
		const engine::ecs::PropertyDescriptor *
		DescriptorOf(Store &store, Entity instance, engine::core::Name property) {
			const engine::ecs::ClassId klass = store.ClassOf(instance);
			if (!klass.IsValid()) {
				return nullptr;
			}

			for (const engine::ecs::PropertyDescriptor &descriptor :
				 engine::ecs::Classes::Describe(klass).Properties) {
				if (descriptor.Name == property) {
					return &descriptor;
				}
			}

			return nullptr;
		}
	}

	EditId CommandLog::Track(WorldId world, Entity instance) {
		(void)world;

		if (instance == NULL_ENTITY) {
			return EditId{};
		}

		if (const auto found = Ids.find(instance); found != Ids.end()) {
			return EditId{found->second};
		}

		const uint64_t minted = NextId++;
		Ids.emplace(instance, minted);
		Entities.emplace(minted, instance);
		return EditId{minted};
	}

	Entity CommandLog::Resolve(EditId id) const {
		if (!id) {
			return NULL_ENTITY;
		}

		const auto found = Entities.find(id.Value);
		return found == Entities.end() ? NULL_ENTITY : found->second;
	}

	void CommandLog::Rebind(EditId id, Entity entity) {
		if (!id) {
			return;
		}

		// The old entity's reverse row goes first. Leaving it would let `Track`
		// hand out this id for a handle that has been recycled into something
		// else entirely — the store reuses indices, so a stale reverse row is
		// not merely useless but actively wrong.
		if (const auto previous = Entities.find(id.Value); previous != Entities.end()) {
			Ids.erase(previous->second);
		}

		if (entity == NULL_ENTITY) {
			Entities.erase(id.Value);
			return;
		}

		Entities[id.Value] = entity;
		Ids[entity] = id.Value;
	}

	void CommandLog::Push(Command &&command) {
		Undone.clear();
		Done.push_back(std::move(command));

		if (Done.size() > DEPTH) {
			Done.erase(Done.begin());
		}
	}

	void CommandLog::RecordCreate(
		Store &store, WorldId world, Entity created, std::string description
	) {
		if (created == NULL_ENTITY || !store.Alive(created)) {
			return;
		}

		Command command;
		command.Kind = CommandKind::Create;
		command.World = world;
		command.Subject = Track(world, created);
		command.OldParent = Track(world, store.ParentOf(created));
		command.Document = engine::game::WriteInstanceDocument(store, created);
		command.Description = std::move(description);

		Push(std::move(command));
	}

	void CommandLog::RecordDestroy(
		Store &store, WorldId world, Entity doomed, std::string description
	) {
		if (doomed == NULL_ENTITY || !store.Alive(doomed)) {
			return;
		}

		Command command;
		command.Kind = CommandKind::Destroy;
		command.World = world;
		command.Subject = Track(world, doomed);
		command.OldParent = Track(world, store.ParentOf(doomed));
		command.Document = engine::game::WriteInstanceDocument(store, doomed);
		command.Description = std::move(description);

		Push(std::move(command));
	}

	void CommandLog::RecordReparent(
		WorldId world, Entity instance, Entity from, Entity to, std::string description
	) {
		if (instance == NULL_ENTITY) {
			return;
		}

		Command command;
		command.Kind = CommandKind::Reparent;
		command.World = world;
		command.Subject = Track(world, instance);
		command.OldParent = Track(world, from);
		command.NewParent = Track(world, to);
		command.Description = std::move(description);

		Push(std::move(command));
	}

	void CommandLog::RecordProperty(
		WorldId world,
		Entity instance,
		engine::core::Name property,
		const PropertyValue &before,
		const PropertyValue &after,
		std::string description
	) {
		if (instance == NULL_ENTITY || !property.IsValid()) {
			return;
		}

		// See the declaration: a properties panel submits on every keystroke
		// that parses, and an undo stack full of writes that changed nothing is
		// one somebody has to press through rather than use.
		if (engine::game::ValuesEqual(before, after)) {
			return;
		}

		Command command;
		command.Kind = CommandKind::Property;
		command.World = world;
		command.Subject = Track(world, instance);
		command.Property = property;
		command.Before = before;
		command.After = after;
		command.Description = std::move(description);

		Push(std::move(command));
	}

	bool CommandLog::Apply(Command &command, bool forward) {
		if (Universe == nullptr || !command.World.IsValid()) {
			return false;
		}

		// **A rebuild is a create and its reverse is a destroy, so the two kinds
		// are one operation read in two directions.** Writing them as four cases
		// would be four places for the parent to be resolved differently.
		//
		// Redoing a `Create` rebuilds and undoing it destroys; a `Destroy` is
		// the same two the other way round.
		const bool rebuilding = (command.Kind == CommandKind::Create) == forward;

		bool landed = false;

		Universe->Enter(command.World, [&](Store &store) {
			switch (command.Kind) {
			case CommandKind::Create:
			case CommandKind::Destroy: {
				if (rebuilding) {
					// The parent may itself have been rebuilt since, which is
					// what the id table is for. A parent that no longer resolves
					// rebuilds as a root rather than refusing — the subtree is
					// still the author's work and losing it to a missing parent
					// would be the delete-nobody-asked-for this log exists to
					// undo.
					const Entity parent = Resolve(command.OldParent);

					std::string error;
					const Entity rebuilt = engine::game::ReadInstanceDocument(
						store, command.Document, store.Alive(parent) ? parent : NULL_ENTITY, error
					);

					if (rebuilt == NULL_ENTITY) {
						ENGINE_WARN("undo could not rebuild '{}': {}", command.Description, error);
						return;
					}

					Rebind(command.Subject, rebuilt);
					landed = true;
					return;
				}

				const Entity subject = Resolve(command.Subject);
				if (subject == NULL_ENTITY || !store.Alive(subject)) {
					return;
				}

				// Photographed again rather than trusting the document recorded
				// at the time: everything done to the subtree since is part of
				// what the other direction has to give back.
				command.Document = engine::game::WriteInstanceDocument(store, subject);
				command.OldParent = Track(command.World, store.ParentOf(subject));

				store.DestroyInstance(subject);
				Rebind(command.Subject, NULL_ENTITY);
				landed = true;
				return;
			}

			case CommandKind::Reparent: {
				const Entity subject = Resolve(command.Subject);
				if (subject == NULL_ENTITY || !store.Alive(subject)) {
					return;
				}

				const Entity target = Resolve(forward ? command.NewParent : command.OldParent);
				if (target != NULL_ENTITY && !store.Alive(target)) {
					return;
				}

				landed = store.SetParent(subject, target);
				return;
			}

			case CommandKind::Property: {
				const Entity subject = Resolve(command.Subject);
				if (subject == NULL_ENTITY || !store.Alive(subject)) {
					return;
				}

				const engine::ecs::PropertyDescriptor *descriptor =
					DescriptorOf(store, subject, command.Property);
				if (descriptor == nullptr) {
					return;
				}

				landed = engine::game::WriteProperty(
					store, subject, *descriptor, forward ? command.After : command.Before
				);
				return;
			}
			}
		});

		return landed;
	}

	bool CommandLog::Undo() {
		if (Done.empty()) {
			return false;
		}

		Command command = std::move(Done.back());
		Done.pop_back();

		// **A command whose subject has gone is dropped, not retried.** The
		// alternative is applying it to whatever now occupies the row, and the
		// store reuses indices — so the wrong instance is not a remote
		// possibility but the ordinary case.
		if (!Apply(command, false)) {
			ENGINE_WARN("nothing to undo for '{}' — its subject is gone", command.Description);
			return false;
		}

		Undone.push_back(std::move(command));
		return true;
	}

	bool CommandLog::Redo() {
		if (Undone.empty()) {
			return false;
		}

		Command command = std::move(Undone.back());
		Undone.pop_back();

		if (!Apply(command, true)) {
			ENGINE_WARN("nothing to redo for '{}' — its subject is gone", command.Description);
			return false;
		}

		Done.push_back(std::move(command));
		return true;
	}

	void CommandLog::Clear() {
		Done.clear();
		Undone.clear();
		Entities.clear();
		Ids.clear();
		NextId = 1;
	}
}
