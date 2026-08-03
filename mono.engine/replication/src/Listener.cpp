#include <engine/core/Log.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Listener.hpp>

#include <algorithm>
#include <utility>

namespace engine::replication {

	namespace {
		// Frames one admission message into `into`, or empties it.
		//
		// These go straight to the transport rather than through a `Session`,
		// because for the challenge there is no session — that is what makes it
		// free. Nothing paces them: the volume is bounded by the initiator's
		// repeat timer and by `LinkSettings::HandshakeTimeoutSeconds` at the far
		// end, and the reply is never larger than the message that asked for it.
		template <class T> void Frame(std::vector<std::byte> &into, const T &message) {
			core::ByteWriter body;
			WriteAdmission(body, message);

			core::ByteWriter datagram;
			if (!FrameAdmission(datagram, body.Bytes())) {
				into.clear();
				return;
			}

			into.assign(datagram.Bytes().begin(), datagram.Bytes().end());
		}
	}

	Listener::Listener(net::Transport &transport, const ListenerSettings &settings)
		: Transport_(&transport), Settings(settings), Authority_(settings.Authority),
		  Cookie_(net::Cookie::Begin(settings.Cookie)) {
		if (!Cookie_.has_value()) {
			// Fail closed, loudly, once. Carrying on with a guessable challenge
			// secret would admit clients and report every counter as healthy
			// while the challenge protected nothing at all.
			ENGINE_ERROR(
				"replication: no operating system entropy for the admission challenge, so this listener "
				"admits nobody."
			);
		}

		// The authority's cap and the link's budget spend the same packets, and
		// nothing else in the build relates the two numbers. Set the cap above
		// what the link will carry and the authority stops being the thing that
		// decides what is dropped — `Link::Reserve` takes the decision back and
		// drops the tail, which is exactly the arbitrary policy D00007 is about.
		const size_t wanted = settings.Authority.MessagesPerTick + settings.Authority.ChunksPerTick;
		if (wanted > settings.Session.Link.PacketsPerTick) {
			ENGINE_WARN(
				"replication: {} delta messages plus {} snapshot chunks per tick is more than the link's "
				"{} packets, so the excess is refused rather than prioritised.",
				settings.Authority.MessagesPerTick,
				settings.Authority.ChunksPerTick,
				settings.Session.Link.PacketsPerTick
			);
		}
	}

	Listener::Peer *Listener::Find(const net::Endpoint &from) {
		for (Peer &peer : Peers) {
			if (peer.Where == from) {
				return &peer;
			}
		}
		return nullptr;
	}

	void Listener::SetAdmission(AdmissionPolicy policy) {
		Policy = std::move(policy);
	}

	void Listener::Greet(const net::Endpoint &from, std::span<const std::byte> datagram, double nowSeconds) {
		core::ByteReader reader(datagram);
		const std::optional<net::Packet::Inbound> packet = net::Packet::Read(reader);
		if (!packet.has_value()) {
			Stats_.Refused++;
			return;
		}

		core::ByteReader body(packet->Payload);
		Admission message;
		if (!ReadAdmission(body, message)) {
			Stats_.Refused++;
			return;
		}

		if (Peer *peer = Find(from); peer != nullptr) {
			Repeat(*peer, message);
			return;
		}

		switch (message.Kind) {
		case AdmissionKind::Hello:
			Challenge(from, message.Hello, nowSeconds);
			return;

		case AdmissionKind::Answer:
			Accept(from, message.Answer, nowSeconds);
			return;

		case AdmissionKind::Challenge:
		case AdmissionKind::Welcome:
			// Server to client only. A client sending one is a client trying to
			// admit itself, which is the whole thing this exchange is for.
			Stats_.Refused++;
			return;
		}

		Stats_.Refused++;
	}

	void Listener::Challenge(const net::Endpoint &from, const replication::Hello &hello, double nowSeconds) {
		if (!Cookie_.has_value()) {
			Stats_.Refused++;
			return;
		}

		// **This is the whole of what a hello costs.** One HMAC and one
		// datagram the same size as the one that asked for it. Nothing is
		// written down, so a hundred thousand of these and one of them leave
		// this process holding exactly the same amount of state, and the reply
		// is no larger than the question so this cannot be aimed at a third
		// party as a reflector.
		replication::Challenge reply;
		reply.Cookie = Cookie_->Issue(nowSeconds, from, hello.PublicKey);

		Frame(Reply, reply);
		Transport_->Send(from, Reply);
		Stats_.Challenged++;
	}

	void Listener::Accept(const net::Endpoint &from, const replication::Answer &answer, double nowSeconds) {
		if (!Cookie_.has_value()) {
			Stats_.Refused++;
			return;
		}

		// The cookie first, because it is the cheapest check and the one that
		// proves the peer can receive where it says it can. Everything after it
		// costs more than the datagram that asked for it.
		if (!Cookie_->Answers(nowSeconds, from, answer.PublicKey, answer.Cookie)) {
			Stats_.Refused++;
			return;
		}

		// **The bound, still in front of everything a slot costs.** A full
		// server is the worst outcome of a flood, which is what makes this a
		// gap rather than a hole — and the handshake in front of it does not
		// change that, it only means the flood has to come from addresses that
		// really answer.
		if (Peers.size() >= Settings.MaximumClients) {
			Stats_.Turned++;
			return;
		}

		// The game's answer to "who is allowed at all", asked before an X25519
		// is spent on the peer and before a slot is taken. Nothing is sent
		// back: telling a stranger why it was refused tells it what to change.
		if (Policy && !Policy(Applicant{from, Peers.size(), nowSeconds})) {
			Stats_.Rejected++;
			return;
		}

		std::optional<net::Handshake> exchange = net::Handshake::Begin(net::HandshakeRole::Responder);
		if (!exchange.has_value() || !exchange->Consume(answer.PublicKey)) {
			// No entropy, or a public key `Handshake` will not agree with — a
			// low-order point, a reflection of a key it just made. Either way
			// no slot is taken.
			Stats_.Refused++;
			return;
		}

		// Copied before the keys are taken. `TakeKeys` wipes what it can, and
		// reading the message out of a handshake afterwards is relying on which
		// fields it chose to leave alone.
		Welcome welcome;
		const std::span<const std::byte> mine = exchange->Message();
		std::copy(mine.begin(), mine.end(), welcome.PublicKey.begin());

		std::optional<net::Handshake::Session> keys = exchange->TakeKeys();
		if (!keys.has_value()) {
			Stats_.Refused++;
			return;
		}

		// **The exchange proving itself.** An empty frame under the sending key,
		// with both public keys and the cookie as associated data, so a client
		// that derived different keys — because something rewrote a key in
		// flight, or because the two builds derive differently — finds out here
		// instead of accepting a connection whose keys do not match.
		const auto transcript = AdmissionTranscript(answer.PublicKey, welcome.PublicKey, answer.Cookie);
		const std::optional<net::Cipher::Sealed> sealed = keys->Sending.Seal({}, transcript);
		if (!sealed.has_value() || sealed->Bytes.size() != welcome.Confirmation.size()) {
			Stats_.Refused++;
			return;
		}

		welcome.Counter = sealed->Counter;
		std::copy(sealed->Bytes.begin(), sealed->Bytes.end(), welcome.Confirmation.begin());

		// Everything that could refuse has refused. Only now does this peer
		// cost anything: a slot, a session, a link and two reliability windows.
		Peer peer;
		peer.Where = from;
		peer.PublicKey = answer.PublicKey;
		peer.Client = Authority_.Admit();
		peer.Wire = std::make_unique<Session>(
			*Transport_, from, net::ConnectionId{NextConnection++, 1}, nowSeconds, Settings.Session
		);

		// The exchange is done, so the link is. This is the line that used to be
		// unconditional on a stranger's first datagram.
		peer.Wire->Link().CompleteHandshake(nowSeconds);

		// **The ciphers move into the session and stay there for the life of the
		// connection.** The sending half has already sealed the confirmation
		// above, at counter zero, so this connection's first payload goes out at
		// one — the exchange and the stream share one nonce sequence rather than
		// two that could overlap. Nothing here reads a key or a counter; holding
		// the `Sealer` *is* the guarantee, because it is the only object that
		// can produce a nonce under this key and there is no way to make a
		// second one.
		if (!peer.Wire->AdoptKeys(std::move(*keys))) {
			// Only reachable if a session were handed keys twice, which cannot
			// happen for one built four lines ago. Refused rather than carried
			// on with, because the alternative is a connection that is admitted
			// and cannot say anything.
			Authority_.Remove(peer.Client);
			Stats_.Refused++;
			return;
		}

		// Kept, because it cannot be rebuilt: the `Sealer` that produced the tag
		// dies at the end of this function and a second agreement for a live
		// connection is precisely what must not happen. A lost welcome is
		// answered with these same bytes.
		Frame(peer.Welcome, welcome);
		if (peer.Welcome.empty()) {
			Authority_.Remove(peer.Client);
			Stats_.Refused++;
			return;
		}

		Transport_->Send(from, peer.Welcome);

		Peers.push_back(std::move(peer));
		Stats_.Admitted++;

		ENGINE_INFO("replication: admitted {} ({} connected)", from.Text(), Peers.size());
	}

	void Listener::Repeat(Peer &peer, const Admission &message) {
		// The one legitimate reason an admitted peer sends a handshake
		// datagram: its `Welcome` was lost and it is still resending the answer
		// that earned one. Answered with the *same* bytes — a fresh agreement
		// for a live connection is either a replay or somebody at this address
		// trying to take the slot from whoever holds it.
		if (message.Kind != AdmissionKind::Answer || message.Answer.PublicKey != peer.PublicKey) {
			Stats_.Refused++;
			return;
		}

		Transport_->Send(peer.Where, peer.Welcome);
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

			// **Routed by channel before it is routed by sender**, because a
			// peer trying to connect is by definition not in the table yet and
			// a peer already in it has no business starting a second exchange.
			// `PeekChannel` reads nine bytes; the payload is still `Read`'s to
			// check on whichever path takes it.
			if (net::Packet::PeekChannel(Datagram) == net::ChannelKind::Handshake) {
				Greet(inbound.From, Datagram, nowSeconds);
				continue;
			}

			Peer *peer = Find(inbound.From);
			if (peer == nullptr) {
				// **This is the line D00006 was about.** A datagram from an
				// address with no session used to be an admission; now it is a
				// stranger who has not answered a challenge, and a stranger
				// gets nothing allocated for it. Counted rather than logged for
				// the usual reason — a peer sending rubbish at line rate would
				// otherwise write a log line per datagram.
				Stats_.Refused++;
				continue;
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
			const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(peer.Client);
			for (size_t index = 0; index < messages.size(); index++) {
				if (peer.Wire->Send(messages[index], nowSeconds)) {
					continue;
				}

				// A refusal is backpressure rather than an ending — the link is
				// over its per-tick budget or the reliable window is full — but
				// **it is not harmless, and this line is the reason.** The next
				// tick's `Publish` rebuilds a delta from the unconfirmed set
				// and rebuilds nothing else, so a refused snapshot chunk used
				// to be a permanent hole in a stream the receiver waits on for
				// ever: 184 chunks of 192 applied, then every delta after it
				// refused as stale, and a client that joined and never joined.
				// Handing the message back is what lets the authority put its
				// cursor and its known set where they were.
				Authority_.Unsent(peer.Client, index);
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
