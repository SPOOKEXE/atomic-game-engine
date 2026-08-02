#include <engine/core/Log.hpp>
#include <engine/replication/Listener.hpp>

namespace engine::replication {

	Listener::Listener(net::Transport &transport, const ListenerSettings &settings)
		: Transport_(&transport), Settings(settings), Authority_(settings.Authority) {}

	Listener::Peer *Listener::Find(const net::Endpoint &from) {
		for (Peer &peer : Peers) {
			if (peer.Where == from) {
				return &peer;
			}
		}
		return nullptr;
	}

	Listener::Peer *Listener::Admit(const net::Endpoint &from, double nowSeconds) {
		if (Peers.size() >= Settings.MaximumClients) {
			// Full, not broken. Counted rather than logged per datagram: a peer
			// that keeps trying would otherwise write a log line per packet,
			// which is its own denial of service.
			Stats_.Turned++;
			return nullptr;
		}

		Peer peer;
		peer.Where = from;
		peer.Client = Authority_.Admit();
		peer.Wire = std::make_unique<Session>(
			*Transport_, from, net::ConnectionId{NextConnection++, 1}, nowSeconds, Settings.Session
		);

		// There is no handshake to complete, because there is no handshake — see
		// the note at the top of the header. Completing it here rather than
		// leaving the link `Connecting` is what lets payload flow at all, and it
		// is the single line that a real key exchange replaces.
		peer.Wire->Link().CompleteHandshake(nowSeconds);

		Peers.push_back(std::move(peer));
		Stats_.Admitted++;

		ENGINE_INFO("replication: admitted {} ({} connected)", from.Text(), Peers.size());
		return &Peers.back();
	}

	void Listener::Drop(size_t index) {
		Authority_.Remove(Peers[index].Client);

		ENGINE_INFO("replication: dropped {} ({} connected)", Peers[index].Where.Text(), Peers.size() - 1);

		// Swap-back rather than erase-shift, for the reason a column does it:
		// the order peers sit in means nothing, and a client is addressed by its
		// `ClientId` rather than by its position here.
		Peers[index] = std::move(Peers.back());
		Peers.pop_back();
		Stats_.Dropped++;
	}

	void Listener::Poll(double nowSeconds) {
		// Until the transport says `Empty`. A datagram count would be a cap on
		// how fast a server can drain its socket, and the receive queue is
		// already bounded by the transport's own setting.
		for (;;) {
			const net::Transport::Inbound inbound = Transport_->Receive(Datagram);
			if (inbound.Status != net::TransportStatus::Ok) {
				break;
			}

			Peer *peer = Find(inbound.From);
			if (peer == nullptr) {
				peer = Admit(inbound.From, nowSeconds);
				if (peer == nullptr) {
					continue;
				}
			}

			// Refusals are the session's to count. A datagram that is not a
			// packet, or is stale, is ordinary traffic on an unreliable network
			// rather than something this loop should react to.
			peer->Wire->Receive(Datagram, nowSeconds);

			for (const std::vector<std::byte> &message : peer->Wire->Inbound()) {
				Authority_.Receive(peer->Client, message);
			}
			peer->Wire->ClearInbound();
		}
	}

	void Listener::Advance(double nowSeconds) {
		for (size_t index = Peers.size(); index > 0; index--) {
			Peer &peer = Peers[index - 1];
			peer.Wire->Link().Advance(nowSeconds);
			peer.Wire->Link().ResetBudget();

			if (peer.Wire->Link().State() == net::ConnectionState::Disconnected) {
				Drop(index - 1);
			}
		}
	}

	void Listener::Publish(ecs::Store &store, uint64_t tick, double nowSeconds) {
		Authority_.Publish(store, tick);

		for (Peer &peer : Peers) {
			for (const std::vector<std::byte> &message : Authority_.Outgoing(peer.Client)) {
				// A refusal is backpressure rather than an ending: the link is
				// over its per-tick budget or the reliable window is full, and
				// the next tick's `Publish` rebuilds what still has to go.
				peer.Wire->Send(message, nowSeconds);
			}
			peer.Wire->Flush(nowSeconds);
		}
	}

	std::vector<Listener::Submission> Listener::Inputs() const {
		std::vector<Submission> submissions;
		submissions.reserve(Peers.size());

		for (const Peer &peer : Peers) {
			const std::span<const Input> inputs = Authority_.Inputs(peer.Client);
			if (!inputs.empty()) {
				submissions.push_back(Submission{peer.Client, inputs});
			}
		}
		return submissions;
	}

	void Listener::ClearInputs() {
		for (const Peer &peer : Peers) {
			Authority_.ClearInputs(peer.Client);
		}
	}
}
