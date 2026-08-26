// The module graph, as three questions a model could not previously ask.
//
// **"May X link Y" is the one that matters.** Everything else here can be got at
// by reading `expected_graph.json`, if you know it exists and know how to read
// it; the verdict cannot, because it is the conclusion of two rules that live in
// two different CMake files and are enforced at two different times. A model
// that has to write an include in `render` has exactly this question and, until
// v0.19, exactly one way to answer it: add the edge and see whether the build
// refuses.
//
// **Registered separately from the universe tools**, because these are the only
// tools in this module that a program with no worlds can still answer honestly.
// A content origin holds no scenes and has no `world_list`; the layer stack is
// the same stack whatever the program is.
//
// Every one of these reads tables that were parsed once and never change, so
// none of them can hold the tick for measurable time - which is the constraint
// `AGENTS.md` puts on everything in this module.

#include <engine/control/Architecture.hpp>
#include <engine/control/Surface.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>

namespace engine::control {

	using nlohmann::json;

	namespace {
		// One row, as a client reads it.
		//
		// `linkedBy` is computed rather than stored: the expectation lists what
		// each module links and nothing lists what links it, and "who breaks if
		// I change this" is the question a model asks second.
		json Describe(const GraphModule &row) {
			json links = json::array();
			for (const std::string &link : row.Links) {
				links.push_back(link);
			}

			json dependents = json::array();
			for (const std::string &name : Graph::Dependents(row.Name)) {
				dependents.push_back(name);
			}

			json out{
				{"name", row.Name},
				{"tier", row.Tier},
				// Three kinds and not two. A row with no layer is the program
				// band whichever section it is in, which is what decides whether
				// anything may link it.
				{"kind",
				 row.Program				 ? "program"
				 : row.Layer == PROGRAM_BAND ? "program band"
											 : "module"},
				{"links", std::move(links)},
				{"linkedBy", std::move(dependents)},
			};

			if (row.Layer == PROGRAM_BAND) {
				out["layer"] = nullptr;
			} else {
				out["layer"] = row.Layer;
			}

			if (!row.Lateral.empty()) {
				json lateral = json::array();
				for (const std::string &name : row.Lateral) {
					lateral.push_back(name);
				}
				out["lateral"] = std::move(lateral);
			}

			if (!row.Requires.empty()) {
				out["requires"] = row.Requires;
			}

			return out;
		}
	}

	void Surface::AddArchitectureTools() {
		Add(Tool{
			"layer_table",
			"The engine's layer stack, bottom to top: every module at each height, with its tier. A "
			"layer may see every layer below it and none above it, so this table is what decides "
			"which module may include which. The programs are listed separately - they are what "
			"links modules and nothing links them. Call module_may_link to ask about one edge.",
			[] { return json{{"type", "object"}}; },
			[](const json &, std::string &) {
				json layers = json::array();
				for (int height = 0; height <= Graph::Height(); height++) {
					const std::vector<std::string> names = Graph::AtLayer(height);
					if (names.empty()) {
						continue;
					}

					json members = json::array();
					for (const std::string &name : names) {
						const GraphModule *row = Graph::Find(name);
						members.push_back(json{{"name", name}, {"tier", row == nullptr ? "" : row->Tier}});
					}
					layers.push_back(json{{"layer", height}, {"modules", std::move(members)}});
				}

				json programs = json::array();
				for (const GraphModule &row : Graph::Programs()) {
					programs.push_back(json{{"name", row.Name}, {"tier", row.Tier}});
				}

				return json{
					{"layers", std::move(layers)},
					{"programs", std::move(programs)},
					{"rule",
					 "A layer may see every layer below it and none above it. A same-layer edge is "
					 "allowed only where the module names it in its `lateral` array. A tier may link "
					 "shared and its own tier and nothing else."},
					{"source",
					 "mono.tools/architecture/expected_graph.json, checked by just test-architecture"},
				};
			},
		});

		Add(Tool{
			"module_get",
			"One module or program: which layer and tier it is, every first-party target it links, "
			"and everything that links it. `linkedBy` is what would have to be rebuilt and reviewed "
			"if this module's interface changed. With no name, every module in layer order.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"name",
						  json{
							  {"type", "string"},
							  {"description", "The module or program. Omit for all of them."},
						  }},
					 }},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				if (arguments.contains("name") && arguments["name"].is_string()) {
					const std::string wanted = arguments["name"].get<std::string>();
					const GraphModule *row = Graph::Find(wanted);
					if (row == nullptr) {
						failure = "no module or program is called '" + wanted +
								  "'. Call layer_table for the names.";
						return nullptr;
					}
					return Describe(*row);
				}

				json modules = json::array();
				for (const GraphModule &row : Graph::Modules()) {
					modules.push_back(Describe(row));
				}

				json programs = json::array();
				for (const GraphModule &row : Graph::Programs()) {
					programs.push_back(Describe(row));
				}

				return json{{"modules", std::move(modules)}, {"programs", std::move(programs)}};
			},
		});

		Add(Tool{
			"module_may_link",
			"Whether one module is allowed to link another, and why. This is the question the "
			"architecture check answers, decided here from the same two rules: a tier may link "
			"shared and its own tier, and a layer may see every layer below it and none above it. "
			"Ask before writing an include - an edge that runs upward is refused at configure time "
			"and the fix is almost never to widen the rule.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"from", json{{"type", "string"}, {"description", "The module doing the linking."}}},
						 {"to", json{{"type", "string"}, {"description", "What it would link."}}},
					 }},
					{"required", json::array({"from", "to"})},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				if (!arguments.contains("from") || !arguments.contains("to")) {
					failure = "module_may_link needs `from` and `to`";
					return nullptr;
				}

				const std::string from = arguments["from"].get<std::string>();
				const std::string to = arguments["to"].get<std::string>();
				const LinkVerdict verdict = Graph::MayLink(from, to);

				// **Allowed or not, this is a result rather than an error.** A
				// refused edge is the answer the caller asked for; reporting it
				// as a tool failure would make "no" indistinguishable from "that
				// module does not exist", and only the second is a mistake.
				const GraphModule *source = Graph::Find(from);
				const bool already =
					source != nullptr &&
					std::find(source->Links.begin(), source->Links.end(), to) != source->Links.end();

				return json{
					{"from", from},
					{"to", to},
					{"allowed", verdict.Allowed},
					{"existing", already},
					{"reason", verdict.Reason},
				};
			},
		});
	}
}
