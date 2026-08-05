#include <studio/RojoSync.hpp>

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace studio {

	namespace {
		using engine::core::Name;
		using engine::ecs::ClassId;
		using engine::ecs::Classes;
		using engine::ecs::Entity;
		using engine::ecs::NULL_ENTITY;
		using engine::ecs::Store;
		using nlohmann::json;

		// Whether a name ends with a suffix.
		bool EndsWith(std::string_view text, std::string_view suffix) {
			return text.size() >= suffix.size() &&
				   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		// What a file becomes, if anything.
		//
		// **`.server` and `.client` decide the class, not a guess from the
		// folder.** Rojo's convention puts the intent in the file name, and a
		// rule based on which directory it happened to sit in would disagree
		// with the same file moved.
		struct ScriptFile {
			bool IsScript = false;
			bool Local = false;

			// **A plain `.luau` is a module, which is Rojo's own rule.** Only the
			// suffixed files are programs the host runs; everything else in a
			// project is something a program requires. Mapping them all to
			// `Script` — which this did before `ModuleScript` existed — meant a
			// synced project executed every library it contained.
			bool Module = false;
			std::string Name;
		};

		ScriptFile ClassifyFile(const std::filesystem::path &file) {
			ScriptFile found;

			const std::string leaf = file.filename().string();
			if (!EndsWith(leaf, ".luau") && !EndsWith(leaf, ".lua")) {
				return found;
			}

			std::string stem = file.stem().string();
			found.IsScript = true;

			if (EndsWith(stem, ".server")) {
				stem.resize(stem.size() - 7);
			} else if (EndsWith(stem, ".client")) {
				stem.resize(stem.size() - 7);
				found.Local = true;
			} else {
				found.Module = true;
			}

			found.Name = stem;
			return found;
		}

		// The class a `$className` names, or an invalid id.
		//
		// **`Folder` is substituted rather than refused when a class is not
		// registered.** A project file is written against Roblox's whole class
		// tree and this engine has a fraction of it; refusing to sync a project
		// because it mentions `Team` would make the feature unusable against
		// every real project. The substitution is reported, so the answer is
		// "this became a folder" rather than silence.
		ClassId ClassFor(std::string_view name, RojoSyncReport &report) {
			if (name.empty()) {
				return FolderClass();
			}

			const ClassId found = Classes::Find(Name(std::string(name)));
			if (found.IsValid()) {
				return found;
			}

			report.Notes.push_back(std::string(name) + " is not a class here — made a Folder instead");
			return FolderClass();
		}

		// Reads a file into the world's program table.
		//
		// **The text goes into `SourceCache`, not onto the filesystem the engine
		// reads assets from.** A Rojo project lives wherever its author keeps it,
		// which is not under `Paths::Assets()` — and `ReadSource` checks the
		// world's table first for exactly this kind of reason. The upshot is a
		// synced game that runs and saves without anything being copied.
		bool StageProgram(Store &store, const std::filesystem::path &file, const std::string &key) {
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();

			auto *cache = store.ResourceMutable<engine::script::SourceCache>();
			if (cache == nullptr) {
				store.SetResource(engine::script::SourceCache{});
				cache = store.ResourceMutable<engine::script::SourceCache>();
			}
			if (cache == nullptr) {
				return false;
			}

			cache->Set(Name(key), buffer.str());
			return true;
		}

		void ReadNode(const std::string &name, const json &value, RojoNode &out) {
			out.Name = name;

			if (!value.is_object()) {
				return;
			}

			if (const auto found = value.find("$className");
				found != value.end() && found->is_string()) {
				out.ClassName = found->get<std::string>();
			}
			if (const auto found = value.find("$path"); found != value.end() && found->is_string()) {
				out.Path = found->get<std::string>();
			}

			for (const auto &entry : value.items()) {
				// Anything beginning with `$` is a directive rather than a child.
				// `$properties`, `$ignoreUnknownInstances` and the rest are read
				// past rather than treated as instances named `$properties`.
				if (!entry.key().empty() && entry.key().front() == '$') {
					continue;
				}
				if (!entry.value().is_object()) {
					continue;
				}

				RojoNode child;
				ReadNode(entry.key(), entry.value(), child);
				out.Children.push_back(std::move(child));
			}
		}

		// Builds one directory into an instance tree.
		void BuildDirectory(
			Store &store,
			const std::filesystem::path &directory,
			Entity parent,
			const std::string &keyPrefix,
			RojoSyncReport &report
		) {
			// **Sorted, because a directory walk is not ordered.** Two syncs of
			// one tree have to produce the same creation order or the entity ids
			// differ between them, and an id that moves is a saved reference that
			// points somewhere else.
			std::vector<std::filesystem::path> entries;
			std::error_code failed;
			for (const auto &entry : std::filesystem::directory_iterator(directory, failed)) {
				entries.push_back(entry.path());
			}
			if (failed) {
				return;
			}
			std::sort(entries.begin(), entries.end());

			for (const std::filesystem::path &entry : entries) {
				std::error_code kind;
				if (std::filesystem::is_directory(entry, kind)) {
					const std::string leaf = entry.filename().string();

					// `init.luau` makes the directory itself the script rather
					// than a folder containing one — which is how a module gets
					// children without every path gaining a level.
					Entity node = NULL_ENTITY;
					const std::filesystem::path init = entry / "init.luau";

					if (std::filesystem::exists(init, kind)) {
						const std::string key = keyPrefix + leaf + "/init.luau";
						if (StageProgram(store, init, key)) {
							// A module, like any other plain `.luau`. `init` is
							// how a module gets children, not how a folder
							// becomes a program.
							node = engine::script::MakeModule(store, key, leaf);
							report.Scripts++;
						}
					}

					if (node == NULL_ENTITY) {
						node = store.CreateInstance(FolderClass(), leaf);
					}
					if (node == NULL_ENTITY) {
						continue;
					}

					report.Instances++;
					store.SetParent(node, parent);
					BuildDirectory(store, entry, node, keyPrefix + leaf + "/", report);
					continue;
				}

				const ScriptFile file = ClassifyFile(entry);
				if (!file.IsScript) {
					// Models, images and everything else a project may carry.
					// Named rather than skipped in silence, so an author whose
					// `.rbxm` did not appear knows why.
					report.Notes.push_back(entry.filename().string() + " is not a script — skipped");
					continue;
				}

				if (entry.filename() == "init.luau") {
					// Already consumed by the directory above it.
					continue;
				}

				const std::string key = keyPrefix + entry.filename().string();
				if (!StageProgram(store, entry, key)) {
					report.Missing.push_back(entry.string());
					continue;
				}

				const Entity script = file.Module
										  ? engine::script::MakeModule(store, key, file.Name)
										  : engine::script::MakeScript(store, key, file.Name, file.Local);
				if (script == NULL_ENTITY) {
					continue;
				}

				store.SetParent(script, parent);
				report.Instances++;
				report.Scripts++;
			}
		}

		void BuildNode(
			Store &store,
			const RojoNode &node,
			const std::filesystem::path &root,
			Entity parent,
			RojoSyncReport &report
		) {
			// **An existing instance of that name is reused, not duplicated.**
			// `scene::InstallServices` has already put `Workspace` and the rest
			// into the world, and a sync that made a second `ReplicatedStorage`
			// beside the real one would produce a tree where half the game
			// cannot find the other half.
			Entity node_ = parent == NULL_ENTITY ? store.FindFirstRoot(node.Name)
												 : store.FindFirstChild(parent, node.Name);

			if (node_ == NULL_ENTITY) {
				node_ = store.CreateInstance(ClassFor(node.ClassName, report), node.Name);
				if (node_ == NULL_ENTITY) {
					return;
				}
				report.Instances++;
				if (parent != NULL_ENTITY) {
					store.SetParent(node_, parent);
				}
			}

			if (!node.Path.empty()) {
				const std::filesystem::path source = root / node.Path;
				std::error_code kind;

				if (std::filesystem::is_directory(source, kind)) {
					BuildDirectory(store, source, node_, node.Path + "/", report);
				} else if (std::filesystem::exists(source, kind)) {
					const ScriptFile file = ClassifyFile(source);
					if (file.IsScript && StageProgram(store, source, node.Path)) {
						const Entity script =
							file.Module ? engine::script::MakeModule(store, node.Path, file.Name)
										: engine::script::MakeScript(
											  store, node.Path, file.Name, file.Local
										  );
						if (script != NULL_ENTITY) {
							store.SetParent(script, node_);
							report.Instances++;
							report.Scripts++;
						}
					}
				} else {
					report.Missing.push_back(node.Path);
				}
			}

			for (const RojoNode &child : node.Children) {
				BuildNode(store, child, root, node_, report);
			}
		}
	}

	engine::ecs::ClassId FolderClass() {
		// A function-local static, so the class exists before the first caller
		// reads an id from it and cannot be registered twice.
		static const ClassId folder = [] {
			// Through `PartClass` first, so the tree's root exists. A second
			// root would be a class tree nothing could compare across.
			(void)engine::scene::PartClass();
			const ClassId instance = Classes::Find(Name("Instance"));
			return Classes::Register("Folder", instance, {});
		}();
		return folder;
	}

	bool ParseRojoProject(std::string_view json_, RojoProject &out, std::string &error) {
		json document = json::parse(json_, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "not a JSON object";
			return false;
		}

		const auto tree = document.find("tree");
		if (tree == document.end() || !tree->is_object()) {
			error = "no 'tree' — this is not a Rojo project file";
			return false;
		}

		if (const auto name = document.find("name"); name != document.end() && name->is_string()) {
			out.Name = name->get<std::string>();
		}

		// The tree's own name is the project's, because the root of a place is
		// the `DataModel` and nothing else names it.
		ReadNode(out.Name.empty() ? "Game" : out.Name, *tree, out.Tree);
		return true;
	}

	bool SyncRojoProject(
		const RojoProject &project,
		const std::filesystem::path &root,
		Store &store,
		RojoSyncReport &report,
		std::string &error
	) {
		std::error_code failed;
		if (!std::filesystem::is_directory(root, failed)) {
			error = root.string() + " is not a directory";
			return false;
		}

		(void)FolderClass();
		engine::script::RegisterScriptComponents();

		// **The tree's own children, not the tree itself.** The root node is the
		// `DataModel`, which this engine models as the world rather than as an
		// instance in it — so its children become the world's roots.
		for (const RojoNode &child : project.Tree.Children) {
			BuildNode(store, child, root, NULL_ENTITY, report);
		}

		if (report.Instances == 0) {
			error = "the project named nothing this world could build";
			return false;
		}
		return true;
	}
}
