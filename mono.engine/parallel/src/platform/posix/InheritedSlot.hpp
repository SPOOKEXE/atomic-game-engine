#pragma once

// The descriptor a supervised child finds its channel on.
//
// Two translation units have to agree on this number and neither can see the
// other's copy of it: the spawn places the handle there, and the child, which
// is a different program run, picks it up. A number kept true by a comment is a
// number that is eventually not true.

namespace engine::parallel {

	// Where a child started with an endpoint finds it.
	//
	// Three: after standard input, output and error, which the child inherits
	// so that a host's log lands where its supervisor's does.
	constexpr int INHERITED = 3;
}
