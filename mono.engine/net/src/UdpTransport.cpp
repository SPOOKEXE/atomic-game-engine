#include <engine/net/Transport.hpp>

#include <algorithm>
#include <asio/detail/socket_option.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>
#include <cstring>
#include <memory>
#include <optional>
#include <system_error>

// The real socket, and the only file in this module that knows what asio is.
//
// **Synchronous and non-blocking, not asynchronous.** asio is here for the
// portable socket rather than for its reactor: an asynchronous receive completes
// on whichever thread is running the `io_context`, and a tick that has to replay
// identically cannot have datagrams arriving on somebody else's thread at a
// moment that depends on scheduling. So the socket is put in non-blocking mode
// and drained from the tick, `io_context` exists only because a socket has to be
// constructed against one, and nothing ever calls `run()`.
//
// **Every failure is a status, never an exception.** Every asio call here takes
// an `error_code`. A datagram that cannot be sent is an ordinary event on an
// unreliable transport, and a bind that fails because the port is taken is an
// ordinary outcome of starting a second server on one machine.
//
// **A receive error is per-datagram, not per-socket.** An ICMP rejection from an
// earlier send is reported on the *next* receive, and on Windows it arrives as
// `connection_reset` on an unconnected socket. Treating that as the socket
// failing would let anyone kill a server by sending it one datagram from a
// closed port, so the datagram is dropped and the next one is read.

namespace engine::net {
	namespace {
		// How many datagram-level errors to step over in one Receive before
		// giving up until the next call. Bounded rather than a `while (true)`,
		// so a socket that fails every read cannot hold the tick.
		constexpr int MAXIMUM_DROPS = 8;

		Endpoint FromAsio(const asio::ip::udp::endpoint &source) {
			const asio::ip::address address = source.address();
			if (address.is_v4()) {
				return Endpoint::FromIPv4(address.to_v4().to_bytes(), source.port());
			}
			if (address.is_v6()) {
				return Endpoint::FromIPv6(address.to_v6().to_bytes(), source.port());
			}
			return {};
		}

		std::optional<asio::ip::udp::endpoint> ToAsio(const Endpoint &source) {
			if (source.Family == AddressFamily::IPv4) {
				asio::ip::address_v4::bytes_type bytes{};
				std::memcpy(bytes.data(), source.Address.data(), bytes.size());
				return asio::ip::udp::endpoint(asio::ip::address_v4(bytes), source.Port);
			}
			if (source.Family == AddressFamily::IPv6) {
				asio::ip::address_v6::bytes_type bytes{};
				std::memcpy(bytes.data(), source.Address.data(), bytes.size());
				return asio::ip::udp::endpoint(asio::ip::address_v6(bytes), source.Port);
			}
			return std::nullopt;
		}

		class UdpTransport final : public Transport {
		  public:
			explicit UdpTransport(const TransportSettings &settings) : Limits(settings), Socket(Context) {
				Limits.MaximumDatagram = std::min(Limits.MaximumDatagram, MAXIMUM_DATAGRAM_BYTES);

				// One byte past the maximum, so a datagram over it comes back
				// full rather than exactly at the limit and is therefore
				// *detectable*. Truncating one to the limit and handing it on
				// would present a mangled frame to Packet::Read as though the
				// sender had written it that way.
				Scratch.resize(Limits.MaximumDatagram + 1);
			}

			~UdpTransport() override {
				Close();
			}

			// Binds, reports whether it worked, and leaves the socket unusable
			// when it did not.
			bool Bind(uint16_t port) {
				const asio::ip::udp::endpoint local(asio::ip::udp::v4(), port);

				std::error_code failure;
				Socket.open(local.protocol(), failure);
				if (failure) {
					return false;
				}

				// Before the bind, and it has to be: both options change what
				// the bind is allowed to do, and setting them afterwards is a
				// no-op the operating system reports as success.
				if (Limits.ReuseAddress) {
					std::error_code sharing;
					Socket.set_option(asio::socket_base::reuse_address(true), sharing);
#if defined(SO_REUSEPORT)
					// SO_REUSEADDR alone does not let two sockets hold one UDP
					// port on Linux or macOS - it only shortens the wait after a
					// close. SO_REUSEPORT is the one that shares, and it has no
					// asio name, so it is spelled out. Windows has no such
					// option and does not need one: SO_REUSEADDR already means
					// sharing there.
					const asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT> reusePort(true);
					Socket.set_option(reusePort, sharing);
#endif
					// Both are advisory. A kernel that refuses leaves a second
					// listener unable to bind, which fails at the bind below
					// with the reason rather than here without one.
				}

				Socket.bind(local, failure);
				if (failure) {
					Socket.close(failure);
					return false;
				}
				Socket.non_blocking(true, failure);
				if (failure) {
					Socket.close(failure);
					return false;
				}

				// The setting means the same thing on both implementations: how
				// much may wait before a send is refused. Advisory - a kernel
				// may round it - which is why nothing reads it back.
				if (Limits.Broadcast) {
					// Asked for rather than always on. A socket that could
					// broadcast by default is one where a mistyped destination
					// reaches every machine on the link instead of nobody.
					std::error_code reach;
					Socket.set_option(asio::socket_base::broadcast(true), reach);
					if (reach) {
						Socket.close(failure);
						return false;
					}
				}

				std::error_code sizing;
				Socket.set_option(
					asio::socket_base::receive_buffer_size(static_cast<int>(Limits.ReceiveQueueBytes)), sizing
				);
				Socket.set_option(
					asio::socket_base::send_buffer_size(static_cast<int>(Limits.ReceiveQueueBytes)), sizing
				);

				const asio::ip::udp::endpoint bound = Socket.local_endpoint(failure);
				if (failure) {
					Socket.close(failure);
					return false;
				}

				// Cached at bind rather than asked for on every call: a port of
				// zero becomes a real one exactly once, and local_endpoint is a
				// system call.
				Address = FromAsio(bound);
				return true;
			}

			TransportStatus Send(const Endpoint &to, std::span<const std::byte> datagram) override {
				if (!Open()) {
					return TransportStatus::Closed;
				}
				if (datagram.size() > Limits.MaximumDatagram) {
					return TransportStatus::TooLarge;
				}

				const std::optional<asio::ip::udp::endpoint> target = ToAsio(to);
				// A v6 destination from a v4 socket is refused here rather than
				// by the system call, so the status says which of the two
				// mistakes was made.
				if (!target || to.Family != Address.Family) {
					return TransportStatus::Unreachable;
				}

				std::error_code failure;
				Socket.send_to(asio::buffer(datagram.data(), datagram.size()), *target, 0, failure);
				if (!failure) {
					return TransportStatus::Ok;
				}
				if (failure == asio::error::would_block || failure == asio::error::try_again) {
					// The send buffer is full. Refused rather than waited on:
					// a blocking send stalls the tick that made it.
					return TransportStatus::Full;
				}
				if (failure == asio::error::bad_descriptor || failure == asio::error::not_socket) {
					return TransportStatus::Closed;
				}
				return TransportStatus::Unreachable;
			}

			Inbound Receive(std::vector<std::byte> &datagram) override {
				if (!Open()) {
					return {TransportStatus::Closed, {}};
				}

				for (int attempt = 0; attempt < MAXIMUM_DROPS; ++attempt) {
					asio::ip::udp::endpoint from;
					std::error_code failure;
					const size_t received =
						Socket.receive_from(asio::buffer(Scratch.data(), Scratch.size()), from, 0, failure);

					if (failure == asio::error::would_block || failure == asio::error::try_again) {
						return {TransportStatus::Empty, {}};
					}
					if (failure) {
						// Somebody else's datagram went wrong, not this socket.
						continue;
					}
					if (received > Limits.MaximumDatagram) {
						// Over the maximum, and therefore already truncated by
						// the system call. Dropped whole: half a frame parses
						// as a corrupt one, which is worse than none.
						continue;
					}

					datagram.assign(Scratch.begin(), Scratch.begin() + static_cast<ptrdiff_t>(received));
					return {TransportStatus::Ok, FromAsio(from)};
				}
				return {TransportStatus::Empty, {}};
			}

			Endpoint Local() const override {
				return Socket.is_open() ? Address : Endpoint{};
			}

			bool Open() const override {
				return Socket.is_open();
			}

			void Close() override {
				std::error_code ignored;
				Socket.close(ignored);
			}

		  private:
			TransportSettings Limits;

			// Declared before the socket, because the socket is constructed
			// against it and destroyed before it.
			asio::io_context Context;
			asio::ip::udp::socket Socket;

			Endpoint Address;
			std::vector<std::byte> Scratch;
		};
	}

	std::unique_ptr<Transport> MakeUdpTransport(uint16_t port, const TransportSettings &settings) {
		auto transport = std::make_unique<UdpTransport>(settings);
		if (!transport->Bind(port)) {
			// A null rather than a throw or a half-open object: a machine with
			// no network and a port already in use are both ordinary, and a
			// caller can fall back to the loopback.
			return nullptr;
		}
		return transport;
	}
}
