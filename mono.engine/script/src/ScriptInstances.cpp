#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Instances.hpp>
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
			// every other class does — and `Classes::Register` returning the
			// existing id for a repeated name would have hidden a second root
			// rather than prevented one.
			scene::PartClass();

			// Through the one function, so a caller that registers the class
			// tree cannot end up with a `SourceCache` the snapshot writer
			// refuses — a resource keyed by an unregistered type is minted
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
			const std::array source{ecs::Components::Of<Source>()};
			const ecs::ClassId container = ecs::Classes::Register("LuaSourceContainer", instance, source);

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

			ecs::Classes::Property<&Source::Path>(container, "Source");
			ecs::Classes::Computed(container, DisabledProperty());
			return script;
		}
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
		store.Each<const Source>([&](ecs::Entity entity, const Source &) {
			// A disabled script is in another archetype, so this query does not
			// visit one — but the check is here anyway, because `Each` matches
			// on `Source` alone and a caller could add the tag to a row this
			// query already found in the same tick.
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

	ecs::ClassId ModuleScriptClass() {
		ScriptClass();
		return ecs::Classes::Find(core::Name("ModuleScript"));
	}

	ecs::Entity MakeModule(ecs::Store &store, std::string_view path, std::string_view name) {
		const ecs::Entity instance = store.CreateInstance(ModuleScriptClass(), name);
		if (instance == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		store.Set(instance, Source{core::Name(path)});
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

		store.Set(instance, Source{core::Name(path)});
		return instance;
	}
}
