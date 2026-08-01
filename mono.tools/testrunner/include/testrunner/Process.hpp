#pragma once

// Running another program and capturing what it said.
//
// Not popen: that goes through a shell, so every argument has to be quoted for
// whichever shell is on the other side, and the quoting rules differ between
// `sh` and `cmd.exe`. A path containing a space is normal on Windows, so the
// shell route is wrong on the platform where it matters most.
//
// The implementations live in src/platform/, and the header names no operating
// system.

#include <string>
#include <vector>

namespace testrunner {

	struct ProcessResult {
		// False when the program could not be started at all — missing,
		// not executable, fork failed. Distinct from a non-zero exit, which is
		// the program running and disagreeing with you.
		bool Started = false;
		int ExitCode = -1;
		std::string Output;
	};

	// Runs `arguments[0]` with the rest as its argv, with stdout and stderr
	// merged into Output. Blocks until it exits.
	//
	// Arguments are passed to the OS as a list. Nothing is quoted, escaped or
	// word-split, because nothing is ever handed to a shell.
	ProcessResult Run(const std::vector<std::string> &arguments);
}
