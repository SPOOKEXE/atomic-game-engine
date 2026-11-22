#pragma once

// `require("./x")`: a path resolved over the instance tree, relative to the
// requiring script.
//
// **One walk for games and plugins.** Both VMs and Studio's plugin host resolve
// a path here, so a path names the same instance everywhere and the module cache,
// which is keyed by instance, makes `require("./a")` and `require(script.Parent.a)`
// one module. The rule is Roblox's path require:
//
// | Prefix | Starts at |
// |---|---|
// | `./` | the script's parent |
// | `../` | the script's parent's parent |
// | `@self/` | the script itself |
//
// After the prefix, `..` steps to a parent and any other part names a child.
// `.luau`, `.lua` and `.js` on a part are ignored, so a file name works.
//
// Not a file lookup: nothing here reads a disk.
//
// @tier L9 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/ecs/StoreFwd.hpp>

#include <string>
#include <string_view>

namespace engine::script {

	// Why a path did not resolve, so each caller can say it in its own words.
	enum class RequirePathFailure {
		None,
		// The path does not start with `./`, `../` or `@self/`.
		Prefix,
		// The path climbs above its scope's root, or names the root itself.
		Leaves,
		// A part names no child. `RequirePathResolution::Part` is that part.
		Missing,
		// The path names an instance that is not a `ModuleScript`.
		NotModule,
	};

	// The outcome of one path walk.
	struct RequirePathResolution {
		RequirePathFailure Failure = RequirePathFailure::None;
		// The module on success, or what the path named for `NotModule`.
		ecs::Entity Found{};
		// The part that named nothing, for `Missing`.
		std::string Part;
	};

	// Walks `path` from `requirer`.
	//
	// @param store    The world the requirer is in.
	// @param requirer The script calling `require`.
	// @param path     The string passed to `require`.
	// @param root     The instance a path may not leave: a plugin's `Plugin`
	//                 instance. Null for a game, where `..` above a service
	//                 reaches the game itself and a name there is a service.
	// @return What the path named, or why it named nothing.
	RequirePathResolution
	WalkRequirePath(const ecs::Store &store, ecs::Entity requirer, std::string_view path, ecs::Entity root);

	// A game script's string `require`: the walk with no root, refusing
	// anything under `PluginGuiService`, which a game cannot reach.
	//
	// The default every runtime uses when its host supplies no resolver.
	//
	// @param error Filled with a message for the script author on failure.
	// @return Whether `module` was filled.
	bool ResolveGameRequire(
		const ecs::Store &store,
		ecs::Entity requirer,
		std::string_view path,
		ecs::Entity &module,
		std::string &error
	);
}
