#pragma once

#include <iosfwd>

namespace pxcimport {
	// Converts one bounded PXCX file to a native authored graph.
	// Returns zero on a completed import, two for argument errors, or one for I/O and import errors.
	int Run(int argc, char **argv, std::ostream &output, std::ostream &errors);
}
