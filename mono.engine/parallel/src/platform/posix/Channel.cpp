// Making the endpoints that cross a process boundary.
//
// A connected socket pair rather than two pipes: one handle per end instead of
// four, and bidirectional without any wiring. What the handle then behaves like
// is `SocketChannel`'s job and owning one is `ProcessChannel.cpp`'s; this file
// only creates the pair, hands one end to a caller who is about to spawn a
// child, and picks the other up again in that child.

#include "../../SocketChannel.hpp"
#include "../Socket.hpp"
#include "InheritedSlot.hpp"

#include <engine/core/Log.hpp>
#include <engine/parallel/ProcessChannel.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

namespace engine::parallel {

	ProcessChannel MakeProcessChannel(const ChannelSettings &settings) {
		ProcessChannel pair;

		int handles[2] = {-1, -1};
		if (::socketpair(AF_UNIX, SOCK_STREAM, 0, handles) != 0) {
			ENGINE_ERROR("could not create a process channel: {}", std::strerror(errno));
			return pair;
		}

		// Close-on-exec on both. Without it a spawned child inherits *every*
		// handle this process holds, including the driver's own end of this
		// very channel — and while the child holds a copy of that end, the
		// kernel counts the socket as still having a peer. Neither side would
		// ever see the other go away, which is the one thing the channel is
		// relied on to notice.
		//
		// The end the child is meant to have is placed by `dup2` at spawn
		// time, and `dup2` clears close-on-exec on what it creates.
		::fcntl(handles[0], F_SETFD, FD_CLOEXEC);
		::fcntl(handles[1], F_SETFD, FD_CLOEXEC);

#if defined(SO_NOSIGPIPE)
		// macOS and iOS have no MSG_NOSIGNAL, so the same promise — that a
		// write to a dead peer fails rather than killing this process — is a
		// socket option there instead of a per-call flag. Set on both ends,
		// because the option travels with the socket and the child's end has
		// nowhere else to acquire it.
		const int on = 1;
		::setsockopt(handles[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
		::setsockopt(handles[1], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

		// Only this end. The remote one is made non-blocking by whoever adopts
		// it, because `O_NONBLOCK` is a property of the open file description
		// and setting it here would set it for the child too — which is
		// correct, but relying on that to cross an exec is the kind of thing
		// that works until somebody re-opens the handle.
		if (!platform::SocketMakeNonBlocking(handles[0])) {
			ENGINE_ERROR("could not configure a process channel: {}", std::strerror(errno));
			::close(handles[0]);
			::close(handles[1]);
			return pair;
		}

		pair.Local = std::make_unique<SocketChannel>(handles[0], settings);
		pair.Remote.Adopt(handles[1]);
		return pair;
	}

	bool HasInheritedChannel() {
		// A stream socket, specifically. The slot is an ordinary handle number
		// and a program run from a shell that happened to leave one open there
		// would otherwise be told it is a supervised host — and would then try
		// to speak a protocol down somebody's log file.
		int kind = 0;
		socklen_t size = sizeof(kind);
		if (::getsockopt(INHERITED, SOL_SOCKET, SO_TYPE, &kind, &size) != 0) {
			return false;
		}
		return kind == SOCK_STREAM;
	}

	std::unique_ptr<Channel> AdoptInheritedChannel(const ChannelSettings &settings) {
		if (!HasInheritedChannel()) {
			return nullptr;
		}
		if (!platform::SocketMakeNonBlocking(INHERITED)) {
			ENGINE_ERROR("could not configure the inherited channel: {}", std::strerror(errno));
			return nullptr;
		}
		return std::make_unique<SocketChannel>(INHERITED, settings);
	}
}
