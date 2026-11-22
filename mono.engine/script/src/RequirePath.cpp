#include <engine/ecs/Store.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/RequirePath.hpp>

namespace engine::script {

	namespace {
		bool EndsWith(std::string_view text, std::string_view suffix) {
			return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
		}

		bool Within(const ecs::Store &store, ecs::Entity instance, ecs::Entity ancestor) {
			for (ecs::Entity at = instance; at != ecs::NULL_ENTITY; at = store.ParentOf(at)) {
				if (at == ancestor) return true;
			}
			return false;
		}
	}

	RequirePathResolution
	WalkRequirePath(const ecs::Store &store, ecs::Entity requirer, std::string_view path, ecs::Entity root) {
		const bool scoped = root != ecs::NULL_ENTITY;
		if (scoped && !Within(store, requirer, root))
			return {.Failure = RequirePathFailure::Leaves, .Found = {}, .Part = {}};

		std::string_view rest = path;
		ecs::Entity at = requirer;
		if (rest.starts_with("@self/")) {
			rest.remove_prefix(6);
		} else if (rest.starts_with("./") || rest.starts_with("../")) {
			at = store.ParentOf(requirer);
		} else {
			return {.Failure = RequirePathFailure::Prefix, .Found = {}, .Part = {}};
		}

		// In a game the services are roots, so the step above one is the game
		// itself (Roblox's DataModel), where a name is a service. A plugin is
		// scoped to its own instance and never gets there.
		bool atGame = !scoped && at == ecs::NULL_ENTITY;

		while (!rest.empty()) {
			const size_t slash = rest.find('/');
			std::string_view part = rest.substr(0, slash);
			rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
			if (part.empty() || part == ".") continue;
			if (part == "..") {
				if (atGame || at == root)
					return {.Failure = RequirePathFailure::Leaves, .Found = {}, .Part = {}};
				at = store.ParentOf(at);
				if (at == ecs::NULL_ENTITY) {
					if (scoped) return {.Failure = RequirePathFailure::Leaves, .Found = {}, .Part = {}};
					atGame = true;
				}
				continue;
			}

			// A file name with its extension names the same module as without.
			for (const std::string_view extension : {".luau", ".lua", ".js"}) {
				if (EndsWith(part, extension)) part.remove_suffix(extension.size());
			}
			const ecs::Entity child = atGame ? store.FindFirstRoot(part) : store.FindFirstChild(at, part);
			if (child == ecs::NULL_ENTITY) {
				return {.Failure = RequirePathFailure::Missing, .Found = {}, .Part = std::string(part)};
			}
			at = child;
			atGame = false;
		}

		if (atGame || at == root || (scoped && !Within(store, at, root))) {
			return {.Failure = RequirePathFailure::Leaves, .Found = {}, .Part = {}};
		}
		if (!store.IsA(at, ModuleScriptClass()))
			return {.Failure = RequirePathFailure::NotModule, .Found = at, .Part = {}};
		return {.Failure = RequirePathFailure::None, .Found = at, .Part = {}};
	}

	bool ResolveGameRequire(
		const ecs::Store &store,
		ecs::Entity requirer,
		std::string_view path,
		ecs::Entity &module,
		std::string &error
	) {
		const std::string quoted = "require('" + std::string(path) + "')";
		if (requirer == ecs::NULL_ENTITY || !store.Alive(requirer)) {
			error = quoted + " has no requiring script to be relative to";
			return false;
		}
		const std::string from = " from " + store.GetFullName(requirer);
		const RequirePathResolution found = WalkRequirePath(store, requirer, path, ecs::NULL_ENTITY);

		// A plugin's tree is Studio's, not the game's, whichever way a path
		// tried to reach it.
		const ecs::Entity plugins = store.FindFirstRoot("PluginGuiService");
		const bool reachesPlugins = found.Found != ecs::NULL_ENTITY && plugins != ecs::NULL_ENTITY &&
									Within(store, found.Found, plugins);

		switch (found.Failure) {
		case RequirePathFailure::None:
			if (reachesPlugins) break;
			module = found.Found;
			return true;
		case RequirePathFailure::Prefix:
			error = quoted + " must start with ./, ../ or @self/";
			return false;
		case RequirePathFailure::Leaves:
			error = quoted + from + " leaves the game";
			return false;
		case RequirePathFailure::Missing:
			error = quoted + from + ": no '" + found.Part + "'";
			return false;
		case RequirePathFailure::NotModule:
			if (reachesPlugins) break;
			error =
				quoted + from + " names " + store.GetFullName(found.Found) + ", which is not a ModuleScript";
			return false;
		}
		error = quoted + from + " leaves the game";
		return false;
	}
}
