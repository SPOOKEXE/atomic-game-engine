// What a script may name, and whether one would compile.
//
// **Checking is offered and evaluating is not, and that is a decision rather
// than a gap.** The reason is this module's own rule and not a security
// posture - `SECURITY.md` already grants this surface enough authority to write
// a property and start a world, so "arbitrary code" is not the line. The line is
// the tick. Everything here runs on the thread that calls `Answer`, inside a
// frame, and `AGENTS.md` says a tool that needs to do slow work starts it and
// returns a handle rather than blocking. A type check is a pure function of text
// with a bounded cost, measured at 111 ms for one file against the whole
// declaration set on this tree. An evaluation is a Luau chunk with a `while
// true` in it, and there is no thread here to interrupt it from: the surface
// would have handed a client the ability to hang the program with four
// characters, and the program could not even log why.
//
// **So a model gets the two halves it can act on**: what classes and properties
// exist, live out of the class table this process actually registered, and
// whether a piece of source agrees with the generated declarations. Running it
// is what `world_run` and a `Script` instance are for, and those go through the
// scheduler that already owns a script's lifetime.
//
// The class table rather than the manifest, deliberately. `manifest.json` is
// generated from the same table and is served as a resource for a model that
// wants the whole of it; these tools answer out of the running process, so a
// program that registered a class the checked-in manifest has not caught up with
// still tells the truth about itself.

#include "Repository.hpp"

#include <engine/control/Surface.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/parallel/Capture.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <vector>

namespace engine::control {

	using ecs::Classes;
	using ecs::ClassId;
	using ecs::ClassInfo;
	using ecs::PropertyDescriptor;
	using ecs::PropertyType;
	using nlohmann::json;

	namespace {
		// How much source `script_check` will take.
		//
		// **A bound rather than a limit somebody will want raised.** The check
		// runs inside a frame, and its cost is roughly linear in what it is
		// given; a quarter of a megabyte of Luau is far larger than any script
		// in this repository and keeps the worst case in the tens of
		// milliseconds. Something bigger than this is a file, and a file is what
		// `path` is for.
		constexpr size_t LONGEST_SOURCE = 256 * 1024;

		json DescribeClass(ClassId id, bool withProperties) {
			const ClassInfo &info = Classes::Describe(id);

			json out{
				{"name", std::string(info.Name.Text())},
				{"properties", info.Properties.size()},
			};

			if (info.Parent.IsValid()) {
				out["parent"] = std::string(Classes::Describe(info.Parent).Name.Text());
			} else {
				out["parent"] = nullptr;
			}

			json ancestry = json::array();
			for (const ClassId ancestor : info.Ancestry) {
				ancestry.push_back(std::string(Classes::Describe(ancestor).Name.Text()));
			}
			out["ancestry"] = std::move(ancestry);

			if (!withProperties) {
				return out;
			}

			json properties = json::array();
			for (const PropertyDescriptor &property : info.Properties) {
				json described{
					{"name", std::string(property.Spelling)},
					// `ecs::Describe`, which is the name the properties panel and
					// both bindings already print. A second table here would be a
					// second vocabulary for one closed enum.
					{"type", ecs::Describe(property.Type)},
					{"writable", property.Writable},
					{"scriptable", property.Scriptable},
				};
				if (property.Type == PropertyType::Enum && property.EnumName.IsValid()) {
					described["enum"] = std::string(property.EnumName.Text());
				}
				properties.push_back(std::move(described));
			}
			out["properties"] = std::move(properties);

			return out;
		}
	}

	void Surface::AddScriptTools() {
		Add(Tool{
			"class_list",
			"Every class this process registers, with what it derives from. A class is what "
			"Instance.new names and what a save file carries; its properties are what a script may "
			"read and write. Inheritance is set inclusion here, so a query for a base class matches "
			"every derived instance. Call class_get for one class's properties.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"derivedFrom",
						  json{
							  {"type", "string"},
							  {"description", "Only classes that are a kind of this one."},
						  }},
					 }},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				ClassId base;
				if (arguments.contains("derivedFrom") && arguments["derivedFrom"].is_string()) {
					const std::string wanted = arguments["derivedFrom"].get<std::string>();
					base = Classes::Find(core::Name(wanted));
					if (!base.IsValid()) {
						failure = "no class is called '" + wanted + "'";
						return nullptr;
					}
				}

				json classes = json::array();
				for (uint32_t index = 0; index < static_cast<uint32_t>(Classes::Count()); index++) {
					const ClassId id(index);
					if (base.IsValid() && !Classes::IsA(id, base)) {
						continue;
					}
					classes.push_back(DescribeClass(id, false));
				}

				return json{{"classes", std::move(classes)}, {"count", classes.size()}};
			},
		});

		Add(Tool{
			"class_get",
			"One class: what it derives from, and every property it exposes with the type it "
			"carries, whether it can be written and whether a script can see it at all. This is the "
			"live class table rather than the checked-in manifest, so it describes what this "
			"process registered. Use instance_get to read the values off a particular instance.",
			[] {
				return json{
					{"type", "object"},
					{"properties", json{{"name", json{{"type", "string"}}}}},
					{"required", json::array({"name"})},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				if (!arguments.contains("name") || !arguments["name"].is_string()) {
					failure = "class_get needs `name`";
					return nullptr;
				}

				const std::string wanted = arguments["name"].get<std::string>();
				const ClassId id = Classes::Find(core::Name(wanted));
				if (!id.IsValid()) {
					failure = "no class is called '" + wanted + "'. Call class_list for the names.";
					return nullptr;
				}

				return DescribeClass(id, true);
			},
		});

		Add(Tool{
			"script_check",
			"Type-checks Luau against the engine's generated declarations, without running any of "
			"it. Give `source` for a draft or `path` for a file in the repository. Every diagnostic "
			"the editor's language server would show comes back here, because both go through the "
			"same declaration file. Nothing is evaluated: this surface will not run a script, "
			"because a tool runs inside the program's frame and there is nothing here that could "
			"interrupt a loop.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"source", json{{"type", "string"}, {"description", "Luau to check."}}},
						 {"path",
						  json{
							  {"type", "string"},
							  {"description", "A path relative to the repository root, instead of `source`."},
						  }},
					 }},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				const std::filesystem::path &root = RepositoryRoot();
				if (root.empty()) {
					failure = "script_check needs the checkout this program was built from, and this "
							  "executable is not inside one.";
					return nullptr;
				}

				const std::filesystem::path checker =
					BuildDirectory() / "tools" / core::Paths::Program("scriptcheck");
				std::error_code missing;
				if (!std::filesystem::exists(checker, missing)) {
					failure = "scriptcheck is not built. Run `just build scriptcheck` and try again.";
					return nullptr;
				}

				// **Written to a scratch file rather than piped**, because the
				// checker takes paths: it is the same program `just typecheck`
				// runs over the authored scripts, and giving it a second input
				// route for one caller is a second thing to keep correct.
				std::filesystem::path subject;
				bool temporary = false;

				if (arguments.contains("source") && arguments["source"].is_string()) {
					const std::string source = arguments["source"].get<std::string>();
					if (source.size() > LONGEST_SOURCE) {
						failure = "that is " + std::to_string(source.size()) +
								  " bytes of source and the limit is " + std::to_string(LONGEST_SOURCE) +
								  ". Write it to a file and pass `path`.";
						return nullptr;
					}

					// Named by the clock rather than by a fixed string, so two
					// programs with a surface open do not check into one another's
					// scratch file.
					subject = std::filesystem::temp_directory_path(missing) /
							  ("atomic-script-check-" + std::to_string(core::Clock::Nanoseconds()) + ".luau");
					std::ofstream out(subject, std::ios::binary | std::ios::trunc);
					if (!out) {
						failure = "could not write a scratch file at " + subject.string();
						return nullptr;
					}
					out << source;
					out.close();
					temporary = true;
				} else if (arguments.contains("path") && arguments["path"].is_string()) {
					// **Resolved under the repository and checked to still be
					// there**, so `../../etc/passwd` names a file outside the
					// checkout and is refused rather than handed to a type
					// checker that would read it.
					const std::filesystem::path asked = root / arguments["path"].get<std::string>();
					const std::filesystem::path resolved = std::filesystem::weakly_canonical(asked, missing);
					const std::filesystem::path base = std::filesystem::weakly_canonical(root, missing);
					if (resolved.string().rfind(base.string(), 0) != 0) {
						failure = "`path` is read from inside the repository, and that one is not.";
						return nullptr;
					}
					if (!std::filesystem::exists(resolved, missing)) {
						failure = "no such file: " + resolved.string();
						return nullptr;
					}
					subject = resolved;
				} else {
					failure = "script_check needs `source` or `path`";
					return nullptr;
				}

				const parallel::CaptureResult ran = parallel::Capture({
					checker.string(),
					subject.string(),
					"--definitions",
					(root / "mono.engine" / "script" / "bindings" / "engine.d.luau").string(),
				});

				if (temporary) {
					std::filesystem::remove(subject, missing);
				}

				if (!ran.Started) {
					failure = "could not run " + checker.string();
					return nullptr;
				}

				// **A script that does not type-check is a result, not a tool
				// error.** The diagnostics are the answer the caller asked for;
				// reporting them as a failure would make "your script is wrong"
				// look like "the checker is missing".
				return json{
					{"ok", ran.ExitCode == 0},
					{"diagnostics", ran.Output},
					{"checked", temporary ? std::string("source") : subject.string()},
				};
			},
		});
	}
}
