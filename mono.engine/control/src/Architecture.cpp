// The target graph, parsed once out of the copy compiled in.
//
// **Parsed lazily and kept**, because a program that never opens its control
// port must not pay to parse twenty-five kilobytes of JSON. The first call
// builds the tables; every later one reads them. Nothing here is mutable
// afterwards, so no lock is needed even though `Surface::Answer` runs on the
// program's own thread and a test may call it from another.
//
// **The two rules are reimplemented here rather than shelled out to**, which is
// worth being uneasy about: `mono.build/MonoLibrary.cmake` owns the tier table
// and `mono.tools/architecture/CheckTargetGraph.cmake` owns the layer rule, and
// this file says the same two things a third time. What stops it drifting is
// that both rules are three lines long and both are covered by fixtures that
// must fail with a named message - and that the *data* is not copied at all,
// which is the half that actually changes. A rule spelled twice and checked
// both times is a different risk from a graph spelled twice and checked once.

#include <engine/control/Architecture.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace engine::control {

	using nlohmann::json;

	// Written by `cmake/GraphData.cpp.in` at configure time.
	extern const char *const GRAPH_JSON;

	namespace {
		// Which tiers a target of each tier may link, from
		// `mono.build/MonoLibrary.cmake`'s `MONO_TIER_ALLOWS_*`.
		bool TierAllows(const std::string &tier, const std::string &linked) {
			if (linked == "shared") {
				return true;
			}
			return tier == linked;
		}

		std::vector<std::string> Strings(const json &node, const char *member) {
			std::vector<std::string> out;
			if (!node.contains(member) || !node[member].is_array()) {
				return out;
			}
			for (const json &entry : node[member]) {
				if (entry.is_string()) {
					out.push_back(entry.get<std::string>());
				}
			}
			return out;
		}

		// Every row, modules first and then programs, plus the index into them.
		//
		// One vector rather than two, so that a `GraphModule *` handed out by
		// `Find` stays valid: two vectors would mean two reallocation histories
		// and `Modules()` would have to be a copy.
		struct Tables {
			std::vector<GraphModule> Rows;
			size_t ProgramsBegin = 0;
			std::unordered_map<std::string, size_t> ByName;
			int Highest = 0;
		};

		void ReadSection(const json &document, const char *section, bool programs, Tables &into) {
			if (!document.contains(section) || !document[section].is_object()) {
				return;
			}

			for (const auto &[name, entry] : document[section].items()) {
				// `_comment` carries the prose, exactly as the CMake check reads
				// it: anything starting with an underscore is documentation.
				if (name.empty() || name.front() == '_' || !entry.is_object()) {
					continue;
				}

				GraphModule row;
				row.Name = name;
				row.Tier = entry.value("tier", std::string());
				row.Layer = entry.value("layer", PROGRAM_BAND);
				row.Requires = entry.value("requires", std::string());
				row.Links = Strings(entry, "links");
				row.Lateral = Strings(entry, "lateral");
				row.Program = programs;
				into.Highest = std::max(into.Highest, row.Layer);
				into.Rows.push_back(std::move(row));
			}
		}

		const Tables &Load() {
			static const Tables tables = [] {
				Tables built;

				json document = json::parse(GRAPH_JSON, nullptr, false);
				if (document.is_discarded()) {
					// The file is generated from a checked-in JSON document that
					// `just test-architecture` also parses, so this is
					// unreachable short of a build that mangled the embedding.
					// Empty tables answer every question with "nothing is called
					// that", which is honest.
					return built;
				}

				ReadSection(document, "modules", false, built);

				// Layer order, then by name. A model reading `Modules()` end to
				// end is reading the stack bottom upward, which is the order
				// `docs/CODE_ARCH.md` prints it in and the order that makes the
				// dependency rule obvious.
				std::sort(
					built.Rows.begin(), built.Rows.end(), [](const GraphModule &a, const GraphModule &b) {
						return a.Layer != b.Layer ? a.Layer < b.Layer : a.Name < b.Name;
					}
				);

				built.ProgramsBegin = built.Rows.size();
				ReadSection(document, "programs", true, built);
				std::sort(
					built.Rows.begin() + static_cast<long>(built.ProgramsBegin),
					built.Rows.end(),
					[](const GraphModule &a, const GraphModule &b) { return a.Name < b.Name; }
				);

				for (size_t index = 0; index < built.Rows.size(); index++) {
					built.ByName.emplace(built.Rows[index].Name, index);
				}

				return built;
			}();

			return tables;
		}
	}

	std::span<const GraphModule> Graph::Modules() {
		const Tables &tables = Load();
		return std::span<const GraphModule>(tables.Rows.data(), tables.ProgramsBegin);
	}

	std::span<const GraphModule> Graph::Programs() {
		const Tables &tables = Load();
		return std::span<const GraphModule>(
			tables.Rows.data() + tables.ProgramsBegin, tables.Rows.size() - tables.ProgramsBegin
		);
	}

	const GraphModule *Graph::Find(std::string_view name) {
		const Tables &tables = Load();
		const auto found = tables.ByName.find(std::string(name));
		return found == tables.ByName.end() ? nullptr : &tables.Rows[found->second];
	}

	std::vector<std::string> Graph::Dependents(std::string_view name) {
		const Tables &tables = Load();
		const std::string wanted(name);

		std::vector<std::string> out;
		for (const GraphModule &row : tables.Rows) {
			if (std::find(row.Links.begin(), row.Links.end(), wanted) != row.Links.end()) {
				out.push_back(row.Name);
			}
		}

		// **Deduplicated, because two rows can share a name.** A program's own
		// library is a row under `modules` and the executable is a row under
		// `programs`, both called `server`, and both link `control` - so the
		// answer to "what links control" listed it twice. Same for `Find`, which
		// keeps the first and therefore answers with the library: that is the
		// row with a tier and a link set worth reading.
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	LinkVerdict Graph::MayLink(std::string_view from, std::string_view to) {
		const GraphModule *source = Find(from);
		if (source == nullptr) {
			return {false, "no module or program is called '" + std::string(from) + "'"};
		}

		const GraphModule *target = Find(to);
		if (target == nullptr) {
			return {false, "no module or program is called '" + std::string(to) + "'"};
		}

		if (source == target) {
			return {false, "'" + source->Name + "' cannot link itself"};
		}

		const bool already =
			std::find(source->Links.begin(), source->Links.end(), target->Name) != source->Links.end();

		// The tier table refuses first, because that is the check that runs
		// first: `mono_check_all_tiers` fails at configure time and the layer
		// rule is not reached at all on a tree that does not configure.
		//
		// **An edge that already exists passes it, because the build already
		// accepted that edge.** The tier rule is the one with a per-edge escape,
		// `ALLOW_TIER_ESCAPE`, and the escape is declared in a `CMakeLists.txt`
		// rather than in the expectation - so the only evidence of one here is
		// the edge being in the checked-in graph at all. `studio` links `server`
		// exactly this way. The layer rule gets no such excuse: its escape is
		// the `lateral` array, which *is* in the data, so an upward edge is
		// refused whether it exists or not - and one that existed would be a
		// graph `just test-architecture` rejects.
		if (!TierAllows(source->Tier, target->Tier)) {
			if (already) {
				return {
					true,
					"'" + source->Name + "' is [" + source->Tier + "] and '" + target->Name + "' is [" +
						target->Tier +
						"], and it links it anyway - so the edge is named in ALLOW_TIER_ESCAPE with a "
						"reason beside it. Read that reason before adding a second one."
				};
			}

			return {
				false,
				"'" + source->Name + "' is [" + source->Tier + "] and '" + target->Name + "' is [" +
					target->Tier + "]. A [" + source->Tier +
					"] target may only link shared and its own tier. If the edge is deliberate it is named "
					"in ALLOW_TIER_ESCAPE with a reason beside it, which is a diff a reviewer sees."
			};
		}

		// **The program band is decided by the absence of a layer, not by which
		// section a row is in.** `expected_graph.json` keeps the tools and each
		// program's own library under `modules` and only the executables under
		// `programs`, and neither kind carries a `layer`.
		//
		// **The source is tested first**, which is the order the CMake check
		// walks in: it skips a row with no layer entirely and reports the band
		// from the other side, so a band row linking another band row - the test
		// harness linking the client and the server libraries - is never
		// refused. Only a *layered* module linking the band is.
		if (source->Layer == PROGRAM_BAND) {
			// Nothing sits above the band, so the layer rule has nothing to say.
			return {
				true,
				already ? "'" + source->Name + "' already links '" + target->Name + "'"
						: "'" + source->Name +
							  "' is in the program band and may link anything of a tier it allows"
			};
		}

		if (target->Layer == PROGRAM_BAND) {
			return {
				false,
				"'" + target->Name +
					"' has no layer, so it is the program band. A module may not link the program band."
			};
		}

		const std::string heights = "[L" + std::to_string(source->Layer) + "] and '" + target->Name +
									"' is [L" + std::to_string(target->Layer) + "]";

		if (target->Layer > source->Layer) {
			return {
				false,
				"'" + source->Name + "' is " + heights +
					". A layer may see every layer below it and none above it."
			};
		}

		if (target->Layer == source->Layer) {
			const bool named = std::find(source->Lateral.begin(), source->Lateral.end(), target->Name) !=
							   source->Lateral.end();
			if (!named) {
				return {
					false,
					"'" + source->Name + "' and '" + target->Name + "' are both [L" +
						std::to_string(source->Layer) +
						"]. Siblings do not include each other. If the edge is deliberate, name it in this "
						"module's `lateral` array and say why beside the edge."
				};
			}
			return {
				true,
				"'" + source->Name + "' names '" + target->Name +
					"' in its `lateral` array, which is how a sideways edge is allowed"
			};
		}

		return {
			true,
			already ? "'" + source->Name + "' already links '" + target->Name + "'"
					: "'" + source->Name + "' is " + heights + ", which runs downward"
		};
	}

	std::vector<std::string> Graph::AtLayer(int layer) {
		std::vector<std::string> out;
		for (const GraphModule &row : Modules()) {
			if (row.Layer == layer) {
				out.push_back(row.Name);
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	int Graph::Height() {
		return Load().Highest;
	}
}
