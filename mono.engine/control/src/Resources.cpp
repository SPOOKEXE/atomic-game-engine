// The things a model should have before it asks its first question.
//
// **A resource is not a tool that returns a document.** MCP clients list
// resources when they connect and offer them for attachment, so what belongs
// here is context rather than action: the layer stack a change has to obey, the
// storage the engine has, the module graph, and the `AGENTS.md` files this
// repository says to read before touching anything. A model that had to *call*
// for those would call for them after it had already written the wrong thing.
//
// **Three of them are compiled in or read out of the running process, and the
// rest are files.** The layer table, the module graph and the component
// catalogue are properties of the build and answer everywhere. The `AGENTS.md`
// files and the scripting manifest are a working tree, so they are listed only
// when this executable was staged into one - the same rule the tool table
// follows, and for the same reason: being told what a program can do beats
// discovering it by asking for something that fails.
//
// The addresses are `atomic://`, which is this engine's own scheme rather than
// `file://`. Two of them are not files at all, and a client that resolved a
// `file://` against its own filesystem would be reading a different machine's.

#include "Catalogue.hpp"
#include "Repository.hpp"

#include <engine/control/Architecture.hpp>
#include <engine/control/Surface.hpp>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace {
		std::string ReadResourceFile(const std::filesystem::path &path, std::string &failure) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				failure = "could not read " + path.string();
				return {};
			}
			std::ostringstream contents;
			contents << file.rdbuf();
			return contents.str();
		}

		// The layer stack as a table somebody reads rather than parses.
		//
		// Markdown rather than JSON, because this is the resource whose whole
		// job is to be read: `atomic://architecture/modules` is the same data
		// for a caller that wants to compute with it.
		std::string LayerTable() {
			std::ostringstream out;
			out << "# The layer stack\n\n"
				<< "A layer may see every layer below it and none above it. A same-layer edge is\n"
				<< "allowed only where the module names it in its `lateral` array, and there are\n"
				<< "three in this repository. A tier may link `shared` and its own tier.\n\n"
				<< "| Layer | Modules |\n|--:|---|\n";

			for (int height = 0; height <= Graph::Height(); height++) {
				const std::vector<std::string> names = Graph::AtLayer(height);
				if (names.empty()) {
					continue;
				}

				out << "| L" << height << " | ";
				for (size_t index = 0; index < names.size(); index++) {
					const GraphModule *row = Graph::Find(names[index]);
					out << (index == 0 ? "" : ", ") << "`" << names[index] << "`";
					if (row != nullptr && row->Tier != "shared") {
						out << " [" << row->Tier << "]";
					}
				}
				out << " |\n";
			}

			out << "\nThe program band links modules and nothing links it: ";
			const std::span<const GraphModule> programs = Graph::Programs();
			for (size_t index = 0; index < programs.size(); index++) {
				out << (index == 0 ? "" : ", ") << "`" << programs[index].Name << "`";
			}
			out << ".\n\nSource: `mono.tools/architecture/expected_graph.json`, checked by "
				   "`just test-architecture`.\n";

			return out.str();
		}

		// The graph, for a caller that wants to compute with it.
		std::string ModuleGraph() {
			json modules = json::array();
			for (const GraphModule &row : Graph::Modules()) {
				modules.push_back(
					json{
						{"name", row.Name},
						{"tier", row.Tier},
						{"layer", row.Layer},
						{"links", row.Links},
						{"lateral", row.Lateral},
						{"linkedBy", Graph::Dependents(row.Name)},
					}
				);
			}

			json programs = json::array();
			for (const GraphModule &row : Graph::Programs()) {
				programs.push_back(json{{"name", row.Name}, {"tier", row.Tier}, {"links", row.Links}});
			}

			return json{{"modules", std::move(modules)}, {"programs", std::move(programs)}}.dump(2);
		}

		// Every `AGENTS.md` in the checkout, as one resource each.
		//
		// Named by the directory holding it - `atomic://agents/engine/control`
		// for `mono.engine/control/AGENTS.md` - so a model that has been told to
		// read the one for the module it is about to touch can address it
		// without a directory listing.
		void AddAgentFiles(Surface &surface, const std::filesystem::path &root) {
			std::error_code walking;
			std::vector<std::filesystem::path> found;

			for (std::filesystem::recursive_directory_iterator step(root, walking), end; step != end;
				 step.increment(walking)) {
				if (walking) {
					break;
				}

				const std::filesystem::path &at = step->path();
				const std::string name = at.filename().string();

				// The vendored trees and the build cache hold thousands of
				// files and no invariants of ours.
				if (step->is_directory(walking) &&
					(name == "mono.vendor" || name == "node_modules" || name.rfind('.', 0) == 0)) {
					step.disable_recursion_pending();
					continue;
				}

				if (name == "AGENTS.md") {
					found.push_back(at);
				}
			}

			std::sort(found.begin(), found.end());

			for (const std::filesystem::path &path : found) {
				const std::filesystem::path directory = path.parent_path().lexically_relative(root);
				std::string address = directory.generic_string();
				if (address.empty() || address == ".") {
					address = "root";
				} else {
					// `mono.engine/control` reads better as `engine/control`,
					// and the `mono.` prefix is on every one of them.
					const std::string prefix = "mono.";
					if (address.rfind(prefix, 0) == 0) {
						address = address.substr(prefix.size());
					}
				}

				surface.AddResource(
					Resource{
						"atomic://agents/" + address,
						"AGENTS.md (" + address + ")",
						"The invariants specific to " + address +
							". AGENTS.md rule: read this before changing anything in it.",
						"text/markdown",
						[path](std::string &failure) { return ReadResourceFile(path, failure); },
					}
				);
			}
		}
	}

	void Surface::AddStandardResources() {
		AddResource(
			Resource{
				"atomic://architecture/layers",
				"The layer stack",
				"Every module by height, with the rule that decides which may include which. "
				"ReadResourceFile "
				"this before adding an include across modules.",
				"text/markdown",
				[](std::string &) { return LayerTable(); },
			}
		);

		AddResource(
			Resource{
				"atomic://architecture/modules",
				"The module graph",
				"Every module and program as JSON: tier, layer, what it links, what links it, and "
				"the sideways edges each is allowed. The same data the architecture check enforces.",
				"application/json",
				[](std::string &) { return ModuleGraph(); },
			}
		);

		AddResource(
			Resource{
				"atomic://components",
				"The component catalogue",
				"Every component type this engine registers, with its size, whether it is a tag, "
				"whether a save file can carry it and how many bytes a replication delta needs.",
				"application/json",
				// The same builder `engine_components` answers with, so the tool
				// and the resource cannot describe the registry differently.
				[](std::string &) { return ComponentCatalogue().dump(2); },
			}
		);

		const std::filesystem::path &root = RepositoryRoot();
		if (root.empty()) {
			return;
		}

		AddAgentFiles(*this, root);

		const std::filesystem::path bindings = root / "mono.engine" / "script" / "bindings";
		std::error_code missing;

		if (std::filesystem::exists(bindings / "manifest.json", missing)) {
			const std::filesystem::path path = bindings / "manifest.json";
			AddResource(
				Resource{
					"atomic://bindings/manifest",
					"The scripting manifest",
					"Every class, property, enum and datatype a script may name, generated from the "
					"class table by `just bindings`. Large - a couple of hundred kilobytes.",
					"application/json",
					[path](std::string &failure) { return ReadResourceFile(path, failure); },
				}
			);
		}

		if (std::filesystem::exists(bindings / "engine.d.luau", missing)) {
			const std::filesystem::path path = bindings / "engine.d.luau";
			AddResource(
				Resource{
					"atomic://bindings/luau",
					"The Luau declarations",
					"The type declarations every Luau script in this engine is checked against, and "
					"the file script_check uses.",
					"text/plain",
					[path](std::string &failure) { return ReadResourceFile(path, failure); },
				}
			);
		}
	}
}
