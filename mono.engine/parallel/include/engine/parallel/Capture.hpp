#pragma once

// Running a program to the end and keeping what it printed.
//
// **`Process.hpp` is next door and does not do this on purpose.** A supervised
// host inherits the supervisor's streams, because a crash message that went
// somewhere nobody reads is a crash nobody explains. This is the opposite case:
// the program is being run *for* its output, it is expected to be brief, and
// letting it print into the parent's terminal would be noise. Two shapes, and
// folding them together would mean a supervisor's child could silently have its
// log swallowed by a flag somebody set for a different reason.
//
// The caller this was added for is `mono.launcher`, which asks every program it
// can start what options it accepts - `core::Arguments::Describe` - so that a
// launcher screen is generated from the truth rather than being a second, stale
// copy of a flag table.
//
// **Not popen: that goes through a shell**, so every argument would have to be
// quoted for whichever shell is on the other side, and the rules differ between
// `sh` and `cmd.exe`. A path containing a space is normal on Windows, so the
// shell route is wrong on the platform where it matters most.
//
// **`mono.tools/testrunner` has its own copy of this and keeps it.** That tool
// links `Engine::core` and nothing else, and giving it a whole L2 module - job
// pool, channels, sockets - to share sixty lines of fork and exec would be the
// worse trade. What rule 2 forbids is two copies of a *fact* that can drift
// apart; two spellings of the same syscall sequence cannot disagree about the
// state of anything.
//
// Blocking, and deliberately so. Every caller so far is asking a question
// during startup and has nothing to do until it is answered.
//
// @tier L2 · shared
// @since v0.18

#include <string>
#include <vector>

namespace engine::parallel {

	// What a finished program left behind.
	//
	// Read `Started` first. The other two describe a program that ran, and say
	// nothing at all about one that never did.
	struct CaptureResult {
		// False when the program could not be started at all - missing, not
		// executable, spawn refused. Distinct from a non-zero exit, which is
		// the program running and disagreeing with you.
		bool Started = false;

		// The program's own exit status, and meaningless unless `Started`.
		int ExitCode = -1;

		// stdout and stderr together, in the order the program wrote them.
		//
		// Merged rather than kept apart because the point of capturing it is to
		// show somebody what happened, and a failure whose error line has been
		// sorted away from the output it followed is harder to read, not easier.
		std::string Output;
	};

	// Runs `arguments[0]` with the rest as its argv, and blocks until it exits.
	//
	// Arguments reach the operating system as a list. Nothing is quoted,
	// escaped or word-split, because nothing is ever handed to a shell.
	//
	// @param arguments The program and its arguments. An empty list runs
	//                  nothing and reports `Started` false.
	// @return What it printed and how it ended.
	CaptureResult Capture(const std::vector<std::string> &arguments);
}
