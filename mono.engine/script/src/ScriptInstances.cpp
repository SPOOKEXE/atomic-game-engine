#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>
#include <vector>

namespace engine::script {

	namespace {
		// Disabled: presence of a tag, spelled as a boolean.
		//
		// **Structural, exactly as `Anchored` is**, and for the same reason: a
		// disabled script is a row in another archetype, so the run loop never
		// visits one. A flag on the row would have to be loaded and branched on
		// for every script in the world, every tick, to answer a question that
		// changes once in a game's life.
		// The enum a script picks a language from, spelled once.
		const core::Name &LanguageEnum() {
			static const core::Name name("ScriptLanguage");
			return name;
		}

		// One container's path, as a property.
		//
		// **A template over the two, because they are the same property twice.**
		// Two hand-written copies of a getter and a setter is where the Luau one
		// and the JavaScript one eventually disagree about what an empty path
		// means.
		template <typename Container> ecs::PropertyDescriptor ContainerProperty(const char *name) {
			ecs::PropertyDescriptor property;
			property.Name = core::Name(name);
			// **`Name`, not `String`.** `Classes::TypeOf` maps a `core::Name`
			// field to `PropertyType::Name`, and every caller sizes its buffer
			// from the type - a descriptor claiming `String` is handed a
			// `std::string` and refused by the size check, silently, which is a
			// property that reads as absent everywhere: no panel row, and
			// nothing written to a game file.
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Container>()});
			property.Writes = property.Reads;

			// **Not scriptable** - `LuaSourceContainer` carries the argument.
			property.Scriptable = false;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Container *held = store.Get<Container>(instance);
				*static_cast<core::Name *>(out) = held != nullptr ? held->Path : core::Name{};
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Container held;
				if (const Container *current = store.Get<Container>(instance); current != nullptr) {
					held = *current;
				}
				held.Path = *static_cast<const core::Name *>(value);
				store.Set(instance, held);
				return true;
			};
			return property;
		}

		// The program this instance actually runs.
		//
		// **Reads through the selector and writes through the extension**, which
		// is the pair that makes one field usable while there are two
		// containers: an author setting `Source = "thing.js"` means "run this
		// JavaScript", and `SetSourcePath` is the one place that decides which
		// container a path belongs in.
		ecs::PropertyDescriptor SourceProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Source");
			// **`Name`, not `String`.** `Classes::TypeOf` maps a `core::Name`
			// field to `PropertyType::Name`, and every caller sizes its buffer
			// from the type - a descriptor claiming `String` is handed a
			// `std::string` and refused by the size check, silently, which is a
			// property that reads as absent everywhere: no panel row, and
			// nothing written to a game file.
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = &ecs::ComponentSet::Intern({
				ecs::Components::Of<LuaSourceContainer>(),
				ecs::Components::Of<JavaScriptSourceContainer>(),
				ecs::Components::Of<CodeSourceContainerSelector>(),
			});
			property.Writes = property.Reads;
			property.Scriptable = false;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				*static_cast<core::Name *>(out) = ActiveSourceOf(store, instance);
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				SetSourcePath(store, instance, *static_cast<const core::Name *>(value));
				return true;
			};
			return property;
		}

		// Which container runs, as an enum a script may set.
		ecs::PropertyDescriptor LanguageProperty() {
			ecs::EnumTable::Register(LanguageEnum().Text(), "Luau");
			ecs::EnumTable::Register(LanguageEnum().Text(), "JavaScript");

			ecs::PropertyDescriptor property;
			property.Name = core::Name("Language");
			property.Type = ecs::PropertyType::Enum;
			property.Size = sizeof(core::Name);
			property.EnumName = LanguageEnum();
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<CodeSourceContainerSelector>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const auto ordinal = static_cast<size_t>(ActiveLanguageOf(store, instance));
				*static_cast<core::Name *>(out) =
					ecs::EnumTable::MemberAt(core::Name("ScriptLanguage"), ordinal);
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				size_t ordinal = 0;
				if (!ecs::EnumTable::OrdinalOf(
						core::Name("ScriptLanguage"), *static_cast<const core::Name *>(value), ordinal
					)) {
					return false;
				}

				CodeSourceContainerSelector selector;
				if (const auto *current = store.Get<CodeSourceContainerSelector>(instance)) {
					selector = *current;
				}
				selector.Active = static_cast<Language>(ordinal);
				store.Set(instance, selector);
				return true;
			};
			return property;
		}

		ecs::PropertyDescriptor DisabledProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Disabled");
			property.Type = ecs::PropertyType::Bool;
			property.Size = sizeof(bool);
			property.Kind = ecs::PropertyKind::Structural;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Disabled>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				*static_cast<bool *>(out) = store.Has<Disabled>(instance);
				return true;
			};

			// Deferred by the store when this runs inside iteration, which is
			// why the kind is declared rather than inferred: a structural change
			// applied inline would move the row out from under the loop walking
			// it.
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				if (*static_cast<const bool *>(value)) {
					store.Set(instance, Disabled{});
				} else {
					store.Remove<Disabled>(instance);
				}
				return true;
			};
			return property;
		}

		// The class tree, built once for the process.
		//
		// A function-local static, so the tree exists before the first caller
		// reads an id from it and cannot be registered twice.
		ecs::ClassId RegisterScriptClasses() {
			// **Through `scene::PartClass` rather than registering `Instance`
			// again.** A script is an instance, so it derives from the same root
			// every other class does - and `Classes::Register` returning the
			// existing id for a repeated name would have hidden a second root
			// rather than prevented one.
			scene::EnsureClassTree();

			// Through the one function, so a caller that registers the class
			// tree cannot end up with a `SourceCache` the snapshot writer
			// refuses - a resource keyed by an unregistered type is minted
			// under the compiler's spelling and aborts once the table is
			// sealed, which is a crash at the first world rather than at the
			// line that caused it.
			RegisterScriptComponents();

			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));

			// `LuaSourceContainer` is Roblox's own base for everything holding a
			// program, and it is worth keeping: `:IsA("LuaSourceContainer")` is
			// how a script asks "is this thing code" without enumerating the
			// leaf classes, and inheritance here is set inclusion so the query
			// costs nothing extra.
			// **The Luau container is in the class set and the JavaScript one is
			// not.** A world of Luau scripts should pay for one column, not two,
			// and `SetSourcePath` adds the other the moment a `.js` path is put
			// on an instance - the same trade `RigidBody` makes by being absent
			// from `BasePart`.
			const std::array source{ecs::Components::Of<LuaSourceContainer>()};
			const ecs::ClassId container = ecs::Classes::Register("LuaSourceContainer", instance, source);
			ecs::Classes::SetCreatable(container, false);

			// **`Disabled` is not in either class's set**, exactly as
			// `RigidBody` is not in `BasePart`'s: whether a script is disabled
			// is an archetype it moves to, so the run loop never visits a
			// disabled row rather than visiting and skipping it.
			const ecs::ClassId script = ecs::Classes::Register("Script", container, {});
			ecs::Classes::Register("LocalScript", container, {});

			// **A sibling of `Script`, not a kind of one, and that is what makes
			// it inert.** `ScriptsIn` collects `IsA(Script)` and
			// `IsA(LocalScript)`; a `ModuleScript` is neither, so the run loop
			// never visits one and nothing had to learn to skip it. Derived from
			// `Script` it would have run on every server by default, which is
			// the opposite of what a module is for.
			ecs::Classes::Register("ModuleScript", container, {});

			// These endpoints carry no state until their delivery models exist.
			// Registering the classes still preserves imported UI hierarchy and lets
			// a port identify the unsupported event call at its source, rather than
			// replacing the instance with a Folder. A networking or callback method
			// here would be a misleading no-op, so neither is exposed yet.
			ecs::Classes::Register("RemoteEvent", instance, {});
			ecs::Classes::Register("BindableEvent", instance, {});

			// --- the two containers, and the switch between them --------------
			//
			// **`Source` is the active one and is what every tool already asks
			// for**, so the name stays. What changed underneath is that there
			// are two programs and something choosing between them.
			//
			// **None of the three source paths is scriptable.** A script that
			// could write another script's source is the sandbox escape that
			// makes the step budget, the memory ceiling and the host role
			// decorative - `LuaSourceContainer` carries the argument. The
			// properties panel, a game file and the Rojo sync all still write
			// them, because they are the author rather than the program.
			ecs::Classes::Computed(container, SourceProperty());
			ecs::Classes::Computed(container, ContainerProperty<LuaSourceContainer>("LuaSource"));
			ecs::Classes::Computed(
				container, ContainerProperty<JavaScriptSourceContainer>("JavaScriptSource")
			);

			// **And the one part of it a script may set.** Which language an
			// instance runs is a decision a game can legitimately make - a mod
			// swapping an implementation, a test running one behaviour twice -
			// and none of it requires reading a line of anybody's source.
			ecs::Classes::Computed(container, LanguageProperty());

			ecs::Classes::Computed(container, DisabledProperty());
			return script;
		}
	}

	core::Name ActiveSourceOf(const ecs::Store &store, ecs::Entity instance) {
		if (ActiveLanguageOf(store, instance) == Language::JavaScript) {
			const JavaScriptSourceContainer *js = store.Get<JavaScriptSourceContainer>(instance);
			return js != nullptr ? js->Path : core::Name{};
		}

		const LuaSourceContainer *lua = store.Get<LuaSourceContainer>(instance);
		return lua != nullptr ? lua->Path : core::Name{};
	}

	Language ActiveLanguageOf(const ecs::Store &store, ecs::Entity instance) {
		// **No selector means Luau**, which is what every script in this engine
		// was before there were two - so a world loaded from an older file runs
		// exactly as it did.
		const CodeSourceContainerSelector *selector = store.Get<CodeSourceContainerSelector>(instance);
		return selector != nullptr ? selector->Active : Language::Luau;
	}

	void SetSourcePath(ecs::Store &store, ecs::Entity instance, core::Name path) {
		// **The extension decides, and it decides in one place.** `LanguageOf`
		// is what says a `.ts` file is JavaScript; a caller picking the
		// container itself would be a second answer to that, and the two would
		// disagree the first time a extension was added to one of them.
		const Language language = LanguageOf(path.IsValid() ? path.Text() : std::string_view{});

		if (language == Language::JavaScript) {
			JavaScriptSourceContainer held;
			if (const auto *current = store.Get<JavaScriptSourceContainer>(instance)) {
				held = *current;
			}
			held.Path = path;
			store.Set(instance, held);
		} else {
			LuaSourceContainer held;
			if (const auto *current = store.Get<LuaSourceContainer>(instance)) {
				held = *current;
			}
			held.Path = path;
			store.Set(instance, held);
		}

		// **The selector follows the write, so one assignment does the obvious
		// thing.** An author setting `Source` to a `.js` file means "run this",
		// not "fill the JavaScript slot and go on running the Luau one".
		CodeSourceContainerSelector selector;
		if (const auto *current = store.Get<CodeSourceContainerSelector>(instance)) {
			selector = *current;
		}
		selector.Active = language;
		store.Set(instance, selector);
	}

	ecs::ClassId ScriptClass() {
		static const ecs::ClassId script = RegisterScriptClasses();
		return script;
	}

	ecs::ClassId LocalScriptClass() {
		ScriptClass();
		return ecs::Classes::Find(core::Name("LocalScript"));
	}

	std::vector<ecs::Entity> ScriptsIn(ecs::Store &store, bool server, bool client) {
		ScriptClass();

		const ecs::ClassId scriptId = ecs::Classes::Find(core::Name("Script"));
		const ecs::ClassId localId = ecs::Classes::Find(core::Name("LocalScript"));

		std::vector<ecs::Entity> found;
		// **The Luau container is what every script instance has**, whichever
		// language it is set to run: it is in the class set, so this walk finds
		// a JavaScript script too - and `ActiveSourceOf` is what decides what
		// each one actually runs.
		store.Each<const LuaSourceContainer>([&](ecs::Entity entity, const LuaSourceContainer &) {
			// A disabled script is in another archetype, so this query does not
			// visit one - but the check is here anyway, because `Each` matches
			// on the container alone and a caller could add the tag to a row
			// this query already found in the same tick.
			if (store.Has<Disabled>(entity)) {
				return;
			}

			const ecs::ClassId owner = store.ClassOf(entity);
			const bool isServerScript = ecs::Classes::IsA(owner, scriptId);
			const bool isLocalScript = ecs::Classes::IsA(owner, localId);

			// **Roblox's rule.** A `Script` runs where `IsServer()` is true and
			// a `LocalScript` where `IsClient()` is; a single-player host is
			// both and runs both, which is exactly right rather than a special
			// case.
			if ((isServerScript && server) || (isLocalScript && client)) {
				found.push_back(entity);
			}
		});

		// Creation order, so a world loaded the same way twice runs its scripts
		// in the same sequence. One script may build what another expects to
		// find, and an archetype-order walk would reorder itself the first time
		// an unrelated component was added to one of them.
		std::sort(found.begin(), found.end(), [](ecs::Entity left, ecs::Entity right) {
			return left.Id < right.Id;
		});
		return found;
	}

	std::vector<ecs::Entity> ClientScriptsIn(ecs::Store &store) {
		// **The identity first, because without it the answer is "none".** A
		// client is told which player is its own over the user channel - see
		// `game::JoinNotice` - and until that arrives there is no "own subtree"
		// for a script to be in. Running everything in the meantime would run
		// another player's scripts for the few ticks before the notice lands.
		const scene::LocalPlayer *viewer = store.Resource<scene::LocalPlayer>();
		const ecs::Entity self = viewer != nullptr ? viewer->Instance : ecs::NULL_ENTITY;

		std::vector<ecs::Entity> found;
		for (const ecs::Entity instance : ScriptsIn(store, false, true)) {
			// Roblox's two containers. `ReplicatedFirst` is everybody's and runs
			// ahead of the world; a player's own subtree is theirs alone.
			if (scene::InReplicatedFirst(store, instance)) {
				found.push_back(instance);
				continue;
			}

			if (self != ecs::NULL_ENTITY && scene::PlayerOwning(store, instance) == self) {
				found.push_back(instance);
			}
		}
		return found;
	}

	ecs::ClassId ModuleScriptClass() {
		ScriptClass();
		return ecs::Classes::Find(core::Name("ModuleScript"));
	}

	ecs::Entity MakeModule(ecs::Store &store, std::string_view path, std::string_view name) {
		const ecs::Entity instance = store.CreateInstance(ModuleScriptClass(), name);
		if (instance == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		SetSourcePath(store, instance, core::Name(path));
		return instance;
	}

	namespace {
		// The class a directory becomes.
		//
		// **A bare `Instance`, because this engine has no `Folder`.** Roblox's
		// `Folder` is a class whose whole content is "a thing with children",
		// which every instance already is here. Registering one so that a mounted
		// directory could have a familiar `ClassName` would put a class in the
		// tree that nothing else names, and a script asking `IsA("Folder")` is
		// asking a question the engine has no other reason to answer.
		ecs::ClassId ContainerClass() {
			ScriptClass();
			return ecs::Classes::Find(core::Name("Instance"));
		}

		// One directory, sorted so a mount produces the same world twice.
		//
		// `directory_iterator` yields in whatever order the filesystem hands
		// back, which differs between machines and between runs on the same
		// machine. A world whose instance ids depended on that would not replay,
		// and `ScriptsIn` sorts by id for exactly this reason.
		std::vector<std::filesystem::path> SortedEntries(const std::filesystem::path &directory) {
			std::vector<std::filesystem::path> entries;

			std::error_code failure;
			for (const auto &entry : std::filesystem::directory_iterator(directory, failure)) {
				entries.push_back(entry.path());
			}
			if (failure) {
				entries.clear();
			}

			std::sort(entries.begin(), entries.end());
			return entries;
		}

		bool IsLuau(const std::filesystem::path &path) {
			return path.extension() == ".luau" || path.extension() == ".lua";
		}
	}

	ecs::Entity MountModuleTree(
		ecs::Store &store, const std::filesystem::path &directory, std::string_view name, ecs::Entity parent
	) {
		std::error_code failure;
		if (!std::filesystem::is_directory(directory, failure)) {
			return ecs::NULL_ENTITY;
		}

		const std::vector<std::filesystem::path> entries = SortedEntries(directory);

		// **`init.luau` decides what this directory *is*, so it is found before
		// anything is created.** A directory holding one becomes a
		// `ModuleScript` with children; without one it becomes a container. Both
		// end up with the same name and the same children, which is what makes
		// the rule invisible to everything downstream.
		std::filesystem::path init;
		for (const std::filesystem::path &entry : entries) {
			if (IsLuau(entry) && entry.stem() == "init") {
				init = entry;
				break;
			}
		}

		ecs::Entity root;
		if (!init.empty()) {
			root = MakeModule(store, std::filesystem::absolute(init).string(), name);
		} else {
			root = store.CreateInstance(ContainerClass(), name);
		}

		if (root == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		size_t mounted = 0;
		for (const std::filesystem::path &entry : entries) {
			if (std::filesystem::is_directory(entry, failure)) {
				if (MountModuleTree(store, entry, entry.filename().string(), root) != ecs::NULL_ENTITY) {
					mounted++;
				}
				continue;
			}

			if (!IsLuau(entry) || entry == init) {
				continue;
			}

			const ecs::Entity module =
				MakeModule(store, std::filesystem::absolute(entry).string(), entry.stem().string());
			if (module != ecs::NULL_ENTITY && store.SetParent(module, root)) {
				mounted++;
			}
		}

		// A directory with nothing in it to mount leaves no instance behind. An
		// empty container would be indistinguishable from a library whose files
		// failed to stage, and the second is worth noticing.
		if (mounted == 0 && init.empty()) {
			store.Destroy(root);
			return ecs::NULL_ENTITY;
		}

		if (parent != ecs::NULL_ENTITY && !store.SetParent(root, parent)) {
			store.Destroy(root);
			return ecs::NULL_ENTITY;
		}

		return root;
	}

	ecs::Entity MakeScript(ecs::Store &store, std::string_view path, std::string_view name, bool local) {
		const ecs::ClassId id = local ? LocalScriptClass() : ScriptClass();

		const ecs::Entity instance = store.CreateInstance(id, name);
		if (instance == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		SetSourcePath(store, instance, core::Name(path));
		return instance;
	}
}
