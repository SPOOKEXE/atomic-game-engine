#include "../Socket.hpp"

#include "Winsock.hpp"

// WIN32_LEAN_AND_MEAN before anything, and it is load-bearing rather than a
// compile-time saving: without it <windows.h> drags in the original <winsock.h>,
// which redefines everything <winsock2.h> declares. With it, the two can be
// included in either order — which they have to be, because `.clang-format`
// regroups and sorts system includes and sorts "windows.h" ahead of
// "winsock2.h". An ordering kept by a comment is one the formatter eventually
// undoes.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

namespace engine::parallel::platform {

	namespace {
		// Winsock's handle is a UINT_PTR and its "no socket" value is all bits
		// set, which is the same -1 the module already uses for "no handle".
		// So the two agree without a translation table, and this is only the
		// cast that says so.
		SOCKET Native(int64_t handle) {
			return static_cast<SOCKET>(handle);
		}

		// Whether the last error means "come back later" rather than "gone".
		//
		// WSAEINTR is in here rather than retried because Winsock only raises
		// it when a blocking call is cancelled, which cannot happen to a socket
		// this module owns: every one of them is non-blocking.
		bool WouldBlock(int error) {
			return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
		}
	}

	void EnsureWinsock() {
		// A function-local static, so the initialisation race is the language's
		// problem rather than this file's.
		static const bool started = [] {
			WSADATA data{};
			return WSAStartup(MAKEWORD(2, 2), &data) == 0;
		}();
		(void)started;
	}

	ptrdiff_t SocketSend(int64_t handle, const std::byte *data, size_t size) {
		// Winsock takes an int length. A frame may be megabytes, so the call is
		// capped rather than truncating the count into an int and writing a
		// negative length; the caller loops on a partial write already.
		const int chunk = static_cast<int>(size > 0x40000000u ? 0x40000000u : size);

		const int written = ::send(Native(handle), reinterpret_cast<const char *>(data), chunk, 0);
		if (written != SOCKET_ERROR) {
			return static_cast<ptrdiff_t>(written);
		}

		// Nothing to suppress here: Windows has no SIGPIPE, so a write to a
		// dead peer was never going to kill this process. It returns
		// WSAECONNRESET or WSAECONNABORTED and that is all.
		return WouldBlock(WSAGetLastError()) ? SOCKET_BLOCKED : SOCKET_FAILED;
	}

	ptrdiff_t SocketReceive(int64_t handle, std::byte *data, size_t size) {
		const int chunk = static_cast<int>(size > 0x40000000u ? 0x40000000u : size);

		const int got = ::recv(Native(handle), reinterpret_cast<char *>(data), chunk, 0);
		if (got != SOCKET_ERROR) {
			// Zero is an orderly shutdown by the peer, which the caller tells
			// apart from SOCKET_BLOCKED.
			return static_cast<ptrdiff_t>(got);
		}

		return WouldBlock(WSAGetLastError()) ? SOCKET_BLOCKED : SOCKET_FAILED;
	}

	void SocketClose(int64_t handle) {
		::closesocket(Native(handle));
	}

	bool SocketMakeNonBlocking(int64_t handle) {
		u_long enabled = 1;
		return ::ioctlsocket(Native(handle), FIONBIO, &enabled) == 0;
	}
}
