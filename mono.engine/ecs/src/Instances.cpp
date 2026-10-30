#include "Instances.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::ecs {

	namespace {
		// How deep a tree this file will walk before deciding it is looking at
		// a cycle rather than at a scene.
		//
		// `SetParent` refuses to make a cycle, so nothing should ever reach
		// this. It exists because the walks that would hang are the ones that
		// build a string or a list as they go, and a hang with a growing
		// allocation behind it is the worst way to find out.
		constexpr size_t MAXIMUM_DEPTH = 4096;

		// Suppresses removal announcements for as long as it is alive.
		struct RemovalGuard {
			StoreState &State;
			bool Previous;

			explicit RemovalGuard(StoreState &state) : State(state), Previous(state.Removing) {
				State.Removing = true;
			}
			~RemovalGuard() {
				State.Removing = Previous;
			}

			RemovalGuard(const RemovalGuard &) = delete;
			RemovalGuard &operator=(const RemovalGuard &) = delete;
		};

		// Tells every ancestor what it is about to lose, while it still has it.
		//
		// **Before a single link is touched, which is what makes this safe to
		// dispatch synchronously.** `script/Changes.hpp` refuses to fire
		// `.Changed` from inside a write because the handler would re-enter the
		// VM with the row half-written. Nothing is half-written here: this runs
		// at the top of the operation, on a tree that is entirely consistent,
		// and it is the only position from which "you are called while it is
		// still there" can be true at all.
		//
		// **Every leaving instance, against every ancestor losing it**, which is
		// Roblox's fan-out. Moving a model out of `Workspace` announces the
		// model *and every part in it* to `Workspace`, because each of them
		// stops being a descendant of it.
		//
		// @param state  The world it is happening in.
		// @param leaving The subtree root that is going.
		// @param from   The parent it is leaving, for a reparent.
		// @param ownChain Whether each subject announces to its *own* ancestors
		//                 rather than to `from`'s. True for a destroy, where the
		//                 ancestors inside the subtree lose their children too;
		//                 false for a reparent, where they move along with it.
		void NotifyRemoving(StoreState &state, Entity leaving, Entity from, bool ownChain) {
			if (!state.BeforeRemoving || state.Removing) {
				return;
			}

			// Collected before anything is announced. A handler may reparent or
			// destroy part of what it was told about, and a walk that read the
			// tree as it went would then be walking something else.
			std::vector<Entity> subjects;
			subjects.push_back(leaving);
			EachDescendant(state, leaving, [&subjects](Entity under) { subjects.push_back(under); });

			const RemovalGuard guard(state);

			for (const Entity subject : subjects) {
				// A handler may have taken this one already, and announcing a
				// row that is gone is the failure this signal exists to avoid.
				if (!IsEntityAlive(state, subject)) {
					continue;
				}

				const Entity start = ownChain ? ParentOf(state, subject) : from;

				size_t steps = 0;
				for (Entity above = start; above != NULL_ENTITY && steps < MAXIMUM_DEPTH;
					 above = ParentOf(state, above), steps++) {
					if (!IsEntityAlive(state, above)) {
						break;
					}
					state.BeforeRemoving(above, subject);
				}
			}
		}

		// Writes down a reparent, when anything is listening for one.
		//
		// @param state    The world it happened in.
		// @param instance What moved.
		// @param from     The parent it left.
		// @param to       The parent it joined.
		void RecordTreeChange(StoreState &state, Entity instance, Entity from, Entity to) {
			if (!state.WatchTree) {
				return;
			}
			state.TreeChanges.push_back(TreeChange{instance, from, to});
		}

		template <typename T> void ReserveAdditional(std::vector<T> &entries, size_t additional) {
			if (additional <= entries.capacity() - entries.size()) {
				return;
			}
			if (additional > entries.max_size() - entries.size()) {
				throw std::length_error("tree change reservation exceeds maximum size");
			}
			const size_t required = entries.size() + additional;
			const size_t growth =
				entries.capacity() > entries.max_size() / 2 ? entries.max_size() : entries.capacity() * 2;
			entries.reserve(std::max(required, growth));
		}

		// No callback runs after this point. Reserve before touching any links so
		// a failed allocation cannot leave the hierarchy half reparented.
		void PrepareReparent(StoreState &state) {
			if (state.WatchTree) {
				ReserveAdditional(state.TreeChanges, 1);
			}
			if (state.Watched.empty()) {
				return;
			}
			const ComponentId hierarchy = Components::Of<Hierarchy>();
			if (hierarchy.Index < state.ComponentChanges.size() &&
				state.ComponentChanges[hierarchy.Index] != 0) {
				// The instance, two siblings, two parents and the destination tail.
				ReserveAdditional(state.ChangedEntities[hierarchy.Index], 6);
			}
		}

		// One node's tree links, or null when the entity is not an instance.
		const Hierarchy *NodeOf(const StoreState &state, Entity instance) {
			return static_cast<const Hierarchy *>(GetComponent(state, instance, Components::Of<Hierarchy>()));
		}

		// The same, for writing. Every call may move the row it returns a
		// pointer into, so a caller holding two of these at once is holding one
		// that has already gone stale.
		Hierarchy *MutableNodeOf(StoreState &state, Entity instance) {
			return static_cast<Hierarchy *>(
				GetComponentMutable(state, instance, Components::Of<Hierarchy>())
			);
		}

		// Rebuilds a parent's child links after a freed row was left named by
		// them, and answers with the tail that survived.
		//
		// **Reached only once a link is already known bad**, so an intact list
		// pays nothing for this. The walk stops at the first dead child rather
		// than stepping over it because the links *out of* a freed row are gone
		// with the row: there is no way to reach what followed it, and guessing
		// would invent an order the author never wrote.
		// One entity handle: the parent, and nothing derived from it.
		constexpr uint32_t WIRE_HIERARCHY_BYTES = 8;

		void WriteHierarchies(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *nodes = static_cast<const Hierarchy *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteUInt64(nodes[index].Parent.Id);
			}
		}

		// **Total over its input, as `WireFormat::Read` requires.** Every bit
		// pattern arrives from a peer, and any of them is a parent handle this
		// store may or may not know - which is the receiver's problem to solve
		// and not a decode failure.
		//
		// The derived links are cleared rather than left alone. A caller that
		// takes this value writes it nowhere near a live tree: `WriteComponents`
		// reads `Parent` out and calls `SetParent`, and leaving stale handles in
		// the other four fields would only be a trap for the next reader.
		void ReadHierarchies(core::ByteReader &reader, void *destination, size_t count) {
			auto *nodes = static_cast<Hierarchy *>(destination);
			for (size_t index = 0; index < count; index++) {
				nodes[index] = Hierarchy{};
				nodes[index].Parent = Entity(reader.ReadUInt64());
			}
		}

		Entity RepairChildren(StoreState &state, Entity parent) {
			const Hierarchy *host = NodeOf(state, parent);
			if (host == nullptr) {
				return NULL_ENTITY;
			}

			Entity tail = NULL_ENTITY;
			for (Entity child = host->FirstChild; child != NULL_ENTITY;) {
				const Hierarchy *link = NodeOf(state, child);
				if (link == nullptr) {
					break;
				}
				tail = child;
				child = link->NextSibling;
			}

			// Whatever the walk ended on is the end of the list now, and its
			// `NextSibling` is still naming the row that was freed.
			if (tail != NULL_ENTITY) {
				MutableNodeOf(state, tail)->NextSibling = NULL_ENTITY;
			}

			Hierarchy *node = MutableNodeOf(state, parent);
			if (tail == NULL_ENTITY) {
				node->FirstChild = NULL_ENTITY;
			}
			node->LastChild = tail;
			return tail;
		}

		// Copies one instance without its subtree.
		Entity CloneOne(StoreState &state, Entity source, Entity copy) {
			const EntityId key = EntityId::Of(source);
			const EntityLocation from = *state.Directory.Locate(key.Index);

			const ComponentSet &set = state.Tables[from.Archetype].Set();

			const EntityId copyKey = EntityId::Of(copy);
			const uint32_t table = TableFor(state, set);
			Relocate(state, copyKey.Index, EntityLocation{}, table);

			// The source row may have moved when the copy was placed, so it is
			// re-read rather than remembered.
			const EntityLocation now = *state.Directory.Locate(key.Index);
			const EntityLocation into = *state.Directory.Locate(copyKey.Index);

			Archetype &destination = state.Tables[into.Archetype];
			Archetype &origin = state.Tables[now.Archetype];

			for (const ComponentId component : set.Ids()) {
				destination.Find(component)->Assign(into.Row, origin.Find(component)->At(now.Row));
			}

			// A copy belongs to no tree until somebody parents it.
			Hierarchy detached;
			destination.Find(Components::Of<Hierarchy>())->Assign(into.Row, &detached);

			return copy;
		}
	}

	void RegisterInstanceComponents() {
		// **The three the table used to name after the compiler, and they are
		// first here because order is the whole of it.** `Components::Of<T>()`
		// mints an id under `TypeNameOf<T>()` when it finds nothing, and
		// `Adopt` aborts on an explicit registration that arrives *after* one -
		// a type has one name. So these have to beat their own first use, and
		// their first use is close: `Store`'s constructor calls this function
		// and then immediately does `SetResource(WorldTime{})`.
		//
		// **Why they need a name at all.** All three reach a `.agame`, and the
		// automatic name is the compiler's spelling of the type. It is stable
		// within one build and nothing wider, so a file written by one compiler
		// and read by another would carry a column nothing matched. That is
		// rule 4 and decision 21: a name that crosses a boundary is a string
		// somebody chose.
		//
		// Registered before `RegisterAttributeComponents` below rather than
		// after, for no reason beyond the one above - nothing there touches
		// these, and "first" is easier to keep true than "before the four
		// things that matter".
		Components::Register<DirtyBits>("ecs.DirtyBits");
		Components::Register<NotArchivable>("ecs.NotArchivable");
		Components::Register<WorldTime>("ecs.WorldTime");

		// **The attribute table, registered here rather than by its own entry
		// point.** It is a resource every world may grow one of, and a resource
		// `Store::SetResource` mints an id for after `Components::Seal` aborts -
		// so it has to be registered during the same single-threaded startup as
		// everything else. Registering it beside the instance components is what
		// makes "any world that has instances can have attributes" true without a
		// caller having to know.
		RegisterAttributeComponents();

		// **`Hierarchy` is handles and nothing else, so the generated byte
		// serialisation is the right one**: an `Entity` is a directory index and
		// a snapshot restores the directory exactly, so a handle inside a
		// component still means the same entity on the far side.
		// **Only the parent crosses a wire, and the other four handles must
		// not.** `Hierarchy` is not a value, it is a structure: `Parent` is what
		// an author decided, and `FirstChild`, `LastChild`, `NextSibling` and
		// `PreviousSibling` are an index the store that owns the tree
		// maintains. Sending them writes one store's index into another store's
		// tree, which was measured doing exactly the damage that invites: a
		// replica whose sibling list came back round to itself, and an
		// `EachChild` collecting it into a sixteen-gigabyte vector until the
		// allocator refused.
		//
		// The far side rebuilds its own links from `Parent` through
		// `SetParent`, so what it needs is the parent and nothing else. Four
		// handles a row is also thirty-two bytes a row not sent.
		//
		// **`Save` and `Load` are untouched by this.** `Column::Write` uses
		// `TypeDescriptor::Write`, never `Wire` - see `WireFormat`'s own header
		// on why a lossy form must not reach a snapshot - so a `.agame` still
		// carries the whole node.
		Components::Register<Hierarchy>(
			"ecs.Hierarchy", WireFormat{WriteHierarchies, ReadHierarchies, WIRE_HIERARCHY_BYTES}
		);

		// **`InstanceClass` reads like the same case and is the opposite one.**
		// A `ClassId` is a *registration* index, and nothing restores the class
		// table the way the directory is restored - `Classes::Register` runs
		// wherever the code that needs a tree runs, and `RegisterGuiClasses` is
		// called lazily on first use rather than at start-up, so two processes
		// of the same build can number the same class differently depending on
		// which of them opened an interface. It went out as a raw index until
		// v0.15, which is `AGENTS.md` rule 4 exactly: a number derived from
		// declaration order does not survive a save file or a wire, and a
		// receiver resolving it would get whatever class took that slot.
		//
		// So the *name* crosses, as it does for every other identity in this
		// engine, and the id stays inside the process that minted it. An
		// unregistered class reads back as an invalid `ClassId` rather than as a
		// substitute: the far end holding a class this build has never heard of
		// is two builds disagreeing, and `ClassOf` answering "I do not know" is
		// the honest report of that. Every other component on the entity still
		// arrives, so the world is kept and only the class is refused - the same
		// answer the render half gives a pipeline it cannot build.
		Components::Register<InstanceClass>(
			"ecs.InstanceClass",
			[](core::ByteWriter &writer, const void *values, size_t count) {
				const auto *declared = static_cast<const InstanceClass *>(values);
				for (size_t index = 0; index < count; index++) {
					// An invalid id describes as an empty record, whose name is
					// an invalid `Name`, which writes an empty string and reads
					// back invalid. The round trip closes with no special case.
					writer.WriteName(Classes::Describe(declared[index].Class).Name);
				}
			},
			[](core::ByteReader &reader, void *values, size_t count) {
				auto *declared = static_cast<InstanceClass *>(values);
				for (size_t index = 0; index < count; index++) {
					const core::Name named = reader.ReadName();
					declared[index].Class = named.IsValid() ? Classes::Find(named) : ClassId{};

					if (named.IsValid() && !declared[index].Class.IsValid()) {
						ENGINE_WARN(
							"ecs: '{}' is not a class registered here, so the instance arrives "
							"untyped.",
							named.Text()
						);
					}
				}
			}
		);

		// **`InstanceName` is the one that cannot use it.** It holds a
		// `core::Name`, whose id is first-seen order *within one process* - so
		// the generated serialiser was writing an interning counter into save
		// files, and a world reloaded by a process that had interned its
		// strings in a different order came back with every instance named
		// something else. `Components.hpp` says in as many words that this is
		// the form to use for anything holding a `Name`; this type is the one
		// that most obviously does and was the one that did not.
		Components::Register<InstanceName>(
			"ecs.InstanceName",
			[](core::ByteWriter &writer, const void *values, size_t count) {
				const auto *labels = static_cast<const InstanceName *>(values);
				for (size_t index = 0; index < count; index++) {
					writer.WriteName(labels[index].Value);
				}
			},
			[](core::ByteReader &reader, void *values, size_t count) {
				auto *labels = static_cast<InstanceName *>(values);
				for (size_t index = 0; index < count; index++) {
					labels[index].Value = reader.ReadName();
				}
			}
		);
		Components::Register<ClientLocal>("ecs.ClientLocal");
		Components::Register<InstanceProjection>(
			"ecs.InstanceProjection",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *values = static_cast<InstanceProjection *>(destination);
				for (size_t index = 0; index < count; ++index)
					values[index] = {};
			}
		);
	}

	bool IsClientLocalInstance(const Store &store, Entity instance) {
		for (size_t depth = 0; instance != NULL_ENTITY && depth < MAXIMUM_DEPTH; ++depth) {
			if (store.Has<ClientLocal>(instance) || Classes::Describe(store.ClassOf(instance)).RuntimeLocal) {
				return true;
			}
			instance = store.ParentOf(instance);
		}
		return false;
	}

	bool HasClientLocalInstances(Store &store) {
		if (store.CountMatching<ClientLocal>() != 0) return true;

		bool found = false;
		ClassId previous;
		store.EachBatch<const InstanceClass>([&](size_t rows, const InstanceClass *classes) {
			if (found) return;
			for (size_t row = 0; row < rows; ++row) {
				const ClassId current = classes[row].Class;
				if (current == previous) continue;
				previous = current;
				if (Classes::Describe(current).RuntimeLocal) {
					found = true;
					return;
				}
			}
		});
		return found;
	}

	ClassId Classes::RegisterInstanceRoot() {
		// The components have to exist before a property can name them, and
		// this is reachable before any store has been built - a test that
		// registers a class tree and never creates a world is the ordinary case
		// in `gui/tests`.
		RegisterInstanceComponents();

		const ClassId instance = Classes::Register("Instance", {});
		Classes::SetCreatable(instance, false);

		// The concrete zero-component container. Keeping it beside the root makes
		// it available to every class tree, including gui-only worlds, while the
		// virtual `Instance` remains a relationship rather than an insertable row.
		Classes::Register("Folder", instance, {});

		// **A real property over the hierarchy, not a courtesy.**
		// `Instance.hpp` states the model: the tree is organisational, exactly
		// as Roblox's is, and parenting moves nothing and re-resolves nothing.
		//
		// What that costs is written down where it was found - a `scene` part
		// draws before it has a parent, which is a genuine divergence from
		// Roblox. `gui` does not inherit that divergence, because a 2D element
		// is drawn exactly when it descends from an enabled `LayerCollector`,
		// which is a fact about ancestry and is derived per frame.
		PropertyDescriptor parent;
		parent.Name = core::Name("Parent");
		parent.PropertiesTag = core::Name("Identity");
		parent.Type = PropertyType::Reference;
		parent.Size = sizeof(Entity);
		parent.Kind = PropertyKind::Computed;
		parent.Reads = &ComponentSet::Intern({Components::Of<Hierarchy>()});
		parent.Writes = parent.Reads;

		parent.Get = [](const Store &store, Entity subject, void *out) -> bool {
			*static_cast<Entity *>(out) = store.ParentOf(subject);
			return true;
		};

		parent.Set = [](Store &store, Entity subject, const void *value) -> bool {
			// **Authored, and this is the fourth of the four doors.** `.Parent`
			// is one lambda serving both VMs and the properties panel alike, so
			// enforcing the rule at the other three and not here would ship
			// something that holds in Luau and not in the panel - which reads as
			// a bug in the panel rather than as a rule.
			return store.SetParentAuthored(subject, *static_cast<const Entity *>(value));
		};

		Classes::Computed(instance, parent);

		// Everything has a name, and a Roblox script sets `.Name`, so it is
		// writable rather than readable. Declared rather than special-cased in
		// a binding, which is the drift this stopped: it was special-cased in
		// the Luau binding and absent from the JavaScript one.
		Classes::Property<&InstanceName::Value>(instance, "Name");
		(void)Classes::SetPropertiesTag(instance, "Name", core::Name("Identity"));

		return instance;
	}

	Entity CreateInstance(StoreState &state, ClassId id, std::string_view name, EntityRange range) {
		const ClassInfo &info = Classes::Describe(id);
		if (info.Set == nullptr) {
			return NULL_ENTITY;
		}

		const Entity entity = CreateEntity(state, range);
		if (entity == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		const EntityId key = EntityId::Of(entity);

		// Straight into the class's archetype rather than one component at a
		// time. Adding them one by one would walk the entity through every
		// intermediate table on the way, creating each of them.
		const uint32_t table = TableFor(state, *info.Set);
		Relocate(state, key.Index, EntityLocation{}, table);

		const EntityLocation location = *state.Directory.Locate(key.Index);
		Archetype &archetype = state.Tables[location.Archetype];

		// The prototype row, copied column by column. This is what makes a
		// default a value rather than a constructor, and it is the same
		// operation Clone performs from a different source.
		for (const ComponentId component : info.Set->Ids()) {
			if (const void *value = Classes::DefaultOf(id, component); value != nullptr) {
				archetype.Find(component)->Assign(location.Row, value);
			}
		}

		const InstanceClass declared{id};
		archetype.Find(Components::Of<InstanceClass>())->Assign(location.Row, &declared);

		if (!name.empty()) {
			const InstanceName label{core::Name(name)};
			archetype.Find(Components::Of<InstanceName>())->Assign(location.Row, &label);
		}

		return entity;
	}

	ClassId ClassOf(const StoreState &state, Entity instance) {
		const auto *declared = static_cast<const InstanceClass *>(
			GetComponent(state, instance, Components::Of<InstanceClass>())
		);
		return declared == nullptr ? ClassId{} : declared->Class;
	}

	bool IsA(const StoreState &state, Entity instance, ClassId id) {
		return Classes::IsA(ClassOf(state, instance), id);
	}

	core::Name InstanceNameOf(const StoreState &state, Entity instance) {
		const auto *label =
			static_cast<const InstanceName *>(GetComponent(state, instance, Components::Of<InstanceName>()));
		return label == nullptr ? core::Name{} : label->Value;
	}

	Entity ParentOf(const StoreState &state, Entity instance) {
		const Hierarchy *node = NodeOf(state, instance);
		return node == nullptr ? NULL_ENTITY : node->Parent;
	}

	bool HasChildren(const StoreState &state, Entity instance) {
		const Hierarchy *node = NodeOf(state, instance);
		return node != nullptr && node->FirstChild != NULL_ENTITY;
	}

	void EachChild(const StoreState &state, Entity instance, const std::function<void(Entity)> &body) {
		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return;
		}

		// **The walk is bounded, and the bound is not paranoia.** A sibling list
		// is a linked list of entity handles, and a `NextSibling` that points
		// back into the list is a walk that never ends. `DestroyInstance` and
		// `DetachFromTree` both collect what this hands them into a vector
		// first, so the symptom is not a hang: it is a vector that doubles until
		// the allocator refuses, which was measured at a single sixteen-gigabyte
		// request and `std::bad_alloc` out of a studio Play session.
		//
		// A list cannot be longer than the entities that could be in it, so
		// anything past that is a cycle rather than a long list. The ceiling is
		// recomputed per call because the directory grows.
		const size_t ceiling = state.Directory.Capacity() + state.Directory.PredictedCapacity() + 1;
		size_t stepped = 0;

		Entity child = node->FirstChild;
		while (child != NULL_ENTITY) {
			if (++stepped > ceiling) {
				// Loud, because the tree is corrupt and every later walk of it
				// is wrong in a way that will not name this moment. The two
				// handles are what a reader needs: the parent whose list loops,
				// and the child it came back round to.
				ENGINE_ERROR(
					"ecs: the children of {} form a cycle - the walk returned to {} after {} steps. "
					"The list is truncated here so the caller does not collect it forever.",
					instance.Id,
					child.Id,
					stepped - 1
				);
				return;
			}

			// Read before the body runs, so a body that reparents or destroys
			// the child it was handed does not lose its place in the list.
			const Hierarchy *link = NodeOf(state, child);

			// **A link that resolves to nothing ends the walk without being
			// handed over.** It used to call the body first and stop
			// afterwards, so the one thing a caller could not survive - a
			// handle that is not a child, and may not be an entity - was the
			// one thing it was given. `GetChildren()` published it straight to
			// a script, and `RepairChildren` two functions up already breaks
			// before using the same value.
			if (link == nullptr) {
				return;
			}

			const Entity next = link->NextSibling;

			body(child);
			child = next;
		}
	}

	void EachDescendant(const StoreState &state, Entity instance, const std::function<void(Entity)> &body) {
		// **An explicit stack, because a scene's depth is the author's.** The
		// walk this replaced lived in `script` and recursed; a file with ten
		// thousand nested folders in it would have been a stack overflow on
		// open, from data, with nothing to point at.
		//
		// Children are pushed in reverse so they pop in insertion order, which
		// is what makes this the order a hand-written recursive walk produces.
		std::vector<Entity> stack;
		std::vector<Entity> children;

		const auto descend = [&](Entity parent) {
			children.clear();
			EachChild(state, parent, [&children](Entity child) { children.push_back(child); });
			for (size_t index = children.size(); index > 0; index--) {
				stack.push_back(children[index - 1]);
			}
		};

		descend(instance);

		while (!stack.empty()) {
			const Entity at = stack.back();
			stack.pop_back();

			body(at);
			descend(at);
		}
	}

	bool SetInstanceName(StoreState &state, Entity instance, std::string_view name) {
		if (GetComponent(state, instance, Components::Of<InstanceName>()) == nullptr) {
			return false;
		}

		const InstanceName label{name.empty() ? core::Name{} : core::Name(name)};
		SetComponent(state, instance, Components::Of<InstanceName>(), &label);
		return true;
	}

	Entity FindFirstChild(const StoreState &state, Entity instance, std::string_view name, bool recursive) {
		// **An empty query finds nothing, and it used to find the unnamed.**
		// `CreateInstance("")` leaves an instance with no `core::Name` at all,
		// `InstanceNameOf` answers with an invalid one, and an invalid name is
		// what `Name("")` compares equal to - so `FindFirstChild(x, "")` was
		// asking for "whatever has no name" and answering with the first one.
		//
		// Self-consistent, and not what anybody calling it means. Roblox
		// returns nil, and a caller passing a name it read from somewhere and
		// got an empty string for wants nil rather than an arbitrary child.
		if (name.empty()) {
			return NULL_ENTITY;
		}

		const core::Name wanted(name);

		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return NULL_ENTITY;
		}

		for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
			if (InstanceNameOf(state, child) == wanted) {
				return child;
			}
			const Hierarchy *link = NodeOf(state, child);
			child = link == nullptr ? NULL_ENTITY : link->NextSibling;
		}

		if (!recursive) {
			return NULL_ENTITY;
		}

		// **The children first, then the descendants**, rather than one
		// depth-first pass over both. Roblox's recursive form is documented as
		// finding the nearest match, and a script asking for a name it expects
		// one level down should not get one six levels down inside the first
		// child instead. The loop above has already answered the common case,
		// so the cost of the second pass is only paid by a search that failed.
		Entity found = NULL_ENTITY;
		EachDescendant(state, instance, [&](Entity descendant) {
			if (found == NULL_ENTITY && InstanceNameOf(state, descendant) == wanted) {
				found = descendant;
			}
		});
		return found;
	}

	Entity FindFirstChildOfClass(const StoreState &state, Entity instance, ClassId id) {
		if (!id.IsValid()) {
			return NULL_ENTITY;
		}

		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return NULL_ENTITY;
		}

		for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
			// Exactly this class. `IsA` would make asking for a `BasePart` find
			// a `Part`, which is what `FindFirstChildWhichIsA` is for.
			if (ClassOf(state, child) == id) {
				return child;
			}
			const Hierarchy *link = NodeOf(state, child);
			child = link == nullptr ? NULL_ENTITY : link->NextSibling;
		}
		return NULL_ENTITY;
	}

	Entity FindFirstChildWhichIsA(const StoreState &state, Entity instance, ClassId id, bool recursive) {
		if (!id.IsValid()) {
			return NULL_ENTITY;
		}

		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return NULL_ENTITY;
		}

		for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
			if (IsA(state, child, id)) {
				return child;
			}
			const Hierarchy *link = NodeOf(state, child);
			child = link == nullptr ? NULL_ENTITY : link->NextSibling;
		}

		if (!recursive) {
			return NULL_ENTITY;
		}

		Entity found = NULL_ENTITY;
		EachDescendant(state, instance, [&](Entity descendant) {
			if (found == NULL_ENTITY && IsA(state, descendant, id)) {
				found = descendant;
			}
		});
		return found;
	}

	Entity FindFirstAncestor(const StoreState &state, Entity instance, std::string_view name) {
		if (name.empty()) {
			return NULL_ENTITY;
		}

		const core::Name wanted(name);
		for (Entity walk = ParentOf(state, instance); walk != NULL_ENTITY; walk = ParentOf(state, walk)) {
			if (InstanceNameOf(state, walk) == wanted) {
				return walk;
			}
		}
		return NULL_ENTITY;
	}

	Entity FindFirstAncestorOfClass(const StoreState &state, Entity instance, ClassId id) {
		if (!id.IsValid()) {
			return NULL_ENTITY;
		}

		for (Entity walk = ParentOf(state, instance); walk != NULL_ENTITY; walk = ParentOf(state, walk)) {
			if (ClassOf(state, walk) == id) {
				return walk;
			}
		}
		return NULL_ENTITY;
	}

	Entity FindFirstAncestorWhichIsA(const StoreState &state, Entity instance, ClassId id) {
		if (!id.IsValid()) {
			return NULL_ENTITY;
		}

		for (Entity walk = ParentOf(state, instance); walk != NULL_ENTITY; walk = ParentOf(state, walk)) {
			if (IsA(state, walk, id)) {
				return walk;
			}
		}
		return NULL_ENTITY;
	}

	std::string GetFullName(const StoreState &state, Entity instance) {
		if (NodeOf(state, instance) == nullptr) {
			return {};
		}

		// Collected upwards and written downwards, because a path reads from
		// the root and the tree only runs the other way.
		std::vector<core::Name> path;
		for (Entity walk = instance; walk != NULL_ENTITY; walk = ParentOf(state, walk)) {
			path.push_back(InstanceNameOf(state, walk));

			// Bounded by nothing else, so a tree that somehow held a cycle
			// would build a string until the process ran out of memory.
			// `SetParent` refuses to make one; this is the second lock.
			if (path.size() > MAXIMUM_DEPTH) {
				return {};
			}
		}

		std::string full;
		for (size_t index = path.size(); index > 0; index--) {
			if (!full.empty()) {
				full.push_back('.');
			}
			const core::Name step = path[index - 1];
			full.append(step.IsValid() ? step.Text() : std::string_view{});
		}
		return full;
	}

	bool IsDescendantOf(const StoreState &state, Entity instance, Entity ancestor) {
		for (Entity walk = instance; walk != NULL_ENTITY; walk = ParentOf(state, walk)) {
			if (walk == ancestor) {
				return true;
			}
		}
		return false;
	}

	bool SetParent(StoreState &state, Entity instance, Entity parent) {
		return SetParentPrepared(state, instance, parent, nullptr, nullptr, false) != ReparentResult::Refused;
	}

	ReparentResult SetParentPrepared(
		StoreState &state,
		Entity instance,
		Entity parent,
		void (*prepare)(void *, Entity, Entity),
		void *context,
		bool authored
	) {
		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return ReparentResult::Refused;
		}
		if (parent != NULL_ENTITY && NodeOf(state, parent) == nullptr) {
			return ReparentResult::Refused;
		}

		// A cycle is not a wrong answer, it is a hang in every walk of the tree
		// - including the one that would destroy it.
		if (parent != NULL_ENTITY && IsDescendantOf(state, parent, instance)) {
			return ReparentResult::Refused;
		}

		// **Re-parenting to the parent it already has does nothing**, and it has
		// to do nothing rather than do the same thing twice. Everything below
		// unlinks and then appends at the end, so a no-op assignment moved the
		// instance to the back of its own sibling list - and the sibling list
		// is the order `GetChildren()` returns, which replication and replay
		// both depend on agreeing across machines.
		//
		// The shape that finds it is ordinary Roblox code: a script that writes
		// `thing.Parent = holder` on a value that may or may not have changed,
		// or an editor that applies a drag by assigning the parent the row was
		// already under. Roblox's own assignment is a no-op here, so a game
		// written against it silently reorders on this engine and nothing says
		// why.
		if (node->Parent == parent) {
			return ReparentResult::Unchanged;
		}

		// **Announced here, before the first link moves.** See `NotifyRemoving`.
		if (node->Parent != NULL_ENTITY) {
			NotifyRemoving(state, instance, node->Parent, false);

			// **Everything read above is re-read, because a handler ran.** It
			// may have destroyed either end, moved the instance somewhere else,
			// or parented the intended destination underneath it - so each of
			// the three checks at the top of this function has to hold again
			// rather than be assumed to.
			node = NodeOf(state, instance);
			if (node == nullptr) {
				return ReparentResult::Refused;
			}
			if (parent != NULL_ENTITY && NodeOf(state, parent) == nullptr) {
				return ReparentResult::Refused;
			}
			if (parent != NULL_ENTITY && IsDescendantOf(state, parent, instance)) {
				return ReparentResult::Refused;
			}
			if (authored && state.Protected.contains(instance.Id)) {
				return ReparentResult::Refused;
			}
			if (node->Parent == parent) {
				return ReparentResult::Unchanged;
			}
		}
		// The callback may have moved the instance; the recorded source is the
		// parent actually unlinked below, not the one initially notified.
		const Entity leaving = node->Parent;
		PrepareReparent(state);
		if (prepare != nullptr) {
			prepare(context, leaving, parent);
			// Reentry must not leave a stale row pointer or consume queue capacity.
			node = NodeOf(state, instance);
			if (node == nullptr || node->Parent != leaving ||
				(authored && state.Protected.contains(instance.Id)) ||
				(parent != NULL_ENTITY &&
				 (NodeOf(state, parent) == nullptr || IsDescendantOf(state, parent, instance)))) {
				return ReparentResult::Refused;
			}
			PrepareReparent(state);
		}
		// An unobserved store has no dirty marks to publish.
		Hierarchy *moving =
			state.Watched.empty() ? const_cast<Hierarchy *>(node) : MutableNodeOf(state, instance);

		// --- unlink from the old parent ---
		if (moving->Parent != NULL_ENTITY) {
			Hierarchy *previous = moving->PreviousSibling == NULL_ENTITY
									  ? nullptr
									  : MutableNodeOf(state, moving->PreviousSibling);
			Hierarchy *next =
				moving->NextSibling == NULL_ENTITY ? nullptr : MutableNodeOf(state, moving->NextSibling);

			if (previous != nullptr) {
				previous->NextSibling = moving->NextSibling;
			}
			if (next != nullptr) {
				next->PreviousSibling = moving->PreviousSibling;
			}

			if (Hierarchy *old = MutableNodeOf(state, moving->Parent); old != nullptr) {
				if (old->FirstChild == instance) {
					old->FirstChild = moving->NextSibling;
				}
				if (old->LastChild == instance) {
					old->LastChild = moving->PreviousSibling;
				}
			}

			// Re-read: every lookup above may have moved this row.
			moving = MutableNodeOf(state, instance);
		}

		moving->Parent = NULL_ENTITY;
		moving->NextSibling = NULL_ENTITY;
		moving->PreviousSibling = NULL_ENTITY;

		if (parent == NULL_ENTITY) {
			RecordTreeChange(state, instance, leaving, NULL_ENTITY);
			return ReparentResult::Changed;
		}

		// --- link at the end of the new parent, so order is insertion order ---
		Hierarchy *host = MutableNodeOf(state, parent);

		// **A tail naming a freed row is repaired before it is written
		// through, not trusted.** `Store::Destroy` releases an entity without
		// touching the links that point *at* it - that unlink is what
		// `DestroyInstance` adds - so a parent outlives a destroyed child while
		// still naming it as `LastChild`. Every other lookup in this function
		// already tolerates a null; this one dereferenced it, and took the
		// process with it the next time anything parented into that same
		// parent.
		Entity last = host->LastChild;
		if (last != NULL_ENTITY && NodeOf(state, last) == nullptr) {
			last = RepairChildren(state, parent);
			// Re-read: the repair is a series of lookups, and any of them may
			// have moved this row.
			host = MutableNodeOf(state, parent);
		}

		if (last == NULL_ENTITY) {
			host->FirstChild = instance;
			host->LastChild = instance;
		} else {
			MutableNodeOf(state, last)->NextSibling = instance;
			MutableNodeOf(state, parent)->LastChild = instance;
		}

		moving = MutableNodeOf(state, instance);
		moving->Parent = parent;
		moving->PreviousSibling = last;

		RecordTreeChange(state, instance, leaving, parent);
		return ReparentResult::Changed;
	}

	void DetachFromTree(StoreState &state, Entity instance) {
		// **`Assigned`, never `Of`.** This runs on every `Store::Destroy`,
		// including in a process that has not registered a single class yet -
		// and `Components::Of<Hierarchy>()` would *register* the type there,
		// under the compiler-spelled name, which the explicit `ecs.Hierarchy`
		// registration then aborts on. A type nothing has registered is a type
		// no row can be carrying, so an invalid id is the whole answer.
		const ComponentId hierarchy = Components::Assigned<Hierarchy>();
		if (!hierarchy.IsValid()) {
			return;
		}

		if (GetComponent(state, instance, hierarchy) == nullptr) {
			// Not an instance, so nothing in the tree names it. One component
			// lookup is the whole cost this adds to destroying a plain entity.
			return;
		}

		// **The children first, and re-rooted rather than destroyed.** Leaving
		// them pointing at a freed parent would not crash - `ParentOf` of a
		// dead handle is null - but it would make them invisible: `EachRoot`
		// asks for `Parent == NULL_ENTITY`, so a child whose parent is merely
		// *dead* is in the world, in the save file, and reachable from nothing.
		//
		// Collected before anything moves, because unlinking one rewrites the
		// sibling links the walk is standing on.
		std::vector<Entity> children;
		EachChild(state, instance, [&children](Entity child) { children.push_back(child); });

		for (const Entity child : children) {
			SetParent(state, child, NULL_ENTITY);
		}

		SetParent(state, instance, NULL_ENTITY);
	}

	static void DestroyAnnouncedInstance(StoreState &state, Entity instance) {
		// **Silent from here down.** The recursion below destroys each child
		// and unparents every row on the way out, and each of those is a
		// removal that has already been announced by the pass above. Without
		// this a subtree of a thousand rows would announce itself a thousand
		// times over.
		const RemovalGuard guard(state);

		// Children collected before anything is destroyed, because destroying
		// one rewrites the sibling links the walk is standing on.
		std::vector<Entity> children;
		EachChild(state, instance, [&children](Entity child) { children.push_back(child); });

		for (const Entity child : children) {
			DestroyInstance(state, child);
		}

		SetParent(state, instance, NULL_ENTITY);
		DestroyEntity(state, instance);
	}

	void DestroyInstance(StoreState &state, Entity instance) {
		if (!IsEntityAlive(state, instance)) return;

		// The subtree and its own ancestors are announced once before destruction.
		NotifyRemoving(state, instance, ParentOf(state, instance), true);
		if (!IsEntityAlive(state, instance)) return;
		DestroyAnnouncedInstance(state, instance);
	}

	bool DestroyInstancePrepared(
		StoreState &state, Entity instance, bool (*prepare)(void *), void *context, bool followReparents
	) {
		if (!IsEntityAlive(state, instance) || NodeOf(state, instance) == nullptr || state.DeferDepth > 0 ||
			state.Protected.contains(instance.Id))
			return false;
		Entity notifiedParent = ParentOf(state, instance);
		// A listener can move the root while it is being announced. Announce the
		// new ancestry as well before deleting it there. Refuse a callback cycle
		// that never settles instead of looping forever.
		constexpr size_t MAX_PARENT_CHANGES = 64;
		for (size_t attempts = 0;; ++attempts) {
			NotifyRemoving(state, instance, notifiedParent, true);
			if (!IsEntityAlive(state, instance) || NodeOf(state, instance) == nullptr ||
				state.DeferDepth != 0 || state.Protected.contains(instance.Id))
				return false;
			const Entity currentParent = ParentOf(state, instance);
			if (currentParent == notifiedParent) break;
			if (!followReparents) return false;
			if (attempts + 1 == MAX_PARENT_CHANGES) return false;
			notifiedParent = currentParent;
		}
		const auto unchanged = [&] {
			return state.DeferDepth == 0 && IsEntityAlive(state, instance) &&
				   NodeOf(state, instance) != nullptr && !state.Protected.contains(instance.Id) &&
				   ParentOf(state, instance) == notifiedParent;
		};
		if (!unchanged() || (prepare != nullptr && !prepare(context)) || !unchanged()) return false;
		DestroyAnnouncedInstance(state, instance);
		return !IsEntityAlive(state, instance);
	}

	namespace {
		// Reserve bare entity slots before creating rows. A failed reservation can
		// be released immediately even while a query defers structural changes.
		bool
		ReserveClone(StoreState &state, Entity source, std::vector<Entity> &reserved, EntityRange range) {
			if (!IsEntityAlive(state, source) || NodeOf(state, source) == nullptr ||
				GetComponent(state, source, Components::Of<NotArchivable>()) != nullptr) {
				return true;
			}
			const Entity slot = CreateEntity(state, range);
			if (slot == NULL_ENTITY) {
				return false;
			}
			reserved.push_back(slot);
			bool success = true;
			EachChild(state, source, [&](Entity child) {
				if (success) {
					success = ReserveClone(state, child, reserved, range);
				}
			});
			return success;
		}

		// Copies a subtree, recording what became what.
		//
		// @param state  The world holding the source.
		// @param source The instance to copy.
		// @param made   Appended to as `{source, copy}` for every row copied.
		// @return The copy, or NULL_ENTITY when the source is not archivable.
		Entity CloneSubtree(
			StoreState &state,
			Entity source,
			std::vector<ClonedPair> &made,
			std::span<const Entity> reserved,
			size_t &next
		) {
			if (!IsEntityAlive(state, source) || NodeOf(state, source) == nullptr) {
				return NULL_ENTITY;
			}

			// **Not archivable, not copied**, which is Roblox's rule and the
			// whole point of the flag: a marker an author puts on something
			// that a duplicate would break - a script's state holder, a debug
			// visualiser, anything that is one-of.
			if (GetComponent(state, source, Components::Of<NotArchivable>()) != nullptr) {
				return NULL_ENTITY;
			}

			const Entity copy = CloneOne(state, source, reserved[next++]);

			made.push_back(ClonedPair{source, copy});

			std::vector<Entity> children;
			EachChild(state, source, [&children](Entity child) { children.push_back(child); });

			for (const Entity child : children) {
				const Entity copied = CloneSubtree(state, child, made, reserved, next);
				if (copied != NULL_ENTITY) {
					SetParent(state, copied, copy);
				}
			}

			return copy;
		}
	}

	Entity CloneInstance(StoreState &state, Entity source, std::vector<ClonedPair> &made, EntityRange range) {
		std::vector<Entity> reserved;
		if (!ReserveClone(state, source, reserved, range)) {
			for (auto at = reserved.rbegin(); at != reserved.rend(); ++at) {
				state.Directory.Free(EntityId::Of(*at).Index);
			}
			return NULL_ENTITY;
		}
		size_t next = 0;
		return CloneSubtree(state, source, made, reserved, next);
	}
}
