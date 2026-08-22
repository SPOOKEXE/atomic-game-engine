// The workflows this repository already has, offered where a client can invoke
// them.
//
// **Served from `.claude/commands/*.md` rather than copied into C++.** Those
// files are the procedures - the completion checklist, the module scaffold, the
// review pass - and a second copy here would be a second thing to keep in step
// with a document whose whole point is being the one that is right. So a fifth
// command file becomes a fifth prompt with no code change, and a prompt is never
// out of date with the file it renders.
//
// **One prompt is written here and has no file**: the architecture-review pass,
// which drives the module-graph tools this version added. It has no file because
// it is not a thing a person does at a terminal - it is a thing a model does
// through this surface, using `layer_table`, `module_get` and `module_may_link`,
// and the instructions are about those calls.
//
// The file-backed ones appear only inside a checkout, which is the tool table's
// own rule: a client is told what this program can actually do.

#include "Repository.hpp"

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
		std::string ReadPromptFile(const std::filesystem::path &path, std::string &failure) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				failure = "could not read " + path.string();
				return {};
			}
			std::ostringstream contents;
			contents << file.rdbuf();
			return contents.str();
		}

		// The `description:` line out of a command file's front matter.
		//
		// **ReadPromptFile rather than guessed**, because it is the one sentence the file
		// already writes for exactly this purpose - a client listing prompts
		// shows it, and a command palette shows the same string. Missing front
		// matter is not an error: the prompt is still offered, described by its
		// own name.
		std::string DescriptionOf(const std::string &document, const std::string &name) {
			if (document.rfind("---", 0) != 0) {
				return "The " + name + " workflow, from .claude/commands/" + name + ".md";
			}

			std::istringstream lines(document);
			std::string line;
			std::getline(lines, line);

			while (std::getline(lines, line) && line.rfind("---", 0) != 0) {
				const std::string key = "description:";
				if (line.rfind(key, 0) != 0) {
					continue;
				}
				std::string value = line.substr(key.size());
				const size_t first = value.find_first_not_of(" \t");
				return first == std::string::npos ? std::string() : value.substr(first);
			}

			return "The " + name + " workflow, from .claude/commands/" + name + ".md";
		}

		// The document without its front matter, which is metadata for the
		// client rather than instructions for the model.
		std::string BodyOf(const std::string &document) {
			if (document.rfind("---", 0) != 0) {
				return document;
			}

			const size_t close = document.find("\n---", 3);
			if (close == std::string::npos) {
				return document;
			}

			const size_t after = document.find('\n', close + 1);
			return after == std::string::npos ? std::string() : document.substr(after + 1);
		}
	}

	void Surface::AddStandardPrompts() {
		AddPrompt(
			Prompt{
				"architecture-review",
				"Review a change against this engine's layer stack, using the module graph tools on "
				"this surface.",
				{PromptArgument{
					"module",
					"The module under review. Omit to start from the whole stack.",
					false,
				}},
				[](const json &arguments, std::string &) {
					const std::string area = arguments.value("module", std::string());

					std::ostringstream out;
					out << "Review this engine's architecture through the control surface you are "
						   "connected to, rather than by reading CMake files.\n\n";

					if (area.empty()) {
						out << "Start with `layer_table`. It is the whole stack, bottom to top, and the "
							   "rule that decides which module may include which.\n\n";
					} else {
						out << "The area under review is `" << area
							<< "`. Start with `module_get` on it, then `layer_table` for the stack "
							   "around it.\n\n";
					}

					out << "Then, for each module you are looking at:\n\n"
						   "1. `module_get` - what it links, and what links it. The second list is what "
						   "would have to be rebuilt and re-reviewed if its interface changed. A module "
						   "nothing links is either a program, a tool, or dead.\n"
						   "2. ReadPromptFile its `AGENTS.md` from `atomic://agents/<path>`. It carries the "
						   "invariants that catch real mistakes in that module, and a change that "
						   "contradicts one is a change that needs the file updated in the same commit.\n"
						   "3. For every edge you are considering adding, ask `module_may_link` before "
						   "you write the include. An edge that runs upward is refused at configure "
						   "time and the fix is almost never to widen the rule.\n\n"
						   "Report: which edges you checked, which the graph refused and why, and any "
						   "module whose `AGENTS.md` no longer describes what it does. Name modules, not "
						   "generalities.";

					return out.str();
				},
			}
		);

		const std::filesystem::path &root = RepositoryRoot();
		if (root.empty()) {
			return;
		}

		const std::filesystem::path commands = root / ".claude" / "commands";
		std::error_code walking;
		if (!std::filesystem::is_directory(commands, walking)) {
			return;
		}

		std::vector<std::filesystem::path> found;
		for (const std::filesystem::directory_entry &entry :
			 std::filesystem::directory_iterator(commands, walking)) {
			if (entry.path().extension() == ".md") {
				found.push_back(entry.path());
			}
		}
		std::sort(found.begin(), found.end());

		for (const std::filesystem::path &path : found) {
			const std::string name = path.stem().string();

			// ReadPromptFile now for the description and again on demand for the body.
			// A file that has changed since the surface was built serves its new
			// text, which is what a checked-in procedure being the authority
			// means.
			std::string reading;
			const std::string document = ReadPromptFile(path, reading);
			if (!reading.empty()) {
				continue;
			}

			AddPrompt(
				Prompt{
					name,
					DescriptionOf(document, name),
					// **No arguments.** A command file is prose somebody follows
					// against whatever they are working on; it has no parameters
					// to declare, and inventing some here would put a form in
					// front of a document that does not read one.
					{},
					[path](const json &, std::string &failure) {
						const std::string current = ReadPromptFile(path, failure);
						return failure.empty() ? BodyOf(current) : std::string();
					},
				}
			);
		}
	}
}
