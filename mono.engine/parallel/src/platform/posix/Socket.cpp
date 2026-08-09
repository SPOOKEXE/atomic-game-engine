#include "../Socket.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if !defined(MSG_NOSIGNAL)
// Absent on macOS and iOS, where the same job is done by the SO_NOSIGPIPE
// option set on the socket when the pair is created. Defined away rather
// than branched at every call site.
#define MSG_NOSIGNAL 0
#endif

namespace engine::parallel::platform {

	ptrdiff_t SocketSend(int64_t handle, const std::byte *data, size_t size) {
		for (;;) {
			// MSG_NOSIGNAL so a send to a dead peer fails rather than raising
			// SIGPIPE, and MSG_DONTWAIT so this never waits even if somebody
			// hands over a handle that was not made non-blocking.
			const ssize_t written = ::send(static_cast<int>(handle), data, size, MSG_NOSIGNAL | MSG_DONTWAIT);

			if (written >= 0) {
				return static_cast<ptrdiff_t>(written);
			}
			if (errno == EINTR) {
				// Retried here rather than reported, because the one caller
				// that cannot come back later is the final flush in `Close`.
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return SOCKET_BLOCKED;
			}

			// EPIPE or ECONNRESET.
			return SOCKET_FAILED;
		}
	}

	ptrdiff_t SocketReceive(int64_t handle, std::byte *data, size_t size) {
		for (;;) {
			const ssize_t got = ::recv(static_cast<int>(handle), data, size, MSG_DONTWAIT);

			if (got >= 0) {
				// Zero is an orderly shutdown by the peer, which the caller
				// tells apart from SOCKET_BLOCKED.
				return static_cast<ptrdiff_t>(got);
			}
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return SOCKET_BLOCKED;
			}

			return SOCKET_FAILED;
		}
	}

	void SocketClose(int64_t handle) {
		::close(static_cast<int>(handle));
	}

	bool SocketMakeNonBlocking(int64_t handle) {
		const int descriptor = static_cast<int>(handle);
		const int flags = ::fcntl(descriptor, F_GETFL, 0);
		if (flags < 0) {
			return false;
		}
		return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
	}
}
