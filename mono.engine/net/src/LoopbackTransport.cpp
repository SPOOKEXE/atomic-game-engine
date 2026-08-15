#include <engine/net/Transport.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// The in-process datagram network, and the socket's behaviour without a socket.
//
// A small routing table rather than a wired-up pair. Every end has an address,
// a send looks the destination up, and a destination that is not in the table is
// dropped exactly as the network drops one. That is what keeps single-player and
// a real server the same code: the two cases a pair cannot express - a datagram
// from a sender this end has never heard of, and one addressed nowhere - are the
// two the layer above has to handle, so the loopback must be able to produce
// them.
//
// **Locked per end rather than per network.** Two ends sending to different
// third parties do not contend, which matters once a host is running several
// worlds against one loopback. The routing table itself is fixed at
// construction, so a lookup takes no lock at all.
//
// The queue is bounded and a send into a full one is refused. On a socket that
// refusal is the send buffer saying `EWOULDBLOCK`; here it is the receive queue
// at its cap. Both mean the same thing to a caller - not sent, try later - and
// neither ever blocks.

namespace engine::net {

	// Lives here, with the first implementation of the interface, rather than in
	// a file of its own that would hold one switch.
	const char *Describe(TransportStatus status) {
		switch (status) {
		case TransportStatus::Ok:
			return "ok";
		case TransportStatus::Empty:
			return "empty";
		case TransportStatus::Full:
			return "full";
		case TransportStatus::Closed:
			return "closed";
		case TransportStatus::TooLarge:
			return "datagram too large";
		case TransportStatus::Unreachable:
			return "unreachable";
		}
		// No default label, so adding a status is a compiler warning here.
		return "?";
	}

	namespace {
		// A datagram waiting to be received, and who sent it.
		struct Queued {
			Endpoint From;
			std::vector<std::byte> Bytes;
		};

		// One end's mailbox.
		struct Mailbox {
			Endpoint Address;

			mutable std::mutex Guard;
			std::deque<Queued> Inbound;
			size_t Bytes = 0;
			bool Attached = true;
		};

		// Every end, and what they agreed to.
		//
		// Shared rather than owned by one end, because any end may be destroyed
		// first and the survivors keep routing.
		struct Network {
			TransportSettings Settings;

			// Fixed at construction and never resized, so a lookup needs no
			// lock. Growing it later would need one, and the comment above the
			// lookup is the place that would have to change.
			std::vector<std::shared_ptr<Mailbox>> Mailboxes;
		};

		class LoopbackTransport final : public Transport {
		  public:
			LoopbackTransport(std::shared_ptr<Network> network, std::shared_ptr<Mailbox> self)
				: Shared(std::move(network)), Self(std::move(self)) {}

			~LoopbackTransport() override {
				Close();
			}

			TransportStatus Send(const Endpoint &to, std::span<const std::byte> datagram) override {
				if (!Open()) {
					return TransportStatus::Closed;
				}
				if (datagram.size() > Shared->Settings.MaximumDatagram) {
					return TransportStatus::TooLarge;
				}
				if (!to.IsValid()) {
					return TransportStatus::Unreachable;
				}

				if (IsBroadcast(to)) {
					// Refused rather than delivered when the end did not ask for
					// it, because that is what a socket does - and a loopback
					// that broadcast anyway would let a discovery beacon pass
					// its suite and then fail to send a single datagram on the
					// machine it shipped to.
					if (!Shared->Settings.Broadcast) {
						return TransportStatus::Unreachable;
					}
					for (const std::shared_ptr<Mailbox> &mailbox : Shared->Mailboxes) {
						if (mailbox != Self) {
							Deliver(*mailbox, datagram);
						}
					}
					// One status for the whole send. A broadcast that filled one
					// peer's queue and not another's has no useful answer to
					// give - the sender cannot resend to just the one - and on a
					// socket the kernel would report success either way.
					return TransportStatus::Ok;
				}

				const std::shared_ptr<Mailbox> target = Find(to);
				if (!target) {
					// Nobody is listening there. `Ok` and dropped, because that
					// is what a socket does - a loopback that reported a
					// delivery failure would let single-player branch on
					// something the real network never says.
					return TransportStatus::Ok;
				}

				return Deliver(*target, datagram) ? TransportStatus::Ok : TransportStatus::Full;
			}

			Inbound Receive(std::vector<std::byte> &datagram) override {
				std::lock_guard lock(Self->Guard);
				if (!Self->Attached) {
					return {TransportStatus::Closed, {}};
				}
				if (Self->Inbound.empty()) {
					return {TransportStatus::Empty, {}};
				}

				// Assigned rather than moved from, so the caller's capacity
				// survives and a transport polled every tick stops allocating.
				Queued &front = Self->Inbound.front();
				datagram.assign(front.Bytes.begin(), front.Bytes.end());

				const Inbound result{TransportStatus::Ok, front.From};
				Self->Bytes -= front.Bytes.size();
				Self->Inbound.pop_front();
				return result;
			}

			Endpoint Local() const override {
				return Open() ? Self->Address : Endpoint{};
			}

			bool Open() const override {
				std::lock_guard lock(Self->Guard);
				return Self->Attached;
			}

			void Close() override {
				std::lock_guard lock(Self->Guard);
				Self->Attached = false;

				// Dropped rather than left to drain. A closed socket's receive
				// buffer goes with it, and a loopback that let a caller keep
				// reading after Close would be the one implementation where
				// that works.
				Self->Inbound.clear();
				Self->Bytes = 0;
			}

		  private:
			// Whether an address is the limited broadcast one, whatever port it
			// names. Compared over the four v4 bytes rather than the whole
			// sixteen, because the rest are the zero padding `FromIPv4` leaves.
			static bool IsBroadcast(const Endpoint &address) {
				if (address.Family != AddressFamily::IPv4) {
					return false;
				}
				for (size_t index = 0; index < 4; ++index) {
					if (address.Address[index] != std::byte{0xFF}) {
						return false;
					}
				}
				return true;
			}

			// Puts one datagram in one mailbox.
			//
			// @return `false` only when the queue is full. A detached peer is
			//         `true` and dropped, because a socket is not told that the
			//         port it sent to has gone away.
			bool Deliver(Mailbox &target, std::span<const std::byte> datagram) const {
				std::lock_guard lock(target.Guard);
				if (!target.Attached) {
					return true;
				}
				if (target.Bytes + datagram.size() > Shared->Settings.ReceiveQueueBytes) {
					return false;
				}

				target.Inbound.push_back({Self->Address, {datagram.begin(), datagram.end()}});
				target.Bytes += datagram.size();
				return true;
			}

			std::shared_ptr<Mailbox> Find(const Endpoint &address) const {
				for (const std::shared_ptr<Mailbox> &mailbox : Shared->Mailboxes) {
					if (mailbox->Address == address) {
						return mailbox;
					}
				}
				return nullptr;
			}

			std::shared_ptr<Network> Shared;
			std::shared_ptr<Mailbox> Self;
		};
	}

	std::vector<std::unique_ptr<Transport>>
	MakeLoopbackTransport(size_t peerCount, const TransportSettings &settings) {
		// The numbering is a port, so it cannot exceed what a port holds. Nobody
		// wants sixty-five thousand loopback ends; the clamp is here so that a
		// count that came from somewhere else cannot fold two ends onto one
		// address, which would route a datagram to whichever was found first.
		const size_t peers = std::min<size_t>(peerCount, 0xFFFFu);

		auto network = std::make_shared<Network>();
		network->Settings = settings;
		network->Settings.MaximumDatagram =
			std::min(network->Settings.MaximumDatagram, Transport::MAXIMUM_DATAGRAM_BYTES);
		network->Mailboxes.reserve(peers);

		for (size_t index = 0; index < peers; ++index) {
			auto mailbox = std::make_shared<Mailbox>();
			mailbox->Address = Endpoint::LoopbackIPv4(static_cast<uint16_t>(index + 1));
			network->Mailboxes.push_back(std::move(mailbox));
		}

		std::vector<std::unique_ptr<Transport>> ends;
		ends.reserve(peers);
		for (const std::shared_ptr<Mailbox> &mailbox : network->Mailboxes) {
			ends.push_back(std::make_unique<LoopbackTransport>(network, mailbox));
		}
		return ends;
	}
}
