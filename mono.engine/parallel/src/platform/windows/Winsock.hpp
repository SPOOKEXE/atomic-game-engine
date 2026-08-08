#pragma once

// What a channel needs on Windows that it needs nowhere else: a library started
// before the first call, and a way to get a socket into a child at all.
//
// **A socket does not survive being inherited.** The handle arrives in the
// child — `GetHandleInformation` finds it — but Winsock does not know it, and
// every call on it fails with `WSAENOTSOCK`. That is not a bug to work around;
// it is what `WSADuplicateSocket` exists for. The parent asks Winsock to mint a
// description of the socket *for a named child process*, and the child turns
// that description back into a socket of its own.
//
// So the handover is in two halves, in two processes, and neither can see the
// other's code — the same problem `posix/InheritedSlot.hpp` has and the reason
// both halves are declared here rather than each in the file that runs it:
//
//   1. The parent makes a pipe, gives the child its end, and starts it.
//   2. The child blocks reading that pipe.
//   3. The parent, now knowing the child's process id, writes the description.
//   4. The child builds its socket and closes the pipe.
//
// A pipe rather than the command line or a file, because the description is
// several hundred opaque bytes and is only meaningful to one process. A pipe
// handle *does* inherit, being an ordinary kernel handle, which is what makes
// it the one thing that can carry the rest.

namespace engine::parallel::platform {

	// Starts Winsock once per process, and never stops it.
	//
	// Every entry point that might be the first socket call in the process
	// calls this. It is idempotent and cheap after the first time.
	//
	// **No matching cleanup, deliberately.** `WSACleanup` is refcounted against
	// `WSAStartup`, and the only correct moment to call it is after the last
	// socket in the process is closed — which is a fact no single translation
	// unit knows. Process teardown releases it, so the alternative to leaking
	// it is a shutdown ordering problem in exchange for nothing.
	void EnsureWinsock();

	// The environment variable a supervised child finds its handover pipe in.
	//
	// **Windows has no descriptor slot to place a handle in.** POSIX puts the
	// channel at a fixed number because descriptor numbers mean the same thing
	// in the child; an inherited Windows handle keeps its value but has no
	// reserved place, so the value itself has to travel. It goes in the
	// environment rather than on the command line for the same reason POSIX
	// uses a fixed slot: a child has exactly one channel to whoever started it,
	// and the argument list should not carry something only two layers
	// understand.
	//
	// The name is fixed, so nothing has to be agreed; the value differs per
	// child, which is the part POSIX gets for free.
	inline constexpr wchar_t INHERITED_VARIABLE[] = L"MONO_INHERITED_CHANNEL";

	// Makes the pipe the socket description crosses on.
	//
	// @param childEnd  Set to the inheritable end, which is named in the
	//                  child's environment and in the spawn's handle list. The
	//                  caller closes its copy once the child has started.
	// @param parentEnd Set to the end kept here, which the description is
	//                  written to. Never inheritable: a child holding a copy
	//                  would keep the pipe from ever reporting end-of-file.
	// @return `false` when the pipe could not be made, in which case neither
	//         handle is set.
	bool MakeChannelHandover(void *&childEnd, void *&parentEnd);

	// Hands a started child one end of a channel.
	//
	// Called after the spawn, because the description is minted for a specific
	// process and there is no process id until then.
	//
	// @param socket    The end the child is to have. Still owned by the caller,
	//                  which closes it afterwards — the child's copy is a
	//                  reference of its own and keeps the socket alive.
	// @param processId The child's process id.
	// @param parentEnd The writing end from `MakeChannelHandover`.
	// @return `false` when the description could not be made or delivered,
	//         which for a child already blocked on the pipe means it will see
	//         end-of-file and start without a channel.
	bool SendChannelToChild(long long socket, unsigned long processId, void *parentEnd);
}
