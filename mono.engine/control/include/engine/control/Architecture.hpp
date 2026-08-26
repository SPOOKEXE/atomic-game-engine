#pragma once

// The module graph: what links what, how high it sits, and whether an edge
// would be legal.
//
// **The same file `just test-architecture` checks the build against.**
// `mono.tools/architecture/expected_graph.json` carries every module, its tier,
// its layer, its exact link set and the sideways edges it is allowed, and
// `CheckTargetGraph.cmake` refuses a build that disagrees with it. This module
// compiles a copy of that file in and answers questions from it, so the answer
// a model gets is the same one the check enforces rather than a second
// description that agrees until somebody edits one of them.
//
// **`MayLink` is the question that had no asker.** A model writing code in this
// engine has to know whether `render` may include `script` before it writes the
// include, and until v0.19 the only way to find out was to add the edge and see
// whether the build refused it. That answer takes a configure; this one takes a
// call, and it is derived from the same two rules - the tier table in
// `mono.build/MonoLibrary.cmake` and the layer rule in
// `CheckTargetGraph.cmake`.
//
// Read-only and process-wide. The graph is a constant of the build.
//
// @tier shared

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::control {

	// The layer of something that links modules and that no module links.
	//
	// The programs, the tools, and the test harness. `expected_graph.json`
	// spells this as a row with no `layer` at all; a sentinel is what that
	// becomes once it is a number, and it is negative so that "below
	// everything" is never accidentally true of it.
	//
	// @since v0.19
	inline constexpr int PROGRAM_BAND = -1;

	// One row of the target graph.
	//
	// @since v0.19
	struct GraphModule {
		// The module's name, as CMake and the expectation both spell it.
		std::string Name;

		// `shared`, `client` or `server`. What a target of this tier may link
		// is the table in `mono.build/MonoLibrary.cmake`.
		std::string Tier;

		// How high it sits, or `PROGRAM_BAND`.
		int Layer = PROGRAM_BAND;

		// The CMake option that has to be on for this row to be in the build,
		// or empty when it is unconditional.
		std::string Requires;

		// Every first-party target it links, sorted. The closure rather than
		// the declaration, which is what the check compares.
		std::vector<std::string> Links;

		// The same-layer edges this module is allowed, each named deliberately.
		// Three exist in this repository and adding a fourth is a diff.
		std::vector<std::string> Lateral;

		// Whether this is a program rather than a module.
		bool Program = false;
	};

	// Whether an edge is allowed, and the sentence that says why.
	//
	// **The reason is filled in either way.** A caller told only "no" has to
	// guess which of two rules refused it, and the two have different fixes: a
	// tier violation is answered with `ALLOW_TIER_ESCAPE` or by moving the code,
	// and a layer violation almost never is.
	//
	// @since v0.19
	struct LinkVerdict {
		// Whether the build would accept the edge.
		bool Allowed = false;

		// Why, in the words the failing check would use.
		std::string Reason;
	};

	// The checked-in target graph, read once.
	//
	// @since v0.19
	class Graph {
	  public:
		// Every layered module, in layer order and then by name.
		//
		// @return The modules. Valid for the life of the process.
		static std::span<const GraphModule> Modules();

		// Every program, tool and harness, by name.
		//
		// @return The programs. Valid for the life of the process.
		static std::span<const GraphModule> Programs();

		// One row by name, module or program.
		//
		// @param name What to look for.
		// @return The row, or null when nothing is called that.
		static const GraphModule *Find(std::string_view name);

		// Everything that links `name`, directly.
		//
		// The other half of `GraphModule::Links`, and the half that is not in
		// the file: an expectation lists what each row links and nothing lists
		// what links it, so this is the reverse index that answers "who breaks
		// if I change this".
		//
		// @param name The module being linked.
		// @return The names, sorted, modules and programs together.
		static std::vector<std::string> Dependents(std::string_view name);

		// Whether `from` may link `to`, and why.
		//
		// Both rules, in the order the build applies them: the tier table
		// refuses first, then the layer rule. An edge that already exists is
		// reported as allowed and says so.
		//
		// @param from The module that would do the linking.
		// @param to   What it would link.
		// @return The verdict.
		static LinkVerdict MayLink(std::string_view from, std::string_view to);

		// The names sitting at one height.
		//
		// @param layer The height.
		// @return The names, sorted. Empty for a height nothing sits at.
		static std::vector<std::string> AtLayer(int layer);

		// The highest layer any module sits at.
		//
		// @return The height, or zero when nothing is layered.
		static int Height();
	};
}
