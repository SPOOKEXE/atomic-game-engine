#pragma once

// Executes one bounded, static imagegraph render from command-line arguments.

#include <iosfwd>

namespace imagegraph_runner {

	// Parses, compiles, evaluates and writes one selected output PNG.
	//
	// @return Zero on success, two for argument errors, or one for graph, execution or file errors.
	int Run(int argc, char **argv, std::ostream &output, std::ostream &errors);
}
