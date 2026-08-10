#include <studio/RojoSync.hpp>

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
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

		// What Rojo says a file becomes, for the ones this engine cannot build.
		//
		// **Named by what they *would* be rather than skipped as unrecognised**,
		// and the difference matters to whoever reads the log: "not a script" is
		// what you say about a stray `.DS_Store`, and it is the wrong thing to
		// say about a `.rbxm`, which Rojo maps and this engine has no reader
		// for. One is noise in the project and the other is a gap here.
		//
		// The list is `rojo.space/docs/v7/sync-details`. `D00104` carries what
		// closing each of them would take.
		const char *UnbuiltKind(const std::filesystem::path &file) {
			const std::string leaf = file.filename().string();

			// **Before the plain `.json` case**, because both suffixes match and
			// the longer one is the specific rule. A `.meta.json` beside a file
			// is properties for it, which is a patch this sync has nowhere to
			// apply yet.
			if (EndsWith(leaf, ".meta.json")) {
				return "a property patch (.meta.json), which this sync cannot apply yet";
			}
			if (EndsWith(leaf, ".model.json")) {
				return "a model definition (.model.json), which this sync cannot build yet";
			}
			if (EndsWith(leaf, ".project.json")) {
				return "a nested project, which this sync does not follow yet";
			}
			if (EndsWith(leaf, ".rbxm") || EndsWith(leaf, ".rbxmx")) {
				return "a Roblox model, which this engine has no reader for";
			}
			if (EndsWith(leaf, ".txt")) {
				return "a StringValue, which this engine has no class for";
			}
			if (EndsWith(leaf, ".csv")) {
				return "a LocalizationTable, which this engine has no class for";
			}
			if (EndsWith(leaf, ".json") || EndsWith(leaf, ".toml")) {
				return "a ModuleScript returning a table, which this sync cannot generate yet";
			}
			return nullptr;
		}

		// The `init` file a directory carries, which makes the directory itself
		// the script rather than a folder holding one.
		//
		// **Rojo's rule is that only one may be present**, and this engine has
		// to do something when an author breaks it. Refusing the whole sync
		// would be one mistake costing every other folder; picking silently
		// would be a directory whose class depends on which name happened to
		// sort first. So the order is fixed and written down — module, then
		// server, then client — and the extras are reported.
		struct InitFile {
			std::filesystem::path File;
			bool Local = false;
			bool Module = false;

			bool Present() const {
				return !File.empty();
			}
		};

		InitFile FindInit(const std::filesystem::path &directory, RojoSyncReport &report) {
			static const struct {
				const char *Leaf;
				bool Local;
				bool Module;
			} CANDIDATES[] = {
				{"init.luau", false, true},
				{"init.lua", false, true},
				{"init.server.luau", false, false},
				{"init.server.lua", false, false},
				{"init.client.luau", true, false},
				{"init.client.lua", true, false},
			};

			InitFile chosen;
			for (const auto &candidate : CANDIDATES) {
				const std::filesystem::path file = directory / candidate.Leaf;

				std::error_code kind;
				if (!std::filesystem::is_regular_file(file, kind)) {
					continue;
				}

				if (chosen.Present()) {
					report.Notes.push_back(
						directory.filename().string() + " has more than one init file — used " +
						chosen.File.filename().string() + " and ignored " + candidate.Leaf
					);
					continue;
				}

				chosen.File = file;
				chosen.Local = candidate.Local;
				chosen.Module = candidate.Module;
			}
			return chosen;
		}

		// Whether a file is *an* init file, whichever one the directory chose.
		//
		// The loop over a directory has to skip every one of them: the chosen
		// one was consumed by the directory itself, and an ignored one must not
		// come back as a child called `init`. That second case is the bug this
		// replaced — only `init.luau` was skipped, so `init.server.luau` became
		// a folder plus a stray `Script` named `init`.
		bool IsInitFile(const std::filesystem::path &file) {
			const std::string leaf = file.filename().string();
			return leaf == "init.luau" || leaf == "init.lua" || leaf == "init.server.luau" ||
				   leaf == "init.server.lua" || leaf == "init.client.luau" ||
				   leaf == "init.client.lua";
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

					// An `init` file makes the directory itself the script
					// rather than a folder containing one — which is how a
					// program gets children without every path gaining a level.
					//
					// **Which class it becomes is the init file's suffix**,
					// exactly as it is for any other file: `init.luau` is a
					// module, `init.server.luau` a `Script`, `init.client.luau`
					// a `LocalScript`. Reading only `init.luau` — which this did
					// — made every `init.server.luau` project a folder plus a
					// stray script called `init`.
					Entity node = NULL_ENTITY;
					const InitFile init = FindInit(entry, report);

					if (init.Present()) {
						const std::string key =
							keyPrefix + leaf + "/" + init.File.filename().string();
						if (StageProgram(store, init.File, key)) {
							node = init.Module
									   ? engine::script::MakeModule(store, key, leaf)
									   : engine::script::MakeScript(store, key, leaf, init.Local);
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

				if (IsInitFile(entry)) {
					// Consumed by the directory above, or reported there as one
					// init file too many. Either way it is not a child.
					continue;
				}

				const ScriptFile file = ClassifyFile(entry);
				if (!file.IsScript) {
					// Named rather than skipped in silence, so an author whose
					// `.rbxm` did not appear knows why — and named by *what Rojo
					// says it is* where there is an answer, so a gap here reads
					// as a gap rather than as an unrecognised file.
					if (const char *kind_ = UnbuiltKind(entry); kind_ != nullptr) {
						report.Notes.push_back(
							entry.filename().string() + " is " + kind_ + " — skipped"
						);
					} else {
						report.Notes.push_back(
							entry.filename().string() + " is not a script — skipped"
						);
					}
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
					if (!file.IsScript) {
						// The same accounting a directory walk does. A `$path`
						// naming a `.rbxm` used to produce nothing and say
						// nothing, which is the one outcome an author cannot
						// act on.
						const char *kind_ = UnbuiltKind(source);
						report.Notes.push_back(
							node.Path + " is " +
							(kind_ != nullptr ? kind_ : "not a script") + " — skipped"
						);
					} else if (StageProgram(store, source, node.Path)) {
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
			engine::scene::EnsureClassTree();
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

	// --- the universe above them -------------------------------------------

	size_t RojoUniverseReport::Synced() const {
		return static_cast<size_t>(
			std::count_if(Worlds.begin(), Worlds.end(), [](const RojoWorldSync &world) {
				return world.Synced;
			})
		);
	}

	size_t RojoUniverseReport::Failed() const {
		return Worlds.size() - Synced();
	}

	bool ParseRojoUniverse(std::string_view json_, RojoUniverse &out, std::string &error) {
		json document = json::parse(json_, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "not a JSON object";
			return false;
		}

		const auto worlds = document.find("worlds");
		if (worlds == document.end() || !worlds->is_object()) {
			error = "no 'worlds' — this is not a universe file";
			return false;
		}

		if (const auto name = document.find("name"); name != document.end() && name->is_string()) {
			out.Name = name->get<std::string>();
		}

		for (const auto &entry : worlds->items()) {
			if (entry.key().empty() || !entry.value().is_string()) {
				// **Skipped and not fatal**, which is the rule the whole layer
				// is built on: one malformed entry costs its own world. A
				// refusal here would be one typo taking every other world with
				// it, which is exactly what syncing them separately is for.
				continue;
			}
			out.Worlds.push_back(RojoUniverseWorld{entry.key(), entry.value().get<std::string>()});
		}

		if (out.Worlds.empty()) {
			error = "'worlds' names nothing";
			return false;
		}
		return true;
	}

	std::filesystem::path RojoProjectFor(const std::filesystem::path &root, const RojoUniverseWorld &world) {
		if (world.Path.empty()) {
			return {};
		}

		const std::filesystem::path candidate = root / world.Path;
		std::error_code kind;

		if (std::filesystem::is_regular_file(candidate, kind)) {
			return candidate;
		}
		if (!std::filesystem::is_directory(candidate, kind)) {
			return {};
		}

		// Rojo's own name first, so a subfolder stays a project every tool in
		// that ecosystem understands.
		for (const char *leaf : {"default.project.json", "main.default.json"}) {
			const std::filesystem::path project = candidate / leaf;
			if (std::filesystem::is_regular_file(project, kind)) {
				return project;
			}
		}
		return {};
	}

	bool SyncRojoUniverse(
		const RojoUniverse &universe,
		const std::filesystem::path &root,
		engine::world::Universe &worlds,
		RojoUniverseReport &report,
		std::string &error
	) {
		std::error_code failed;
		if (!std::filesystem::is_directory(root, failed)) {
			error = root.string() + " is not a directory";
			return false;
		}

		for (const RojoUniverseWorld &declared : universe.Worlds) {
			RojoWorldSync &result = report.Worlds.emplace_back();
			result.World = declared.Name;

			result.Project = RojoProjectFor(root, declared);
			if (result.Project.empty()) {
				result.Error = "no project file at " + declared.Path;
				continue;
			}

			std::ifstream in(result.Project, std::ios::binary);
			if (!in) {
				result.Error = "could not read " + result.Project.string();
				continue;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();

			RojoProject project;
			if (!ParseRojoProject(buffer.str(), project, result.Error)) {
				continue;
			}

			// **Found before created**, so a second sync builds into the world
			// the first one made rather than beside it. `Universe::Create`
			// already returns the world holding a name, and this says so at the
			// call site because the difference decides whether an author's
			// hand-placed instances survive.
			const engine::core::Name key(declared.Name);
			engine::world::WorldId id = worlds.Find(key);
			bool created = false;

			if (!id.IsValid()) {
				engine::world::WorldSettings settings;
				settings.Name = key;

				engine::world::WorldStatus status = engine::world::WorldStatus::Ok;
				id = worlds.Create(settings, &status);
				created = true;

				if (!id.IsValid()) {
					result.Error = "the driver refused to create a world called " + declared.Name;
					continue;
				}
			}

			const std::filesystem::path directory = result.Project.parent_path();
			std::string built;

			worlds.Enter(id, [&](Store &store) {
				// A world this call made has none of the services a place
				// needs, and `BuildNode` reuses an existing `Workspace` rather
				// than making a second — so installing them first is what stops
				// a synced world from having two.
				if (created) {
					engine::scene::InstallServices(store);
				}
				if (!SyncRojoProject(project, directory, store, result.Report, built)) {
					result.Error = built;
					return;
				}
				result.Synced = true;
			});

			if (!result.Synced && result.Error.empty()) {
				// `Enter` refused — the world is remote, faulted or held down.
				// Named rather than left as a silent failure, because "nothing
				// happened and nothing said why" is the report this layer exists
				// to avoid.
				result.Error = "could not enter world " + declared.Name;
			}
		}

		if (report.Synced() == 0) {
			error = "no world in the universe could be built";
			return false;
		}
		return true;
	}
}
