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

	// What a finished program left behind.
	//
	// Read Started first. The other two describe a program that ran, and say
	// nothing at all about one that never did.
	struct ProcessResult {
		// False when the program could not be started at all - missing,
		// not executable, fork failed. Distinct from a non-zero exit, which is
		// the program running and disagreeing with you.
		bool Started = false;

		// The program's own exit status, and meaningless unless Started.
		int ExitCode = -1;

		// stdout and stderr together, in the order the program wrote them.
		//
		// Merged rather than kept apart because the point of capturing it is to
		// show somebody what happened, and a failure whose error line has been
		// sorted away from the output it followed is harder to read, not easier.
		std::string Output;
	};

	// Runs `arguments[0]` with the rest as its argv, with stdout and stderr
	// merged into Output. Blocks until it exits.
	//
	// Arguments are passed to the OS as a list. Nothing is quoted, escaped or
	// word-split, because nothing is ever handed to a shell.
	ProcessResult Run(const std::vector<std::string> &arguments);
}
