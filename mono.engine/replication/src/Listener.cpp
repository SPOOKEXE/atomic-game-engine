#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Listener.hpp>

#include <algorithm>
#include <utility>

namespace engine::replication {

	namespace {
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
			ENGINE_ERROR(
				"replication: no operating system entropy for the admission challenge, so this listener "
				"admits nobody."
			);
		}

		Authority_.SetIdentityCheck([this](ClientId client, const Identify &claim) {
			Peer *peer = nullptr;
			for (Peer &candidate : Peers) {
				if (candidate.Client == client) {
					peer = &candidate;
					break;
				}
			}
			if (peer == nullptr) {
				return false;
			}

			if (!assets::VerifySessionTranscript(peer->Transcript, claim.Signature, claim.Key)) {
				ENGINE_WARN("replication: a client's identity claim did not verify - dropping it.");
				Stats_.Refused++;
				return false;
			}

			if (ClientPolicy && !ClientPolicy(client, claim.Key)) {
				ENGINE_INFO("replication: a client proved an identity the game does not admit.");
				Stats_.Rejected++;
				return false;
			}

			peer->Identity = claim.Key;
			return true;
		});

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

	void Listener::SetIdentity(const assets::SigningKey *key) {
		Identity = key;
	}

	void Listener::SetClientPolicy(std::function<bool(ClientId, const assets::PublicKey &)> policy) {
		ClientPolicy = std::move(policy);
	}

	void Listener::RequireClientIdentity(bool required) {
		RequireIdentity = required;
	}

	std::optional<assets::PublicKey> Listener::IdentityOf(ClientId client) const {
		for (const Peer &peer : Peers) {
			if (peer.Client == client) {
				return peer.Identity;
			}
		}
		return std::nullopt;
	}

	void Listener::SetAdmission(AdmissionPolicy policy) {
		Policy = std::move(policy);
	}

	void Listener::OnUserMessage(std::function<void(ClientId, std::span<const std::byte>)> handler) {
		UserMessages = std::move(handler);
	}

	bool Listener::SendTo(ClientId client, std::span<const std::byte> message, double nowSeconds) {
		for (Peer &peer : Peers) {
			if (!(peer.Client == client) || peer.Wire == nullptr) {
				continue;
			}
			core::ByteWriter writer;
			User payload;
			payload.Bytes.assign(message.begin(), message.end());
			WriteMessage(writer, payload);
			return peer.Wire->Send(writer.Bytes(), nowSeconds);
		}
		return false;
	}

	size_t Listener::Broadcast(std::span<const std::byte> message, double nowSeconds, ClientId except) {
		// Encoded once for everybody. The envelope is the same bytes whoever it
		// goes to, and re-encoding per client would be the same work done once
		// per person in the session.
		core::ByteWriter writer;
		User payload;
		payload.Bytes.assign(message.begin(), message.end());
		WriteMessage(writer, payload);

		size_t taken = 0;
		for (Peer &peer : Peers) {
			if (peer.Wire == nullptr || peer.Client == except) {
				continue;
			}
			if (peer.Wire->Send(writer.Bytes(), nowSeconds)) {
				taken++;
			}
		}
		return taken;
	}

	void
	Listener::SetForeign(std::function<bool(std::span<const std::byte>, const net::Endpoint &)> handler) {
		Foreign = std::move(handler);
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

		if (!Cookie_->Answers(nowSeconds, from, answer.PublicKey, answer.Cookie)) {
			Stats_.Refused++;
			return;
		}

		if (Peers.size() >= Settings.MaximumClients) {
			Stats_.Turned++;
			return;
		}

		if (Policy && !Policy(Applicant{from, Peers.size(), nowSeconds})) {
			Stats_.Rejected++;
			return;
		}

		std::optional<net::Handshake> exchange = net::Handshake::Begin(net::HandshakeRole::Responder);
		if (!exchange.has_value() || !exchange->Consume(answer.PublicKey)) {
			Stats_.Refused++;
			return;
		}

		Welcome welcome;
		const std::span<const std::byte> mine = exchange->Message();
		std::copy(mine.begin(), mine.end(), welcome.PublicKey.begin());

		std::optional<net::Handshake::Session> keys = exchange->TakeKeys();
		if (!keys.has_value()) {
			Stats_.Refused++;
			return;
		}

		const auto transcript = AdmissionTranscript(answer.PublicKey, welcome.PublicKey, answer.Cookie);
		const std::optional<net::Cipher::Sealed> sealed = keys->Sending.Seal({}, transcript);
		if (!sealed.has_value() || sealed->Bytes.size() != welcome.Confirmation.size()) {
			Stats_.Refused++;
			return;
		}

		welcome.Counter = sealed->Counter;
		std::copy(sealed->Bytes.begin(), sealed->Bytes.end(), welcome.Confirmation.begin());

		if (Identity != nullptr) {
			const assets::SignatureBytes signature = Identity->SignSessionTranscript(transcript);
			for (size_t index = 0; index < signature.Value.size(); index++) {
				welcome.Identity[index] = static_cast<std::byte>(signature.Value[index]);
			}
		}

		Peer peer;
		peer.Where = from;
		peer.PublicKey = answer.PublicKey;
		peer.Transcript = transcript;
		peer.Client = Authority_.Admit();
		peer.Wire = std::make_unique<Session>(
			*Transport_, from, net::ConnectionId{NextConnection++, 1}, nowSeconds, Settings.Session
		);

		peer.Wire->Link().CompleteHandshake(nowSeconds);

		if (!peer.Wire->AdoptKeys(std::move(*keys))) {
			Authority_.Remove(peer.Client);
			Stats_.Refused++;
			return;
		}

		Frame(peer.Welcome, welcome);
		if (peer.Welcome.empty()) {
			Authority_.Remove(peer.Client);
			Stats_.Refused++;
			return;
		}

		Transport_->Send(from, peer.Welcome);

		const ClientId admitted = peer.Client;
		Peers.push_back(std::move(peer));
		Stats_.Admitted++;

		ENGINE_INFO("replication: admitted {} ({} connected)", from.Text(), Peers.size());

		// After the peer is in the list, so a handler that asks this listener
		// about the client it was just handed gets an answer.
		if (Admitted_) {
			Admitted_(admitted);
		}
	}

	void Listener::Repeat(Peer &peer, const Admission &message) {
		if (message.Kind != AdmissionKind::Answer || message.Answer.PublicKey != peer.PublicKey) {
			Stats_.Refused++;
			return;
		}

		Transport_->Send(peer.Where, peer.Welcome);
	}

	void Listener::Drop(size_t index) {
		// **Before the handle is retired**, so a host can still use it to find
		// whatever it hung off this client - the `Player` instance, most
		// obviously, which has to be destroyed rather than leaked.
		if (Dropped_) {
			Dropped_(Peers[index].Client);
		}

		Authority_.Remove(Peers[index].Client);

		ENGINE_INFO("replication: dropped {} ({} connected)", Peers[index].Where.Text(), Peers.size() - 1);

		Peers[index] = std::move(Peers.back());
		Peers.pop_back();
		Stats_.Dropped++;
	}

	void Listener::OnAdmitted(std::function<void(ClientId)> handler) {
		Admitted_ = std::move(handler);
	}

	void Listener::OnDropped(std::function<void(ClientId)> handler) {
		Dropped_ = std::move(handler);
	}

	void Listener::ApplyOwnedState(ecs::Store &store) {
		for (const Peer &peer : Peers) {
			Authority_.ApplySubmitted(peer.Client, store);
		}
	}

	void Listener::Poll(double nowSeconds) {
		ENGINE_PROFILE_CAT("Listener::Poll", core::ProfileCategory::Network);

		for (;;) {
			const net::Transport::Inbound inbound = Transport_->Receive(Datagram);
			if (inbound.Status != net::TransportStatus::Ok) {
				break;
			}

			// Before the handshake check and before the source check. A
			// rendezvous message comes from a coordination point this listener
			// has never heard of and would otherwise be counted as a refusal.
			if (Foreign && Foreign(Datagram, inbound.From)) {
				continue;
			}

			if (net::Packet::PeekChannel(Datagram) == net::ChannelKind::Handshake) {
				Greet(inbound.From, Datagram, nowSeconds);
				continue;
			}

			Peer *peer = Find(inbound.From);
			if (peer == nullptr) {
				Stats_.Refused++;
				continue;
			}

			peer->Wire->Receive(Datagram, nowSeconds);

			for (const std::vector<std::byte> &message : peer->Wire->Inbound()) {
				// **Routed before the authority sees it.** A user message is
				// not this module's, and handing one to `Authority::Receive`
				// parses fine and then falls off the end of its switch - so the
				// message would look delivered while a refusal counter an
				// operator reads climbed.
				if (PeekMessageKind(message) == MessageKind::User) {
					if (UserMessages) {
						core::ByteReader reader(message);
						Message read;
						if (ReadMessage(reader, read)) {
							UserMessages(peer->Client, read.User.Bytes);
						}
					}
					continue;
				}
				Authority_.Receive(peer->Client, message);
			}
			peer->Wire->ClearInbound();
		}
	}

	size_t Listener::Flush(double nowSeconds) {
		size_t busy = 0;
		for (Peer &peer : Peers) {
			if (peer.Wire != nullptr && peer.Wire->Flush(nowSeconds) > 0) {
				busy++;
			}
		}
		return busy;
	}

	void Listener::Advance(double nowSeconds) {
		ENGINE_PROFILE_CAT("Listener::Advance", core::ProfileCategory::Network);

		for (size_t index = Peers.size(); index > 0; index--) {
			Peer &peer = Peers[index - 1];
			peer.Wire->Link().Advance(nowSeconds);
			peer.Wire->Link().ResetBudget();

			// **Right after `ResetBudget`, because that is where the link
			// decides the number.** The allowance is a function of one tick's
			// worth of observation, and reading it anywhere else in the tick
			// reads whatever is left of it rather than what it was.
			//
			// No reordering was needed to make this current: `Advance` runs
			// after `Publish`, so what the next `Publish` reads is the allowance
			// this tick's observations produced. See `Authority::SetAllowance`.
			Authority_.SetAllowance(
				peer.Client, static_cast<size_t>(peer.Wire->Link().Stats().SendAllowanceBytes)
			);

			if (peer.Wire->Link().State() == net::ConnectionState::Disconnected) {
				Drop(index - 1);
			}
		}
	}

	void Listener::Publish(ecs::Store &store, uint64_t tick, double nowSeconds) {
		Authority_.Publish(store, tick);

		// **One span for every peer's sending rather than one per peer.**
		// `FrameGraph::MAXIMUM_SPANS` is 4096 and a span per client would spend
		// most of it on a two-hundred-client host, so the frame that a
		// flamegraph is of would be the frame the instrumentation overflowed.
		// What a reader wants here is the split between building a tick and
		// putting it on the wire, and that is two spans whatever the client
		// count.
		ENGINE_PROFILE_CAT("Listener::Send", core::ProfileCategory::Network);

		for (Peer &peer : Peers) {
			// **The identity gate, and it is here rather than in `Accept`.** A
			// claim arrives *after* admission - `SetIdentityCheck` is what fills
			// `Peer::Identity`, and it runs on a message the client sends once
			// the session exists. Refusing at admission would refuse everybody,
			// because nobody has proved anything yet at that point.
			//
			// So the gate is on what a peer is *given*: an unidentified client
			// holds a session and receives no world state. It still ages out
			// through `Advance`'s ordinary link timeout if it never identifies,
			// which is why this needs no deadline of its own.
			if (RequireIdentity && !peer.Identity.has_value()) {
				peer.Wire->Flush(nowSeconds);
				continue;
			}

			const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(peer.Client);
			for (size_t index = 0; index < messages.size(); index++) {
				if (peer.Wire->Send(messages[index], nowSeconds)) {
					continue;
				}

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

	float Listener::RoundTripMilliseconds(ClientId client) const {
		for (const Peer &peer : Peers) {
			if (peer.Client == client && peer.Wire != nullptr) {
				return peer.Wire->Link().Stats().RoundTripMilliseconds;
			}
		}
		return 0.0f;
	}

	void Listener::ClearInputs() {
		for (const Peer &peer : Peers) {
			Authority_.ClearInputs(peer.Client);
		}
	}
}
