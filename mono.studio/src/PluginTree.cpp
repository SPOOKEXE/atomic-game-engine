// A script plugin's instance subtree: the `Plugin` object under
// `PluginGuiService`, its folder mirrored as instances, and the string
// `require` that walks that tree.
//
// **The tree is what the plugin is, and the files are how it was written.** A
// plugin may be one script, a folder of instances that includes scripts, or
// several scripts under one parent, and all three become the same shape here:
// a `Plugin` instance with the scripts and everything else parented to it,
// which is Roblox's model. A `.model.json`, `.rbxm` or `.rbxmx` file becomes the
// instances it describes, built as a Rojo sync builds it. `require("./a")` resolves over that tree rather
// than over the disk, so it returns the same module as `require(script.Parent.a)`.
//
// Not a second source of truth for the files: every script carries the program
// it was built with, and `PluginReload` rebuilds the whole tree when any file
// under the plugin's folder changes.

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <studio/Config.hpp>
#include <studio/Plugins.hpp>
#include <studio/RojoSync.hpp>
#include <system_error>
#include <vector>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::Store;

	namespace {
		bool ReadFile(const std::filesystem::path &path, std::string &out) {
			std::ifstream file(path, std::ios::binary);
			if (!file) return false;
			std::ostringstream contents;
			contents << file.rdbuf();
			out = contents.str();
			return true;
		}

		bool EndsWith(std::string_view text, std::string_view suffix) {
			return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
		}

		// What a file becomes. Empty class means the file is not mirrored.
		struct FileShape {
			engine::ecs::ClassId Class;
			std::string Name;
		};

		FileShape ShapeOf(const std::string &file) {
			for (const std::string_view extension : {".luau", ".lua", ".js"}) {
				if (!EndsWith(file, extension)) continue;
				const std::string stem = file.substr(0, file.size() - extension.size());
				if (EndsWith(stem, ".server")) {
					return {engine::script::ScriptClass(), stem.substr(0, stem.size() - 7)};
				}
				if (EndsWith(stem, ".client")) {
					return {engine::script::LocalScriptClass(), stem.substr(0, stem.size() - 7)};
				}
				return {engine::script::ModuleScriptClass(), stem};
			}
			return {};
		}

		engine::script::Language LanguageOfFile(const std::filesystem::path &file) {
			return file.extension() == ".js" ? engine::script::Language::JavaScript
											 : engine::script::Language::Luau;
		}

		// The init file that makes a folder a script, if it has one.
		std::filesystem::path InitOf(const std::filesystem::path &folder) {
			for (const char *name :
				 {"init.luau",
				  "init.server.luau",
				  "init.client.luau",
				  "init.lua",
				  "init.server.lua",
				  "init.client.lua",
				  "init.js",
				  "init.server.js",
				  "init.client.js"}) {
				std::error_code failed;
				if (std::filesystem::is_regular_file(folder / name, failed)) return folder / name;
			}
			return {};
		}

		struct Mirror {
			LoadedPlugin &Plugin;
			Store &World;
			std::filesystem::path Main;
			Entity MainScript;
			std::vector<Entity> Scripts;

			// One script instance carrying its file's program.
			Entity
			MakeScript(engine::ecs::ClassId type, std::string_view name, const std::filesystem::path &file) {
				if (file == Main) type = engine::script::ScriptClass();
				const Entity script = World.CreateInstance(type, name);
				if (script == engine::ecs::NULL_ENTITY) return script;

				// An absolute path, so it can never collide with a game's own
				// relative source paths in the world's cache.
				const engine::core::Name path(
					std::filesystem::absolute(file).lexically_normal().generic_string()
				);
				engine::script::SetSourcePath(World, script, path);
				std::string text;
				if (ReadFile(file, text)) World.Set(script, engine::script::Program{path, std::move(text)});
				Plugin.Files[script.Id] = file.lexically_relative(Plugin.Root).generic_string();

				if (file == Main) {
					MainScript = script;
				} else if ((type == engine::script::ScriptClass() ||
							type == engine::script::LocalScriptClass()) &&
						   LanguageOfFile(file) == Plugin.Language) {
					// A script in the other language stays an instance and
					// does not run: one runtime runs one language.
					Scripts.push_back(script);
				}
				return script;
			}

			// A Rojo model file as the instances it describes, through the same
			// builder a project sync uses. What it could not build is logged.
			void Model(const std::filesystem::path &file, Entity parent) {
				RojoSyncReport report;
				const std::string key =
					std::filesystem::absolute(file.parent_path()).lexically_normal().generic_string() + "/";
				const Entity model = BuildRojoModelFile(World, file, key, report);
				for (const std::string &note : report.Notes) {
					ENGINE_WARN("plugin '{}': {}", Plugin.Manifest.Name, note);
				}
				for (const std::string &missing : report.Missing) {
					ENGINE_WARN("plugin '{}': could not read {}", Plugin.Manifest.Name, missing);
				}
				if (model != engine::ecs::NULL_ENTITY) World.SetParent(model, parent);
			}

			// Mirrors `folder`'s entries under `parent`, leaving out `init`,
			// the file that already became `parent` itself.
			void Folder(
				const std::filesystem::path &folder, Entity parent, const std::filesystem::path &init = {}
			) {
				std::vector<std::filesystem::path> entries;
				std::error_code failed;
				for (const auto &entry : std::filesystem::directory_iterator(folder, failed)) {
					entries.push_back(entry.path());
				}
				// Sorted, so a plugin's scripts run in the same order on every disk.
				std::sort(entries.begin(), entries.end());

				for (const std::filesystem::path &entry : entries) {
					const std::string name = entry.filename().string();
					if (name.empty() || name.front() == '.' || name == "plugin.json") continue;

					if (std::filesystem::is_directory(entry, failed)) {
						const std::filesystem::path init = InitOf(entry);
						Entity node;
						if (!init.empty()) {
							node = MakeScript(ShapeOf(init.filename().string()).Class, name, init);
						} else {
							node = World.CreateInstance(FolderClass(), name);
						}
						if (node == engine::ecs::NULL_ENTITY) continue;
						World.SetParent(node, parent);
						Folder(entry, node, init);
						continue;
					}

					if (entry == init) continue;
					if (EndsWith(name, ".model.json") || EndsWith(name, ".rbxm") ||
						EndsWith(name, ".rbxmx")) {
						Model(entry, parent);
						continue;
					}
					const FileShape shape = ShapeOf(name);
					if (!shape.Class.IsValid() && entry != Main) continue;
					const engine::ecs::ClassId type =
						shape.Class.IsValid() ? shape.Class : engine::script::ScriptClass();
					const Entity script =
						MakeScript(type, shape.Name.empty() ? entry.stem().string() : shape.Name, entry);
					if (script != engine::ecs::NULL_ENTITY) World.SetParent(script, parent);
				}
			}
		};

		// The `Plugin` instance holding `instance`, or null.
		Entity PluginRootOf(const Store &store, Entity instance, Entity root) {
			for (Entity at = instance; at != engine::ecs::NULL_ENTITY; at = store.ParentOf(at)) {
				if (at == root) return root;
			}
			return engine::ecs::NULL_ENTITY;
		}
	}

	PluginInstanceTree::~PluginInstanceTree() {
		Reset();
	}

	PluginInstanceTree::PluginInstanceTree(PluginInstanceTree &&other) noexcept
		: World(other.World), Root(other.Root) {
		other.World = nullptr;
		other.Root = engine::ecs::NULL_ENTITY;
	}

	PluginInstanceTree &PluginInstanceTree::operator=(PluginInstanceTree &&other) noexcept {
		if (this != &other) {
			Reset();
			World = other.World;
			Root = other.Root;
			other.World = nullptr;
			other.Root = engine::ecs::NULL_ENTITY;
		}
		return *this;
	}

	void PluginInstanceTree::Reset() {
		if (World != nullptr && Root != engine::ecs::NULL_ENTITY && World->Alive(Root)) {
			World->Destroy(Root);
		}
		World = nullptr;
		Root = engine::ecs::NULL_ENTITY;
	}

	Entity EnsurePluginGuiService(Store &store) {
		engine::script::ScriptClass();
		const Entity found = store.FindFirstRoot("PluginGuiService");
		if (found != engine::ecs::NULL_ENTITY) return found;

		const engine::ecs::ClassId service =
			engine::ecs::Classes::Find(engine::core::Name("PluginGuiService"));
		const Entity made = store.CreateInstance(service, "PluginGuiService");
		if (made != engine::ecs::NULL_ENTITY) store.Set(made, engine::ecs::NotArchivable{});
		return made;
	}

	std::vector<Entity> BuildPluginInstances(LoadedPlugin &plugin, Store &store) {
		plugin.Tree.Reset();
		plugin.Files.clear();

		const Entity service = EnsurePluginGuiService(store);
		const engine::ecs::ClassId pluginClass = engine::ecs::Classes::Find(engine::core::Name("Plugin"));
		const Entity root = store.CreateInstance(pluginClass, plugin.Manifest.Name);
		if (root == engine::ecs::NULL_ENTITY) return {};
		store.Set(root, engine::ecs::NotArchivable{});
		store.SetParent(root, service);
		plugin.Tree = PluginInstanceTree(store, root);

		Mirror mirror{plugin, store, (plugin.Root / plugin.Manifest.Main).lexically_normal(), {}, {}};
		mirror.Folder(plugin.Root, root);

		std::vector<Entity> scripts;
		if (mirror.MainScript != engine::ecs::NULL_ENTITY) scripts.push_back(mirror.MainScript);
		scripts.insert(scripts.end(), mirror.Scripts.begin(), mirror.Scripts.end());
		return scripts;
	}

	bool ResolvePluginRequire(
		const LoadedPlugin &plugin,
		const Store &store,
		Entity requirer,
		std::string_view path,
		Entity &module,
		std::string &error
	) {
		const Entity root = plugin.Tree.Instance();
		const std::string quoted = "require('" + std::string(path) + "')";
		if (root == engine::ecs::NULL_ENTITY ||
			PluginRootOf(store, requirer, root) == engine::ecs::NULL_ENTITY) {
			error = quoted + " is only resolved inside plugin '" + plugin.Manifest.Name + "'";
			return false;
		}

		std::string_view rest = path;
		Entity at = requirer;
		if (rest.starts_with("@self/")) {
			rest.remove_prefix(6);
		} else if (rest.starts_with("./") || rest.starts_with("../")) {
			at = store.ParentOf(requirer);
		} else {
			error = quoted + " must start with ./, ../ or @self/";
			return false;
		}

		const auto outside = [&] {
			error = quoted + " leaves plugin '" + plugin.Manifest.Name + "'";
			return false;
		};

		while (!rest.empty()) {
			const size_t slash = rest.find('/');
			std::string_view part = rest.substr(0, slash);
			rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
			if (part.empty() || part == ".") continue;
			if (part == "..") {
				if (at == root) return outside();
				at = store.ParentOf(at);
				if (at == engine::ecs::NULL_ENTITY) return outside();
				continue;
			}

			// A file name with its extension names the same module as without.
			for (const std::string_view extension : {".luau", ".lua", ".js"}) {
				if (EndsWith(part, extension)) part.remove_suffix(extension.size());
			}
			const Entity child = store.FindFirstChild(at, part);
			if (child == engine::ecs::NULL_ENTITY) {
				const auto here = plugin.Files.find(requirer.Id);
				error = quoted + " in plugin '" + plugin.Manifest.Name + "': no '" + std::string(part) + "'" +
						(here != plugin.Files.end() ? " (from " + here->second + ")" : std::string());
				return false;
			}
			at = child;
		}

		if (PluginRootOf(store, at, root) == engine::ecs::NULL_ENTITY || at == root) return outside();
		if (!store.IsA(at, engine::script::ModuleScriptClass())) {
			error = quoted + " in plugin '" + plugin.Manifest.Name + "' names '" +
					std::string(store.InstanceNameOf(at).Text()) + "', which is not a ModuleScript";
			return false;
		}
		module = at;
		return true;
	}

	std::filesystem::path PluginSettingsPath(const PluginPresentation &plugin) {
		return ConfigRoot() / "plugin-settings" / (PluginIdentity(plugin) + ".json");
	}

	void UnloadPlugins(std::vector<LoadedPlugin> &plugins) {
		for (LoadedPlugin &plugin : plugins) {
			const Entity instance = plugin.Tree.Instance();
			if (!plugin.Running || plugin.Vm == nullptr || instance == engine::ecs::NULL_ENTITY) continue;
			if (!plugin.Vm->FireHostEvent(instance, "Unloading", {})) {
				const std::string &error = plugin.Vm->LastError();
				if (!error.empty())
					ENGINE_WARN("plugin '{}' Unloading raised: {}", plugin.Manifest.Name, error);
			}
		}
		plugins.clear();
	}
}
