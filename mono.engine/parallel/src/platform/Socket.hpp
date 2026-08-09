#pragma once

// The four socket calls `SocketChannel` makes, with the platform taken out of
// them.
//
// A private header, so it is unreachable from outside `parallel`. It names no
// operating system; the implementations under src/platform/<os>/ do, and the
// build picks exactly one of them.
//
// **This is the whole platform difference, and it is deliberately this small.**
// The framing, the buffering and the backpressure in `SocketChannel` are the
// parts that are subtle and the parts that would silently diverge if there were
// one copy per platform. What actually differs between a Berkeley socket and a
// Winsock one is the type of the handle, the spelling of "would block", and
// which function closes it. Those four are here; nothing else needs to be.

#include <cstddef>
#include <cstdint>

namespace engine::parallel::platform {

	// Nothing moved, and the socket is fine. The caller stops and comes back.
	//
	// Distinct from zero, because a zero-byte read means the peer shut down in
	// an orderly way and a zero-byte "would block" means it did not.
	constexpr ptrdiff_t SOCKET_BLOCKED = -1;

	// The peer is gone. Nothing queued will ever arrive, in either direction.
	constexpr ptrdiff_t SOCKET_FAILED = -2;

	// Writes what the socket will take, without blocking and without raising
	// anything when the peer is gone.
	//
	// **Nothing here may kill the process.** A driver that died because a host
	// crashed would turn one host's fault into the whole server's, which is the
	// opposite of what processes are here for. So a write to a dead peer
	// reports `SOCKET_FAILED` rather than raising `SIGPIPE`.
	//
	// Retries an interrupted call itself, because the one caller that cannot
	// come back later is the final flush in `Close`.
	//
	// @param handle The socket, which must already be non-blocking.
	// @param data   The bytes to write.
	// @param size   How many, at least one.
	// @return The count written, `SOCKET_BLOCKED`, or `SOCKET_FAILED`.
	ptrdiff_t SocketSend(int64_t handle, const std::byte *data, size_t size);

	// Reads whatever the socket has, without blocking.
	//
	// @param handle The socket, which must already be non-blocking.
	// @param data   Where the bytes go.
	// @param size   The most to read, at least one.
	// @return The count read, zero for an orderly shutdown by the peer,
	//         `SOCKET_BLOCKED`, or `SOCKET_FAILED`.
	ptrdiff_t SocketReceive(int64_t handle, std::byte *data, size_t size);

	// Releases a socket handle.
	//
	// @param handle The socket, which must be one this process owns.
	void SocketClose(int64_t handle);

	// Puts a socket into the non-blocking mode the channel relies on.
	//
	// @param handle The socket.
	// @return `false` when the operating system refused.
	bool SocketMakeNonBlocking(int64_t handle);
}
