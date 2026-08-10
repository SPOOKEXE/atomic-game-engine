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
		if (instance == NULL_ENTITY || !world.IsValid()) {
			return EditId{};
		}

		// Keyed on the pair. See `Bound`: the same handle exists in every world
		// at once, so keying on the handle alone hands one id to two instances.
		const Bound bound{world, instance};

		if (const auto found = Ids.find(bound); found != Ids.end()) {
			return EditId{found->second};
		}

		const uint64_t minted = NextId++;
		Ids.emplace(bound, minted);
		Entities.emplace(minted, bound);
		return EditId{minted};
	}

	Entity CommandLog::Resolve(EditId id) const {
		if (!id) {
			return NULL_ENTITY;
		}

		const auto found = Entities.find(id.Value);
		return found == Entities.end() ? NULL_ENTITY : found->second.Instance;
	}

	void CommandLog::Rebind(EditId id, WorldId world, Entity entity) {
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

		const Bound bound{world, entity};
		Entities[id.Value] = bound;
		Ids[bound] = id.Value;
	}

	const char *Describe(FinishOperation operation) {
		switch (operation) {
		case FinishOperation::Commit:
			return "commit";
		case FinishOperation::Cancel:
			return "cancel";
		case FinishOperation::Append:
			return "append";
		}
		// No default label, so adding an operation is a warning here.
		return "?";
	}

	uint64_t CommandLog::NextWaypoint() {
		return ++Waypoints;
	}

	void CommandLog::Push(Command &&command) {
		// **Disabled and replaying are two different silences and both are
		// silences.** A log told to stop recording records nothing; a log
		// applying an undo must not record the opposite edit it is performing,
		// or Ctrl+Z toggles one instance in and out of existence for ever.
		if (!Recording_ || Replaying) {
			return;
		}

		// **One command is one waypoint unless a recording says otherwise**,
		// which is what this editor has always done and what every existing
		// suite asserts. Grouping is something a caller asks for; a default
		// that grouped would make one Ctrl+Z reverse an unbounded run of edits
		// nobody bracketed.
		command.Waypoint = Open.InProgress() ? Open.Waypoint : NextWaypoint();

		Undone.clear();
		Done.push_back(std::move(command));

		if (Done.size() > DEPTH) {
			// **Dropped a whole waypoint at a time**, so the oldest step in the
			// history is never half of one. Erasing one command out of a group
			// would leave an undo that reverses part of an action, which is a
			// worse state than not being able to reach it at all.
			const uint64_t oldest = Done.front().Waypoint;
			size_t drop = 0;
			while (drop < Done.size() && Done[drop].Waypoint == oldest) {
				drop++;
			}
			Done.erase(Done.begin(), Done.begin() + static_cast<ptrdiff_t>(drop));
		}

		// A command outside a recording is its own waypoint and is complete the
		// moment it is pushed. Inside one, the commit is what publishes.
		if (!Open.InProgress() && Listener.Committed) {
			const uint64_t waypoint = Done.back().Waypoint;
			size_t first = Done.size();
			while (first > 0 && Done[first - 1].Waypoint == waypoint) {
				first--;
			}
			Listener.Committed(waypoint, std::span<const Command>(Done).subspan(first));
		}
	}

	std::optional<std::string> CommandLog::TryBeginRecording(std::string name, std::string displayName) {
		if (!Recording_ || Open.InProgress()) {
			// Refused rather than nested. A cancel of an outer recording that
			// contained a committed inner one has no answer that is not
			// surprising, so there is no way to ask the question.
			return std::nullopt;
		}

		Open.Waypoint = NextWaypoint();
		// Derived from the waypoint rather than drawn, because it only has to
		// be unique within this log and a caller that forges one is a caller
		// that could have kept the real one.
		Open.Identifier = "rec-" + std::to_string(Open.Waypoint);
		Open.Name = std::move(name);
		Open.DisplayName = displayName.empty() ? Open.Name : std::move(displayName);
		Open.DepthAtStart = Done.size();

		// A recording is a new action, so the branch that had been undone is
		// not reachable any more — the same rule `Push` applies.
		Undone.clear();
		Cut = Done.size();

		if (Listener.RecordingStarted) {
			Listener.RecordingStarted(Open);
		}
		return Open.Identifier;
	}

	bool CommandLog::IsRecordingInProgress(std::string_view identifier) const {
		if (!Open.InProgress()) {
			return false;
		}
		return identifier.empty() || identifier == Open.Identifier;
	}

	bool CommandLog::FinishRecording(std::string_view identifier, FinishOperation operation) {
		if (!Open.InProgress()) {
			return false;
		}
		// The identifier is ignored for a cancel, which is Roblox's rule and is
		// the one case where not having kept it is the normal situation: a
		// plugin abandoning its own edit knows it has one open and may not know
		// what it was called.
		if (operation != FinishOperation::Cancel && identifier != Open.Identifier) {
			return false;
		}

		const Recording finished = Open;
		Open = {};

		switch (operation) {
		case FinishOperation::Cancel:
			RollBackTo(finished.DepthAtStart);
			break;

		case FinishOperation::Append: {
			// Folded into the waypoint before it. Nothing moves and nothing is
			// re-applied — the commands are already in order and already
			// applied, so all that changes is which step one undo reverses.
			if (finished.DepthAtStart > 0) {
				const uint64_t into = Done[finished.DepthAtStart - 1].Waypoint;
				for (size_t index = finished.DepthAtStart; index < Done.size(); ++index) {
					Done[index].Waypoint = into;
				}
			}
			break;
		}

		case FinishOperation::Commit:
			break;
		}

		if (Listener.RecordingFinished) {
			Listener.RecordingFinished(finished, operation);
		}

		// **Published on commit and on append, and not on cancel.** A peer has
		// to be told about an appended recording — the changes happened — and
		// must not be told about a cancelled one, because the rollback above
		// already put everything back and there is nothing for them to apply.
		if (operation != FinishOperation::Cancel && Listener.Committed &&
			finished.DepthAtStart < Done.size()) {
			Listener.Committed(
				Done[finished.DepthAtStart].Waypoint,
				std::span<const Command>(Done).subspan(finished.DepthAtStart)
			);
		}
		return true;
	}

	void CommandLog::RollBackTo(size_t depth) {
		// Reversed newest first, because the commands in a group are ordered and
		// reversing them forwards would re-create a parent after restoring the
		// child that hangs from it.
		const bool wasReplaying = Replaying;
		Replaying = true;
		while (Done.size() > depth) {
			Command command = std::move(Done.back());
			Done.pop_back();
			if (!Apply(command, false)) {
				ENGINE_WARN("could not roll back '{}' — its subject is gone", command.Description);
			}
		}
		Replaying = wasReplaying;
		Cut = Done.size();

		// **Nothing goes on the redo stack.** A cancel is "this never
		// happened", not "step back", and offering to redo a recording the
		// plugin decided to abandon would be offering a state nobody asked for.
	}

	void CommandLog::SetWaypoint(std::string name) {
		if (!Recording_ || Open.InProgress()) {
			// A recording owns its own grouping. A cut inside one would split
			// an action the caller has already said is one.
			return;
		}

		// **Merges everything since the previous cut into one step and names
		// it.** That is Roblox's semantics — changes between two waypoints are
		// one undo — expressed over a log whose default is one command per
		// step. A plugin that writes forty properties and then calls this gets
		// one Ctrl+Z; the editor, which never calls it, is unaffected, and that
		// asymmetry is the point rather than an oversight.
		if (Cut < Done.size()) {
			const uint64_t into = Done[Cut].Waypoint;
			for (size_t index = Cut; index < Done.size(); ++index) {
				Done[index].Waypoint = into;
				if (!name.empty()) {
					Done[index].Description = name;
				}
			}

			// Published as one, for the same reason a commit is: a peer that
			// applied a merged group in pieces would show a state the author
			// never saw.
			if (Listener.Committed) {
				Listener.Committed(into, std::span<const Command>(Done).subspan(Cut));
			}
		}
		Cut = Done.size();
	}

	void CommandLog::ResetWaypoints() {
		// Nothing is reverted. This establishes a floor rather than restoring
		// one — what it is for is the moment after a load or a save, where
		// reaching back past the current state means reaching into a document
		// that is no longer open.
		Done.clear();
		Undone.clear();
		Cut = 0;
	}

	void CommandLog::SetEnabled(bool enabled) {
		if (Recording_ == enabled) {
			return;
		}
		Recording_ = enabled;
		if (!enabled) {
			// Cleared and not repopulated when it comes back, which is Roblox's
			// behaviour and is the safe one: a log that kept its stacks across a
			// period it had been told not to watch would offer to undo across
			// edits it never saw.
			Open = {};
			Done.clear();
			Undone.clear();
			Cut = 0;
		}
	}

	void CommandLog::Adopt(EditId id, WorldId world, Entity instance) {
		if (!id) {
			return;
		}
		Rebind(id, world, instance);

		// The high-water mark moves with it, so a locally issued id can never
		// collide with a foreign one this log has adopted. Without it two
		// editors that both started at one would hand the same number to two
		// different instances the moment either created something.
		if (id.Value >= NextId) {
			NextId = id.Value + 1;
		}
	}

	size_t CommandLog::ApplyForeign(std::span<const Command> commands) {
		// **Applied and not recorded.** Somebody else's edit is not a step in
		// this author's history: Ctrl+Z is a promise about what *you* did, and
		// an editor that reversed a colleague's change because you pressed it
		// once too often would be an editor nobody could work in.
		const bool wasReplaying = Replaying;
		Replaying = true;

		size_t landed = 0;
		for (const Command &command : commands) {
			Command copy = command;
			if (Apply(copy, true)) {
				landed++;
			}
		}

		Replaying = wasReplaying;
		return landed;
	}

	void CommandLog::RecordCreate(Store &store, WorldId world, Entity created, std::string description) {
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

	void CommandLog::RecordDestroy(Store &store, WorldId world, Entity doomed, std::string description) {
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

					Rebind(command.Subject, command.World, rebuilt);
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
				Rebind(command.Subject, command.World, NULL_ENTITY);
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

		// The whole waypoint, newest first. A command recorded outside a
		// recording is a waypoint of its own, so a log nobody groups behaves
		// exactly as it did before waypoints existed.
		const uint64_t waypoint = Done.back().Waypoint;
		const std::string named(Done.back().Description);

		const bool wasReplaying = Replaying;
		Replaying = true;

		size_t landed = 0;
		while (!Done.empty() && Done.back().Waypoint == waypoint) {
			Command command = std::move(Done.back());
			Done.pop_back();

			// **A command whose subject has gone is dropped, not retried.** The
			// alternative is applying it to whatever now occupies the row, and
			// the store reuses indices — so the wrong instance is not a remote
			// possibility but the ordinary case.
			if (Apply(command, false)) {
				landed++;
			} else {
				ENGINE_WARN("nothing to undo for '{}' — its subject is gone", command.Description);
			}

			// **Kept on the redo stack even when it did not land**, so a group
			// is not silently split in half: a redo of a waypoint whose middle
			// command was unreversible has to face the same partial state
			// rather than a shorter group that looks complete.
			Undone.push_back(std::move(command));
		}

		Replaying = wasReplaying;
		// A cut, so a later `SetWaypoint` cannot merge across an undo into
		// commands the stack no longer holds in the order they were made.
		Cut = Done.size();

		if (landed == 0) {
			return false;
		}
		if (Listener.Undone) {
			Listener.Undone(named);
		}
		return true;
	}

	bool CommandLog::Redo() {
		if (Undone.empty()) {
			return false;
		}

		const uint64_t waypoint = Undone.back().Waypoint;

		const bool wasReplaying = Replaying;
		Replaying = true;

		// Forwards, which is the order they were made in — the reverse of the
		// order `Undo` reversed them in, and the reason `Undone` holds them
		// newest-last.
		size_t landed = 0;
		std::string named;
		while (!Undone.empty() && Undone.back().Waypoint == waypoint) {
			Command command = std::move(Undone.back());
			Undone.pop_back();

			if (Apply(command, true)) {
				landed++;
			} else {
				ENGINE_WARN("nothing to redo for '{}' — its subject is gone", command.Description);
			}

			named = command.Description;
			Done.push_back(std::move(command));
		}

		Replaying = wasReplaying;
		Cut = Done.size();

		if (landed == 0) {
			return false;
		}
		if (Listener.Redone) {
			Listener.Redone(named);
		}
		return true;
	}

	void CommandLog::Forget(WorldId world) {
		if (!world.IsValid()) {
			return;
		}

		// The ids the dropped commands referred to go too. An `EditId` left
		// bound to a handle in a destroyed world is a row that `Track` could
		// hand back for an entity the store has since recycled into something
		// else — the same reasoning as `Rebind`'s reverse-row erase.
		const auto forgetIds = [this](const Command &command) {
			for (const EditId id : {command.Subject, command.OldParent, command.NewParent}) {
				if (!id) {
					continue;
				}
				if (const auto found = Entities.find(id.Value); found != Entities.end()) {
					Ids.erase(found->second);
					Entities.erase(found);
				}
			}
		};

		const auto drop = [&](std::vector<Command> &stack) {
			const auto first = std::remove_if(stack.begin(), stack.end(), [&](const Command &command) {
				if (command.World != world) {
					return false;
				}
				forgetIds(command);
				return true;
			});
			stack.erase(first, stack.end());
		};

		drop(Done);
		drop(Undone);
	}

	void CommandLog::Clear() {
		Done.clear();
		Undone.clear();
		Entities.clear();
		Ids.clear();
		NextId = 1;
	}
}
