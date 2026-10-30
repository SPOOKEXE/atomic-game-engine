#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Document.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/ProceduralJobs.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <studio/Commands.hpp>
#include <type_traits>
#include <utility>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;
	using engine::game::PropertyValue;
	using engine::world::WorldId;

	namespace {
		template <typename T> void ReserveOne(std::vector<T> &entries) {
			if (entries.size() < entries.capacity()) return;
			if (entries.size() == entries.max_size()) throw std::length_error("history is full");
			const size_t growth =
				entries.capacity() > entries.max_size() / 2 ? entries.max_size() : entries.capacity() * 2;
			entries.reserve(std::max(entries.size() + 1, growth));
		}
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

		bool ApplyComponent(
			Store &store, Entity instance, engine::core::Name name, std::span<const std::byte> bytes
		) {
			const engine::ecs::ComponentId id = engine::ecs::Components::Find(name);
			if (!id.IsValid() || !store.HasComponent(instance, id)) return false;
			const engine::ecs::TypeDescriptor &type = engine::ecs::Components::Describe(id);
			if (!type.Serialisable || type.Size == 0 || type.DefaultConstruct == nullptr ||
				type.Destruct == nullptr || type.Read == nullptr)
				return false;
			void *value = ::operator new(type.Size, std::align_val_t(type.Alignment));
			type.DefaultConstruct(value, 1);
			engine::core::ByteReader reader(bytes);
			type.Read(reader, value, 1);
			const bool valid = !reader.Failed() && reader.AtEnd();
			if (valid) store.SetComponent(instance, id, value);
			type.Destruct(value, 1);
			::operator delete(value, std::align_val_t(type.Alignment));
			return valid;
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
		try {
			Entities.emplace(minted, bound);
		} catch (...) {
			Ids.erase(bound);
			throw;
		}
		return EditId{minted};
	}

	Entity CommandLog::Resolve(EditId id) const {
		if (!id) {
			return NULL_ENTITY;
		}

		const auto found = Entities.find(id.Value);
		return found == Entities.end() ? NULL_ENTITY : found->second.Instance;
	}

	Entity CommandLog::ResolveInWorld(EditId id, WorldId world) const {
		if (!id || !world.IsValid()) return NULL_ENTITY;
		const auto found = Entities.find(id.Value);
		if (found == Entities.end() || found->second.World != world) return NULL_ENTITY;
		return found->second.Instance;
	}

	void CommandLog::Rebind(EditId id, WorldId world, Entity entity) {
		if (!id) {
			return;
		}

		// The old entity's reverse row goes first. Leaving it would let `Track`
		// hand out this id for a handle that has been recycled into something
		// else entirely - the store reuses indices, so a stale reverse row is
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

	CommandLog::Batch::Batch(CommandLog &log, bool group) : Owner(&log) {
		if (group && log.Recording_ && !log.Replaying && !log.Open.InProgress() && log.BatchWaypoint == 0) {
			if (log.BatchDepth != 0) ReserveOne(log.BatchPublications);
			log.BatchSnapshot.clear();
			log.BatchWaypoint = log.NextWaypoint();
			OwnsWaypoint = true;
		}
		++log.BatchDepth;
	}

	CommandLog::Batch::~Batch() {
		try {
			Finish();
		} catch (const std::exception &error) {
			ENGINE_ERROR("could not publish editor action: {}", error.what());
		} catch (...) {
			ENGINE_ERROR("could not publish editor action: unknown exception");
		}
	}

	void CommandLog::Batch::Finish() {
		if (Owner == nullptr) return;
		CommandLog *log = Owner;
		Owner = nullptr;
		struct ReleaseDepth {
			CommandLog &Log;
			~ReleaseDepth() {
				--Log.BatchDepth;
			}
		} release{*log};
		std::exception_ptr firstFailure;
		try {
			if (OwnsWaypoint) log->FinishBatch(log->BatchDepth > 1);
		} catch (...) {
			firstFailure = std::current_exception();
		}
		if (log->BatchDepth == 1) {
			try {
				log->FlushBatchPublications();
			} catch (...) {
				if (!firstFailure) firstFailure = std::current_exception();
			}
		}
		if (firstFailure) std::rethrow_exception(firstFailure);
	}

	void CommandLog::FinishBatch(bool defer) {
		const uint64_t waypoint = std::exchange(BatchWaypoint, 0);
		std::vector<Command> snapshot = std::move(BatchSnapshot);
		if (waypoint == 0 || snapshot.empty() || Done.empty() || Done.back().Waypoint != waypoint) return;
		Cut = Done.size();
		if (defer) {
			BatchPublications.push_back(BatchPublication{waypoint, std::move(snapshot)});
		} else if (Listener.Committed) {
			struct PublicationScope {
				size_t &Depth;
				explicit PublicationScope(size_t &depth) : Depth(depth) {
					++Depth;
				}
				~PublicationScope() {
					--Depth;
				}
			} publishing{BatchPublishing};
			Listener.Committed(waypoint, snapshot);
		}
	}

	void CommandLog::FlushBatchPublications() {
		struct DrainScope {
			bool &Flag;
			bool Previous;
			explicit DrainScope(bool &flag) : Flag(flag), Previous(flag) {
				Flag = true;
			}
			~DrainScope() {
				Flag = Previous;
			}
		} draining{BatchDraining};
		std::exception_ptr firstFailure;
		while (!BatchPublications.empty()) {
			BatchPublication item = std::move(BatchPublications.front());
			BatchPublications.erase(BatchPublications.begin());
			if (Listener.Committed) {
				struct PublicationScope {
					size_t &Depth;
					explicit PublicationScope(size_t &depth) : Depth(depth) {
						++Depth;
					}
					~PublicationScope() {
						--Depth;
					}
				} publishing{BatchPublishing};
				try {
					Listener.Committed(item.Waypoint, item.Commands);
				} catch (...) {
					if (!firstFailure) firstFailure = std::current_exception();
				}
			}
		}
		if (firstFailure) std::rethrow_exception(firstFailure);
	}

	void CommandLog::Push(Command &&command, std::vector<Command> *preparedSnapshot) {
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
		command.Waypoint =
			Open.InProgress() ? Open.Waypoint : (BatchWaypoint != 0 ? BatchWaypoint : NextWaypoint());

		// The batch snapshot is prepared before the caller mutates the world, so
		// publication cannot fail to allocate after an edit has already landed.
		const bool batching = command.Waypoint == BatchWaypoint && BatchWaypoint != 0;
		const bool defer = !Open.InProgress() && !batching && BatchDepth != 0 &&
						   (BatchPublishing == 0 || BatchDraining || preparedSnapshot != nullptr);
		std::vector<Command> localSnapshot;
		if (defer) {
			if (preparedSnapshot == nullptr) {
				ReserveOne(BatchPublications);
				localSnapshot.push_back(command);
				preparedSnapshot = &localSnapshot;
			}
			preparedSnapshot->front().Waypoint = command.Waypoint;
		}
		if (batching) {
			if (preparedSnapshot != nullptr) {
				preparedSnapshot->front().Waypoint = command.Waypoint;
				BatchSnapshot.push_back(std::move(preparedSnapshot->front()));
			} else {
				BatchSnapshot.push_back(command);
			}
		}
		try {
			Done.push_back(std::move(command));
		} catch (...) {
			if (batching) BatchSnapshot.pop_back();
			throw;
		}
		Undone.clear();
		if (defer)
			BatchPublications.push_back(BatchPublication{Done.back().Waypoint, std::move(*preparedSnapshot)});

		while (Done.size() > DEPTH) {
			// **Dropped a whole waypoint at a time**, so the oldest step in the
			// history is never half of one. Erasing one command out of a group
			// would leave an undo that reverses part of an action, which is a
			// worse state than not being able to reach it at all.
			const uint64_t oldest = Done.front().Waypoint;
			// An open recording must remain reversible even when it alone is larger
			// than the normal limit. A later edit can evict the completed waypoint.
			if ((Open.InProgress() && oldest == Open.Waypoint) || oldest == BatchWaypoint) break;
			size_t drop = 0;
			while (drop < Done.size() && Done[drop].Waypoint == oldest) {
				drop++;
			}
			Done.erase(Done.begin(), Done.begin() + static_cast<ptrdiff_t>(drop));
			Cut = Cut > drop ? Cut - drop : 0;
			if (Open.InProgress()) {
				Open.DepthAtStart = Open.DepthAtStart > drop ? Open.DepthAtStart - drop : 0;
			}
		}

		if (Done.back().Kind == CommandKind::RenameWorld) Cut = Done.size();

		// A command outside a recording is its own waypoint and is complete the
		// moment it is pushed. Inside one, the commit is what publishes.
		if (!Open.InProgress() && BatchWaypoint == 0 && !defer && (BatchDepth == 0 || BatchPublishing != 0) &&
			Listener.Committed) {
			const uint64_t waypoint = Done.back().Waypoint;
			size_t first = Done.size();
			while (first > 0 && Done[first - 1].Waypoint == waypoint) {
				first--;
			}
			Listener.Committed(waypoint, std::span<const Command>(Done).subspan(first));
		}
	}

	std::optional<std::string> CommandLog::TryBeginRecording(std::string name, std::string displayName) {
		if (!Recording_ || Open.InProgress() || BatchDepth != 0) {
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

		// Opening a recording is not an edit. `Push` clears redo when a change
		// actually lands, so an empty or read-only recording preserves it.
		Cut = Done.size();

		if (Listener.RecordingStarted) {
			const Recording started = Open;
			Listener.RecordingStarted(started);
			// A listener may finish this recording and start another one.
			if (!Open.InProgress() || Open.Identifier != started.Identifier) return std::nullopt;
		}
		return Open.Identifier;
	}

	bool CommandLog::IsRecordingInProgress(std::string_view identifier) const {
		if (!Open.InProgress()) {
			return false;
		}
		return identifier.empty() || identifier == Open.Identifier;
	}

	bool CommandLog::FinishRecording(
		std::string_view identifier, FinishOperation operation, const std::function<void()> &afterRollback
	) {
		if (BatchDepth != 0 || !Open.InProgress()) {
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
			// An edit already undone during this recording belongs to the cancelled
			// recording too. Keep unrelated redo entries in their original order.
			std::erase_if(Undone, [&](const Command &command) {
				return command.Waypoint == finished.Waypoint;
			});
			if (afterRollback) afterRollback();
			break;

		case FinishOperation::Append: {
			// Folded into the waypoint before it. Nothing moves and nothing is
			// re-applied - the commands are already in order and already
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

		// **Published on commit and on append, and not on cancel.** A peer has
		// to be told about an appended recording - the changes happened - and
		// must not be told about a cancelled one, because the rollback above
		// already put everything back and there is nothing for them to apply.
		// Publish before plugin callbacks can change the history being published.
		if (operation != FinishOperation::Cancel && Listener.Committed &&
			finished.DepthAtStart < Done.size()) {
			Listener.Committed(
				Done[finished.DepthAtStart].Waypoint,
				std::span<const Command>(Done).subspan(finished.DepthAtStart)
			);
		}
		if (Listener.RecordingFinished) {
			Listener.RecordingFinished(finished, operation);
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
				ENGINE_WARN("could not roll back '{}' - its subject is gone", command.Description);
			}
		}
		Replaying = wasReplaying;
		Cut = Done.size();

		// **Nothing goes on the redo stack.** A cancel is "this never
		// happened", not "step back", and offering to redo a recording the
		// plugin decided to abandon would be offering a state nobody asked for.
	}

	void CommandLog::SetWaypoint(std::string name) {
		if (!Recording_ || Open.InProgress() || BatchDepth != 0) {
			// A recording owns its own grouping. A cut inside one would split
			// an action the caller has already said is one.
			return;
		}

		// **Merges everything since the previous cut into one step and names
		// it.** That is Roblox's semantics - changes between two waypoints are
		// one undo - expressed over a log whose default is one command per
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
		if (BatchDepth != 0) return;
		// Nothing is reverted. This establishes a floor rather than restoring
		// one - what it is for is the moment after a load or a save, where
		// reaching back past the current state means reaching into a document
		// that is no longer open.
		Done.clear();
		Undone.clear();
		Cut = 0;
		if (Open.InProgress()) Open.DepthAtStart = 0;
	}

	void CommandLog::SetEnabled(bool enabled) {
		if (BatchDepth != 0) return;
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

	EditId CommandLog::Mint() {
		return EditId{NextId++};
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

	void CommandLog::CaptureInstanceDocument(Command &command, Store &store, Entity root) {
		std::vector<engine::game::InstanceDocumentIdentity> identities;
		command.Document = engine::game::WriteInstanceDocument(store, root, &identities);
		command.DocumentBindings.clear();
		for (const auto &identity : identities) {
			const auto found = Ids.find(Bound{command.World, identity.Instance});
			if (found != Ids.end())
				command.DocumentBindings.push_back({identity.LocalId, EditId{found->second}});
		}
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
		CaptureInstanceDocument(command, store, created);
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
		CaptureInstanceDocument(command, store, doomed);
		command.Description = std::move(description);

		Push(std::move(command));
	}

	bool CommandLog::RecordUiDocumentImport(
		Store &store,
		WorldId world,
		const engine::gui::UiDocument &document,
		std::span<const Entity> roots,
		std::span<const Entity> themes,
		std::span<const Entity> parents,
		std::string description
	) {
		if (!world.IsValid() || (roots.empty() && themes.empty()) || roots.size() != parents.size())
			return false;
		for (const Entity root : roots)
			if (root == NULL_ENTITY || !store.Alive(root)) return false;
		for (const Entity theme : themes)
			if (theme == NULL_ENTITY || !store.Alive(theme)) return false;
		for (const Entity parent : parents)
			if (parent == NULL_ENTITY || !store.Alive(parent)) return false;

		engine::core::ByteWriter writer(0, engine::gui::DocumentLimits::HARD_MAXIMUM_BINARY_BYTES);
		engine::gui::DocumentReport report;
		if (!engine::gui::EncodeDocument(document, writer, report)) return false;

		Command command;
		command.Kind = CommandKind::UiDocumentImport;
		command.World = world;
		if (!roots.empty()) {
			command.Subject = Track(world, roots.front());
			command.OldParent = Track(world, parents.front());
		}
		command.Import.Encoded.assign(writer.Bytes().begin(), writer.Bytes().end());
		command.Import.Roots.reserve(roots.size());
		command.Import.Themes.reserve(themes.size());
		command.Import.Parents.reserve(parents.size());
		for (const Entity root : roots)
			command.Import.Roots.push_back(Track(world, root));
		for (const Entity theme : themes)
			command.Import.Themes.push_back(Track(world, theme));
		for (const Entity parent : parents)
			command.Import.Parents.push_back(Track(world, parent));
		command.Description = std::move(description);
		Push(std::move(command));
		return true;
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

	CommandLog::PreparedReparent CommandLog::PrepareReparent(
		WorldId world, Entity instance, Entity from, Entity to, std::string description
	) {
		PreparedReparent prepared;
		if (!Recording_ || Replaying || instance == NULL_ENTITY) return prepared;
		if (BatchDepth == 0) throw std::logic_error("prepared reparent requires an active batch");

		Command command;
		command.Kind = CommandKind::Reparent;
		command.World = world;
		command.Subject = Track(world, instance);
		command.OldParent = Track(world, from);
		command.NewParent = Track(world, to);
		command.Description = std::move(description);

		// Preserve geometric growth rather than reallocating for each row in a
		// multi-selection. Every allocation is before the tree mutation.
		ReserveOne(Done);
		if (!Open.InProgress() && BatchWaypoint != 0) {
			ReserveOne(BatchSnapshot);
			prepared.Publication.push_back(command);
		} else if (!Open.InProgress()) {
			ReserveOne(BatchPublications);
			prepared.Publication.push_back(command);
		}
		prepared.Entry.emplace(std::move(command));
		return prepared;
	}

	void CommandLog::CommitReparent(PreparedReparent &&prepared) noexcept {
		static_assert(std::is_nothrow_move_constructible_v<Command>);
		static_assert(std::is_nothrow_move_assignable_v<Command>);
		if (!prepared.Entry) return;
		Push(std::move(*prepared.Entry), prepared.Publication.empty() ? nullptr : &prepared.Publication);
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

	void CommandLog::RecordComponent(
		WorldId world,
		Entity instance,
		engine::core::Name component,
		std::vector<std::byte> before,
		std::vector<std::byte> after,
		std::string description
	) {
		if (instance == NULL_ENTITY || !component.IsValid() || before == after) return;
		Command command;
		command.Kind = CommandKind::UiComponent;
		command.World = world;
		command.Subject = Track(world, instance);
		command.Component = component;
		command.ComponentBefore = std::move(before);
		command.ComponentAfter = std::move(after);
		command.Description = std::move(description);
		Push(std::move(command));
	}

	void CommandLog::RecordProceduralAuthority(
		WorldId world, std::vector<std::byte> before, std::vector<std::byte> after, std::string description
	) {
		using engine::script::ProceduralHostLimits;
		if (!world.IsValid() || before == after ||
			before.size() > ProceduralHostLimits::MaximumAuthorityBytes ||
			after.size() > ProceduralHostLimits::MaximumAuthorityBytes)
			return;

		Command command;
		command.Kind = CommandKind::ProceduralAuthority;
		command.World = world;
		command.AuthorityBefore = std::move(before);
		command.AuthorityAfter = std::move(after);
		command.Description = std::move(description);
		Push(std::move(command));
	}

	bool CommandLog::RenameWorld(WorldId world, engine::core::Name desired) {
		if (Universe == nullptr || !world.IsValid() || !desired.IsValid() || !Recording_ || Replaying ||
			Open.InProgress() || BatchDepth != 0 || !WorldRenameCoordinator || Universe->TickInFlight())
			return false;
		const auto current = Universe->NameOf(world);
		if (!current.IsValid() || current == desired || Universe->Find(desired).IsValid()) return false;
		std::vector<Bound> added;
		auto rollback = [&] {
			for (const auto &bound : added) {
				const auto found = Ids.find(bound);
				if (found != Ids.end()) {
					Entities.erase(found->second);
					Ids.erase(found);
				}
			}
		};
		try {
			Command command;
			command.Kind = CommandKind::RenameWorld;
			command.World = world;
			command.WorldNameBefore = current;
			command.WorldNameAfter = desired;
			command.Description = "Rename world";
			std::vector<Bound> portals;
			for (const WorldId candidate : Universe->Worlds()) {
				if (Universe->IsRemote(candidate)) return false;
				Universe->Enter(candidate, [&](Store &store) {
					store.Each<const engine::scene::Portal>([&](Entity entity,
																const engine::scene::Portal &portal) {
						if (portal.DestinationWorld == current) portals.push_back({candidate, entity});
					});
				});
			}
			added.reserve(portals.size());
			command.WorldRenamePortals.reserve(portals.size());
			Done.reserve(Done.size() + 1);
			for (const auto &portal : portals) {
				const bool isNew = !Ids.contains(portal);
				const auto id = Track(portal.World, portal.Instance);
				if (isNew) added.push_back(portal);
				command.WorldRenamePortals.push_back({portal.World, id});
			}
			if (!ApplyWorldRename(command, true)) {
				rollback();
				return false;
			}
			// Push may notify an observer after storing the command.
			try {
				Push(std::move(command));
			} catch (...) {
				try {
					ENGINE_WARN("world rename observer failed after commit");
				} catch (...) {}
			}
			FinalizeWorldRename(world, current);
			return true;
		} catch (const std::bad_alloc &) {
			rollback();
			return false;
		} catch (const std::length_error &) {
			rollback();
			return false;
		}
	}

	void CommandLog::FinalizeWorldRename(WorldId world, engine::core::Name previous) {
		try {
			if (WorldRenameFinalizer) WorldRenameFinalizer(world, previous);
		} catch (...) {
			try {
				ENGINE_WARN("world rename presentation cleanup failed after commit");
			} catch (...) {}
		}
	}

	bool CommandLog::ApplyWorldRename(const Command &command, bool forward) {
		if (!WorldRenameCoordinator || Universe == nullptr || Universe->TickInFlight()) return false;
		const auto from = forward ? command.WorldNameBefore : command.WorldNameAfter;
		const auto to = forward ? command.WorldNameAfter : command.WorldNameBefore;
		if (Universe->NameOf(command.World) != from || Universe->Find(to).IsValid()) return false;

		using Write = std::function<void(Store &, engine::ecs::Scheduler &)>;
		struct PreparedWrite {
			WorldId World;
			Write Body;
		};
		std::vector<PreparedWrite> writes;
		writes.reserve(command.WorldRenamePortals.size());
		for (const WorldId world : Universe->Worlds()) {
			if (Universe->IsRemote(world)) return false;
			size_t count = 0;
			bool valid = true;
			Universe->Enter(world, [&](Store &store, engine::ecs::Scheduler &) {
				for (const auto &row : command.WorldRenamePortals) {
					if (row.World != world) continue;
					const Entity entity = ResolveInWorld(row.Instance, world);
					const auto *portal = store.Get<engine::scene::Portal>(entity);
					if (portal == nullptr || portal->DestinationWorld != from) {
						valid = false;
						return;
					}
					++count;
					writes.push_back({world, [entity, to](Store &target, engine::ecs::Scheduler &) {
										  target.GetMutable<engine::scene::Portal>(entity)->DestinationWorld =
											  to;
									  }});
				}
				if (count != 0)
					store.ReserveChangedEntities(engine::ecs::Components::Of<engine::scene::Portal>(), count);
			});
			if (!valid) return false;
		}
		if (writes.size() != command.WorldRenamePortals.size()) return false;
		if (!WorldRenameCoordinator(command.World, from, to)) return false;
		for (const auto &write : writes)
			Universe->Enter(write.World, write.Body);
		return true;
	}

	bool CommandLog::Apply(Command &command, bool forward) {
		if (Universe == nullptr || !command.World.IsValid()) {
			return false;
		}
		if (command.Kind == CommandKind::RenameWorld) return ApplyWorldRename(command, forward);

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
					// rebuilds as a root rather than refusing - the subtree is
					// still the author's work and losing it to a missing parent
					// would be the delete-nobody-asked-for this log exists to
					// undo.
					const Entity parent = Resolve(command.OldParent);

					std::string error;
					std::vector<engine::game::InstanceDocumentIdentity> identities;
					const Entity rebuilt = engine::game::ReadInstanceDocument(
						store,
						command.Document,
						store.Alive(parent) ? parent : NULL_ENTITY,
						error,
						&identities
					);

					if (rebuilt == NULL_ENTITY) {
						ENGINE_WARN("undo could not rebuild '{}': {}", command.Description, error);
						return;
					}

					for (const auto &binding : command.DocumentBindings) {
						const auto found =
							std::find_if(identities.begin(), identities.end(), [&](const auto &identity) {
								return identity.LocalId == binding.LocalId;
							});
						if (found != identities.end())
							Rebind(binding.Instance, command.World, found->Instance);
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
				CaptureInstanceDocument(command, store, subject);
				command.OldParent = Track(command.World, store.ParentOf(subject));

				store.DestroyInstance(subject);
				for (const auto &binding : command.DocumentBindings)
					Rebind(binding.Instance, command.World, NULL_ENTITY);
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

			case CommandKind::UiComponent: {
				const Entity subject = Resolve(command.Subject);
				if (subject == NULL_ENTITY || !store.Alive(subject)) return;
				landed = ApplyComponent(
					store,
					subject,
					command.Component,
					forward ? std::span<const std::byte>(command.ComponentAfter)
							: std::span<const std::byte>(command.ComponentBefore)
				);
				return;
			}

			case CommandKind::ProceduralAuthority: {
				const auto &bytes = forward ? command.AuthorityAfter : command.AuthorityBefore;
				if (bytes.empty()) {
					if (!store.HasResource<engine::script::ProceduralAuthority>()) return;
					store.RemoveResource<engine::script::ProceduralAuthority>();
					landed = true;
					return;
				}
				if (bytes.size() > engine::script::ProceduralHostLimits::MaximumAuthorityBytes) return;
				engine::procedural::Diagnostic diagnostic;
				landed = engine::script::DecodeProceduralAuthority(
					store,
					std::span(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()),
					diagnostic
				);
				return;
			}
			case CommandKind::RenameWorld:
				return;

			case CommandKind::UiDocumentImport: {
				const auto resolve = [this, &command](EditId id) {
					return ResolveInWorld(id, command.World);
				};
				if (!forward) {
					bool removed = false;
					for (const EditId id : command.Import.Roots) {
						const Entity root = resolve(id);
						if (root != NULL_ENTITY && store.Alive(root)) {
							store.DestroyInstance(root);
							removed = true;
						}
						Rebind(id, command.World, NULL_ENTITY);
					}
					for (const EditId id : command.Import.Themes) {
						const Entity theme = resolve(id);
						if (theme != NULL_ENTITY && store.Alive(theme)) {
							store.DestroyInstance(theme);
							removed = true;
						}
						Rebind(id, command.World, NULL_ENTITY);
					}
					landed = removed;
					return;
				}

				if ((command.Import.Roots.empty() && command.Import.Themes.empty()) ||
					command.Import.Roots.size() != command.Import.Parents.size() ||
					command.Import.Encoded.empty())
					return;
				std::vector<Entity> parents;
				parents.reserve(command.Import.Parents.size());
				for (const EditId id : command.Import.Parents) {
					const Entity parent = resolve(id);
					if (parent == NULL_ENTITY || !store.Alive(parent)) return;
					parents.push_back(parent);
				}
				engine::core::ByteReader reader(command.Import.Encoded);
				engine::gui::UiDocument document;
				engine::gui::DocumentReport report;
				if (!engine::gui::DecodeDocument(reader, document, report)) return;
				std::vector<Entity> roots;
				std::vector<Entity> themes;
				if (!engine::gui::ImportDocument(store, document, roots, report, {}, &themes)) return;
				if (roots.size() != command.Import.Roots.size() ||
					themes.size() != command.Import.Themes.size()) {
					for (const Entity root : roots)
						if (store.Alive(root)) store.DestroyInstance(root);
					for (const Entity theme : themes)
						if (store.Alive(theme)) store.DestroyInstance(theme);
					return;
				}
				for (size_t index = 0; index < roots.size(); ++index) {
					if (store.SetParent(roots[index], parents[index])) continue;
					for (const Entity root : roots)
						if (store.Alive(root)) store.DestroyInstance(root);
					for (const Entity theme : themes)
						if (store.Alive(theme)) store.DestroyInstance(theme);
					return;
				}
				for (size_t index = 0; index < roots.size(); ++index)
					Rebind(command.Import.Roots[index], command.World, roots[index]);
				for (size_t index = 0; index < themes.size(); ++index)
					Rebind(command.Import.Themes[index], command.World, themes[index]);
				landed = true;
				return;
			}
			}
		});

		return landed;
	}

	bool CommandLog::Undo(const std::function<void()> &afterReplay) {
		if (BatchDepth != 0 || Done.empty()) {
			return false;
		}
		if (Done.back().Kind == CommandKind::RenameWorld) {
			try {
				const std::string named = Done.back().Description;
				Undone.reserve(Undone.size() + 1);
				if (!ApplyWorldRename(Done.back(), false)) return false;
				const auto world = Done.back().World;
				const auto previous = Done.back().WorldNameAfter;
				Undone.push_back(std::move(Done.back()));
				Done.pop_back();
				Cut = Done.size();
				if (Open.InProgress()) Open.DepthAtStart = std::min(Open.DepthAtStart, Done.size());
				FinalizeWorldRename(world, previous);
				if (afterReplay) afterReplay();
				try {
					if (Listener.Undone) Listener.Undone(named);
				} catch (...) {}
				return true;
			} catch (const std::bad_alloc &) {
				return false;
			} catch (const std::length_error &) {
				return false;
			}
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
			// the store reuses indices - so the wrong instance is not a remote
			// possibility but the ordinary case.
			const bool applied = Apply(command, false);
			if (applied) {
				landed++;
			} else {
				ENGINE_WARN("nothing to undo for '{}' - its subject is gone", command.Description);
			}

			// **Kept on the redo stack even when it did not land**, so a group
			// is not silently split in half: a redo of a waypoint whose middle
			// command was unreversible has to face the same partial state
			// rather than a shorter group that looks complete.
			// An import whose whole membership was independently deleted is the
			// exception. Its original bytes would create a new, stale document on
			// redo, so the lost import is dropped from history rather than revived.
			if (command.Kind != CommandKind::UiDocumentImport || applied) {
				Undone.push_back(std::move(command));
			}
		}

		Replaying = wasReplaying;
		// A cut, so a later `SetWaypoint` cannot merge across an undo into
		// commands the stack no longer holds in the order they were made.
		Cut = Done.size();
		if (Open.InProgress()) Open.DepthAtStart = std::min(Open.DepthAtStart, Done.size());

		if (landed == 0) {
			return false;
		}
		if (afterReplay) afterReplay();
		if (Listener.Undone) {
			Listener.Undone(named);
		}
		return true;
	}

	bool CommandLog::Redo(const std::function<void()> &afterReplay) {
		if (BatchDepth != 0 || Undone.empty()) {
			return false;
		}
		if (Undone.back().Kind == CommandKind::RenameWorld) {
			try {
				const std::string named = Undone.back().Description;
				Done.reserve(Done.size() + 1);
				if (!ApplyWorldRename(Undone.back(), true)) return false;
				const auto world = Undone.back().World;
				const auto previous = Undone.back().WorldNameBefore;
				Done.push_back(std::move(Undone.back()));
				Undone.pop_back();
				Cut = Done.size();
				if (Open.InProgress()) ++Open.DepthAtStart;
				FinalizeWorldRename(world, previous);
				if (afterReplay) afterReplay();
				try {
					if (Listener.Redone) Listener.Redone(named);
				} catch (...) {}
				return true;
			} catch (const std::bad_alloc &) {
				return false;
			} catch (const std::length_error &) {
				return false;
			}
		}

		const uint64_t waypoint = Undone.back().Waypoint;

		const bool wasReplaying = Replaying;
		Replaying = true;

		// Forwards, which is the order they were made in - the reverse of the
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
				ENGINE_WARN("nothing to redo for '{}' - its subject is gone", command.Description);
			}

			named = command.Description;
			const bool priorRecording = Open.InProgress() && command.Waypoint != Open.Waypoint;
			Done.push_back(std::move(command));
			if (priorRecording) ++Open.DepthAtStart;
		}

		Replaying = wasReplaying;
		Cut = Done.size();

		if (landed == 0) {
			return false;
		}
		if (afterReplay) afterReplay();
		if (Listener.Redone) {
			Listener.Redone(named);
		}
		return true;
	}

	void CommandLog::Forget(WorldId world) {
		if (BatchDepth != 0 || !world.IsValid()) {
			return;
		}
		const auto affects = [world](const Command &command) {
			return command.World == world ||
				   (command.Kind == CommandKind::RenameWorld &&
					std::any_of(
						command.WorldRenamePortals.begin(),
						command.WorldRenamePortals.end(),
						[world](const auto &portal) { return portal.World == world; }
					));
		};
		size_t cutThrough = 0;
		for (size_t index = 0; index < Done.size(); ++index) {
			if (Done[index].Kind == CommandKind::RenameWorld && affects(Done[index])) cutThrough = index + 1;
		}
		const bool renameInRedo = std::any_of(Undone.begin(), Undone.end(), [&](const Command &command) {
			return command.Kind == CommandKind::RenameWorld && affects(command);
		});
		if (cutThrough != 0) {
			Done.erase(Done.begin(), Done.begin() + static_cast<ptrdiff_t>(cutThrough));
			Cut = Cut > cutThrough ? Cut - cutThrough : 0;
			if (Open.InProgress())
				Open.DepthAtStart = Open.DepthAtStart > cutThrough ? Open.DepthAtStart - cutThrough : 0;
		}
		if (cutThrough != 0 || renameInRedo) Undone.clear();

		// The ids the dropped commands referred to go too. An `EditId` left
		// bound to a handle in a destroyed world is a row that `Track` could
		// hand back for an entity the store has since recycled into something
		// else - the same reasoning as `Rebind`'s reverse-row erase.
		const auto forgetIds = [this](const Command &command) {
			auto forget = [this](EditId id) {
				if (!id) {
					return;
				}
				if (const auto found = Entities.find(id.Value); found != Entities.end()) {
					Ids.erase(found->second);
					Entities.erase(found);
				}
			};
			for (const EditId id : {command.Subject, command.OldParent, command.NewParent}) {
				forget(id);
			}
			for (const EditId id : command.Import.Roots)
				forget(id);
			for (const EditId id : command.Import.Themes)
				forget(id);
			for (const EditId id : command.Import.Parents)
				forget(id);
		};

		const auto drop = [&](std::vector<Command> &stack) {
			const auto first = std::remove_if(stack.begin(), stack.end(), [&](const Command &command) {
				if (!affects(command)) {
					return false;
				}
				forgetIds(command);
				return true;
			});
			stack.erase(first, stack.end());
		};

		const auto survivingBoundary = [&](size_t boundary) {
			boundary = std::min(boundary, Done.size());
			return static_cast<size_t>(std::count_if(
				Done.begin(),
				Done.begin() + static_cast<ptrdiff_t>(boundary),
				[world](const Command &command) { return command.World != world; }
			));
		};
		Cut = survivingBoundary(Cut);
		if (Open.InProgress()) Open.DepthAtStart = survivingBoundary(Open.DepthAtStart);
		drop(Done);
		drop(Undone);
		for (auto at = Entities.begin(); at != Entities.end();) {
			if (at->second.World != world) {
				++at;
				continue;
			}
			Ids.erase(at->second);
			at = Entities.erase(at);
		}
	}

	void CommandLog::Clear() {
		if (BatchDepth != 0) return;
		Done.clear();
		Undone.clear();
		Cut = 0;
		if (Open.InProgress()) Open.DepthAtStart = 0;
		Entities.clear();
		Ids.clear();
		// Open tabs may still hold old identities; do not reuse them after a clear.
	}
}
