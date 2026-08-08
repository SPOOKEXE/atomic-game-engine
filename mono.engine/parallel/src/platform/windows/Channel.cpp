// Making the endpoints that cross a process boundary, on the platform with no
// `socketpair`.
//
// **A connected pair has to be built by hand here, and the build has a hole in
// it.** Winsock offers no `socketpair`, so the only way to two connected stream
// sockets is to listen on loopback, connect to that, and accept — and between
// the listen and the accept, any process on the machine can reach that port.
// Accepting the wrong connection would hand a stranger one end of a channel the
// engine trusts.
//
// So the accepted connection is checked against the one that was made, by
// address and port, and anything else is dropped and the accept retried. That
// closes the hole: the impostor would have to be bound to the same loopback
// port as our own connecting socket, which the kernel does not allow while that
// socket holds it.
//
// What the handle then behaves like is `SocketChannel`'s job and owning one is
// `ProcessChannel.cpp`'s; this file only creates the pair, hands one end to a
// caller who is about to spawn a child, and picks the other up again in that
// child.

#include "../../SocketChannel.hpp"
#include "../Socket.hpp"
#include "Winsock.hpp"

#include <engine/core/Log.hpp>
#include <engine/parallel/ProcessChannel.hpp>

// WIN32_LEAN_AND_MEAN before anything, and it is load-bearing rather than a
// compile-time saving: without it <windows.h> drags in the original <winsock.h>,
// which redefines everything <winsock2.h> declares. With it, the two can be
// included in either order — which they have to be, because `.clang-format`
// regroups and sorts system includes and sorts "windows.h" ahead of
// "winsock2.h". An ordering kept by a comment is one the formatter eventually
// undoes.
#define WIN32_LEAN_AND_MEAN
#include <memory>
#include <stdexcept>
#include <string>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace engine::parallel {

	namespace {
		void CloseIfOpen(SOCKET handle) {
			if (handle != INVALID_SOCKET) {
				::closesocket(handle);
			}
		}

		// Keeps a socket out of every spawned child.
		//
		// The Windows counterpart of close-on-exec, and needed for the same
		// reason: `CreateProcessW` with handle inheritance on would otherwise
		// give a child *every* inheritable handle this process holds, including
		// the driver's own end of the very channel being handed over. While the
		// child holds a copy of that end the socket still has a peer, so
		// neither side would ever see the other go away — which is the one
		// thing the channel is relied on to notice.
		//
		// Both ends, and permanently: even the end a child is meant to have
		// does not reach it by inheritance, because an inherited socket handle
		// is not a socket. It goes over the handover pipe instead.
		void KeepFromChildren(SOCKET handle) {
			SetHandleInformation(reinterpret_cast<HANDLE>(handle), HANDLE_FLAG_INHERIT, 0);
		}

		// Turns off Nagle, which would otherwise hold a small frame back
		// waiting for a larger one to join it.
		//
		// A local socket pair carries a supervisor's heartbeats and a host's
		// answers — small frames whose whole value is arriving now. On POSIX
		// the pair is an AF_UNIX socket and there is no Nagle to turn off;
		// loopback TCP is the price of Winsock having no `socketpair`, and this
		// is the rest of that price.
		void SendImmediately(SOCKET handle) {
			const BOOL on = TRUE;
			::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
		}

		// Whether two loopback addresses name the same socket.
		bool SameEndpoint(const sockaddr_in &left, const sockaddr_in &right) {
			return left.sin_port == right.sin_port && left.sin_addr.s_addr == right.sin_addr.s_addr;
		}

		// Two connected stream sockets, or two INVALID_SOCKETs.
		//
		// The equivalent of `socketpair(AF_UNIX, SOCK_STREAM, 0, ...)`, built
		// out of the three calls Winsock does have.
		bool MakePair(SOCKET pair[2]) {
			pair[0] = INVALID_SOCKET;
			pair[1] = INVALID_SOCKET;

			SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listener == INVALID_SOCKET) {
				return false;
			}

			// Loopback and a port the kernel picks. Never a fixed port: two
			// hosts starting at once would collide on it, and a fixed port is
			// also a thing a stranger can wait on.
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;

			int size = sizeof(address);
			if (::bind(listener, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
				::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) != 0 ||
				::listen(listener, 1) != 0) {
				CloseIfOpen(listener);
				return false;
			}

			const SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (client == INVALID_SOCKET) {
				CloseIfOpen(listener);
				return false;
			}

			// Blocking, and that is fine: a loopback connect to a listening
			// socket in this same process completes without waiting on
			// anything outside the machine.
			if (::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
				CloseIfOpen(client);
				CloseIfOpen(listener);
				return false;
			}

			sockaddr_in expected{};
			int expectedSize = sizeof(expected);
			if (::getsockname(client, reinterpret_cast<sockaddr *>(&expected), &expectedSize) != 0) {
				CloseIfOpen(client);
				CloseIfOpen(listener);
				return false;
			}

			// Anything that is not the connection just made is a stranger who
			// got to the port first. Dropped rather than accepted, and the
			// accept retried, because our own connection is still queued behind
			// it.
			SOCKET server = INVALID_SOCKET;
			for (;;) {
				sockaddr_in peer{};
				int peerSize = sizeof(peer);
				server = ::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peerSize);
				if (server == INVALID_SOCKET) {
					CloseIfOpen(client);
					CloseIfOpen(listener);
					return false;
				}
				if (SameEndpoint(peer, expected)) {
					break;
				}
				::closesocket(server);
			}

			// The listener has done its one job. Left open it would keep a
			// loopback port bound for the life of the channel, for nothing.
			CloseIfOpen(listener);

			pair[0] = server;
			pair[1] = client;
			return true;
		}

		// The handover pipe this process was started with, or null.
		//
		// A value rather than a fixed slot — see `INHERITED_VARIABLE` for why
		// Windows cannot have the slot.
		HANDLE HandoverPipe() {
			wchar_t text[32] = {};
			const DWORD written =
				GetEnvironmentVariableW(platform::INHERITED_VARIABLE, text, ARRAYSIZE(text));
			if (written == 0 || written >= ARRAYSIZE(text)) {
				return nullptr;
			}

			try {
				size_t consumed = 0;
				const unsigned long long value = std::stoull(std::wstring(text, written), &consumed, 10);
				if (consumed != written) {
					return nullptr;
				}
				return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
			} catch (const std::exception &) {
				// A variable somebody else set to something that is not a
				// number. Not a host, then.
				return nullptr;
			}
		}
	}

	// --- the handover, parent side -----------------------------------------

	bool platform::MakeChannelHandover(void *&childEnd, void *&parentEnd) {
		SECURITY_ATTRIBUTES security{};
		security.nLength = sizeof(security);
		security.bInheritHandle = TRUE;

		HANDLE reading = nullptr;
		HANDLE writing = nullptr;
		if (!CreatePipe(&reading, &writing, &security, 0)) {
			return false;
		}

		// The child must not inherit the writing end, or the pipe never reports
		// end-of-file and a child whose parent died mid-handover waits forever.
		SetHandleInformation(writing, HANDLE_FLAG_INHERIT, 0);

		childEnd = reading;
		parentEnd = writing;
		return true;
	}

	bool platform::SendChannelToChild(long long socket, unsigned long processId, void *parentEnd) {
		EnsureWinsock();

		// Minted for that child specifically. This is the call the whole
		// handover exists for: an inherited socket handle arrives intact and is
		// still not a socket, and this is what Winsock offers instead.
		WSAPROTOCOL_INFOW description{};
		if (WSADuplicateSocketW(static_cast<SOCKET>(socket), processId, &description) != 0) {
			ENGINE_ERROR("could not describe a channel for the child: winsock error {}", WSAGetLastError());
			return false;
		}

		DWORD written = 0;
		const BOOL sent = WriteFile(
			static_cast<HANDLE>(parentEnd), &description, sizeof(description), &written, nullptr
		);
		if (sent == 0 || written != sizeof(description)) {
			ENGINE_ERROR("could not hand a channel to the child: {}", GetLastError());
			return false;
		}
		return true;
	}

	ProcessChannel MakeProcessChannel(const ChannelSettings &settings) {
		platform::EnsureWinsock();

		ProcessChannel pair;

		SOCKET handles[2] = {INVALID_SOCKET, INVALID_SOCKET};
		if (!MakePair(handles)) {
			ENGINE_ERROR("could not create a process channel: winsock error {}", WSAGetLastError());
			return pair;
		}

		KeepFromChildren(handles[0]);
		KeepFromChildren(handles[1]);
		SendImmediately(handles[0]);
		SendImmediately(handles[1]);

		// Only this end. The remote one is made non-blocking by whoever adopts
		// it, because a handle handed to a child is configured by the child —
		// relying on the mode surviving the spawn is the kind of thing that
		// works until somebody re-opens the handle.
		if (!platform::SocketMakeNonBlocking(static_cast<int64_t>(handles[0]))) {
			ENGINE_ERROR("could not configure a process channel: winsock error {}", WSAGetLastError());
			CloseIfOpen(handles[0]);
			CloseIfOpen(handles[1]);
			return pair;
		}

		pair.Local = std::make_unique<SocketChannel>(static_cast<int64_t>(handles[0]), settings);
		pair.Remote.Adopt(static_cast<int64_t>(handles[1]));
		return pair;
	}

	bool HasInheritedChannel() {
		const HANDLE pipe = HandoverPipe();
		if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
			return false;
		}

		// A pipe, specifically, and asked without reading anything. The
		// variable is an ordinary name, and a program run with it set to some
		// other number would otherwise be told it is a supervised host — and
		// would then try to speak a protocol down whatever that handle turned
		// out to be.
		//
		// Non-destructive on purpose: this is documented as the cheap question,
		// and a check that consumed the handover would make asking it cost the
		// caller the channel.
		return GetFileType(pipe) == FILE_TYPE_PIPE;
	}

	std::unique_ptr<Channel> AdoptInheritedChannel(const ChannelSettings &settings) {
		platform::EnsureWinsock();

		if (!HasInheritedChannel()) {
			return nullptr;
		}
		const HANDLE pipe = HandoverPipe();

		// Blocking, and that is the synchronisation: the parent cannot write
		// this until it knows this process's id, which it does not until the
		// spawn has returned. A parent that died before writing closes its end,
		// and the read then fails rather than waiting forever.
		WSAPROTOCOL_INFOW description{};
		DWORD read = 0;
		const BOOL got =
			ReadFile(pipe, &description, sizeof(description), &read, nullptr);

		// Once, whatever happens. The pipe has carried the one thing it was for,
		// and leaving it open would leave a handle nothing will ever read again.
		CloseHandle(pipe);
		SetEnvironmentVariableW(platform::INHERITED_VARIABLE, nullptr);

		if (got == 0 || read != sizeof(description)) {
			ENGINE_ERROR("the inherited channel was never handed over: {}", GetLastError());
			return nullptr;
		}

		const SOCKET handle = WSASocketW(
			FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &description, 0, WSA_FLAG_OVERLAPPED
		);
		if (handle == INVALID_SOCKET) {
			ENGINE_ERROR("could not adopt the inherited channel: winsock error {}", WSAGetLastError());
			return nullptr;
		}

		if (!platform::SocketMakeNonBlocking(static_cast<int64_t>(handle))) {
			ENGINE_ERROR("could not configure the inherited channel: winsock error {}", WSAGetLastError());
			::closesocket(handle);
			return nullptr;
		}
		return std::make_unique<SocketChannel>(static_cast<int64_t>(handle), settings);
	}
}
