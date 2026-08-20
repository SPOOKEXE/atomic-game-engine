#pragma once

// The two things this module needs from an operating system that are not a
// socket. Private: nothing outside `mono.discord` has a use for either.
//
// @since v0.17

namespace discord {

	// This process's id, which Discord wants in every `SET_ACTIVITY`.
	//
	// It uses it to clear the presence when the process exits without closing
	// the socket, so a wrong one leaves somebody's profile stuck on a game they
	// have quit.
	//
	// @return The id.
	int ProcessId();
}
