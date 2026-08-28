#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Wire.hpp>
#include <engine/replication/Connector.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace engine::replication {

	Connector::Connector(
		net::Transport &transport,
		const net::Endpoint &server,
		double nowSeconds,
		const ConnectorSettings &settings
	)
		: Transport_(&transport), Server(server), Prediction_(settings.Prediction), Settings(settings) {
		if (!Settings.ServerIdentity.has_value()) {
			ENGINE_WARN(
				"replication: no server identity pinned - the exchange is encrypted but "
				"authenticates nobody, so a relay in the path can read everything."
			);
		}

		// **QUIC first, always, and no flag says otherwise.** The only thing
		// that moves the first attempt is an advert from the server itself
		// saying it does not serve QUIC, which saves the refusal round trip and
		// changes nothing else.
		const bool datagramOnly =
			Settings.Advertised.has_value() && !net::Serves(*Settings.Advertised, net::WireKind::Quic);
		Begin(datagramOnly ? net::WireKind::Datagram : net::WireKind::Quic, nowSeconds);
	}

	void Connector::Begin(net::WireKind wire, double nowSeconds) {
		Attempting = wire;
		AttemptStartedAt = nowSeconds;
		Tried++;

		// Everything an attempt accumulated goes with it. A half-finished key
		// exchange carried into the next attempt is a state machine two
		// attempts can be in at once, which is the shape a reconnect is banned
		// from having for the same reason.
		Phase = Stage::Greeting;
		Turned = false;
		Spoken = false;
		Cookie_ = {};
		Mine = {};
		Exchange.reset();
		Quic = nullptr;
		Port.reset();

		if (wire == net::WireKind::Quic) {
			// The pin is stated once by the caller and lands wherever the wire
			// needs it. Under QUIC that is the RFC 7250 raw public key the TLS
			// handshake checks against the server's CertificateVerify.
			QuicSessionSettings quic = Settings.Quic;
			quic.Connection.Tls.PinIdentity = Settings.ServerIdentity.has_value();
			if (Settings.ServerIdentity.has_value()) {
				for (size_t index = 0; index < net::quic::IDENTITY_BYTES; index++) {
					quic.Connection.Tls.Expected[index] =
						static_cast<std::byte>(Settings.ServerIdentity->Value[index]);
				}
			}

			std::unique_ptr<QuicSession> session =
				QuicSession::Connect(*Transport_, Server, nowSeconds, quic);
			if (session == nullptr) {
				ENGINE_ERROR("replication: a QUIC connection could not be opened, so nothing connects.");
				Phase = Stage::Refused;
				return;
			}
			Quic = session.get();
			Port = std::move(session);
			return;
		}

		Port = std::make_unique<Session>(
			*Transport_, Server, net::ConnectionId{1, 1}, nowSeconds, Settings.Session
		);

		Exchange = net::Handshake::Begin(net::HandshakeRole::Initiator);
		if (!Exchange.has_value()) {
			ENGINE_ERROR("replication: no operating system entropy for a key exchange, so nothing connects.");
			Refuse();
			return;
		}

		const std::span<const std::byte> mine = Exchange->Message();
		std::copy(mine.begin(), mine.end(), Mine.begin());
	}

	bool Connector::Fallback(double nowSeconds, const char *why) {
		const bool exhausted = Attempting == net::WireKind::Datagram || Tried >= MAXIMUM_ATTEMPTS;
		const bool pointless =
			Settings.Advertised.has_value() && !net::Serves(*Settings.Advertised, net::WireKind::Datagram);

		if (exhausted || pointless) {
			ENGINE_ERROR(
				"replication: could not connect over {} ({}); {} attempt(s) made and no transport left.",
				net::Describe(Attempting),
				why,
				Tried
			);
			Refuse();
			return false;
		}

		// **Said out loud, and it says which and why.** A client that quietly
		// changed transport is one whose operator cannot tell a server running
		// the old stack from a QUIC handshake that is failing for its own
		// reasons.
		ENGINE_INFO(
			"replication: {} did not connect ({}) - retrying over the datagram wire.",
			net::Describe(Attempting),
			why
		);
		Begin(net::WireKind::Datagram, nowSeconds);
		return true;
	}

	void Connector::Reconsider(double nowSeconds) {
		if (Phase == Stage::Admitted || Phase == Stage::Refused) {
			return;
		}

		// **An explicit refusal costs one round trip and a silence costs the
		// deadline**, which is the whole reason the server answers at all.
		if (Turned || (Quic != nullptr && Quic->Refused())) {
			Fallback(nowSeconds, "the server does not serve this transport");
			return;
		}

		if (Port == nullptr || !Port->Live()) {
			Fallback(nowSeconds, "the link ended before the handshake did");
			return;
		}

		if (nowSeconds - AttemptStartedAt >= Settings.AttemptSeconds) {
			Fallback(nowSeconds, "no answer");
		}
	}

	void Connector::Landed() {
		ENGINE_INFO(
			"replication: connected over {} on attempt {} of {}",
			net::Describe(Attempting),
			Tried,
			MAXIMUM_ATTEMPTS
		);
	}

	void Connector::Settle(double nowSeconds) {
		// **The QUIC admission, which is the handshake finishing and nothing
		// else.** There is no hello, no cookie and no welcome: the address
		// validation is Retry's, the key exchange is TLS 1.3's, and the pinned
		// identity was checked inside the handshake rather than after it.
		if (Phase != Stage::Greeting || !Port->Carrying()) {
			return;
		}

		Phase = Stage::Admitted;
		Landed();

		if (Settings.ClientIdentity == nullptr) {
			return;
		}

		// The claim is signed over a value derived from this connection and no
		// other, so a signature captured here proves nothing anywhere else.
		std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES> binding{};
		if (!Port->Binding(binding)) {
			ENGINE_WARN("replication: no binding to sign an identity over - not claiming one.");
			return;
		}

		Identify claim;
		claim.Key = Settings.ClientIdentity->Public();
		claim.Signature = Settings.ClientIdentity->SignSessionTranscript(binding);

		core::ByteWriter writer;
		WriteMessage(writer, claim);
		if (!Port->Send(writer.Bytes(), nowSeconds)) {
			ENGINE_WARN("replication: the identity claim did not fit - the server may refuse us.");
		}
	}

	void
	Connector::SetForeign(std::function<bool(std::span<const std::byte>, const net::Endpoint &)> handler) {
		Foreign = std::move(handler);
	}

	void Connector::OnUserMessage(std::function<void(std::span<const std::byte>)> handler) {
		UserMessages = std::move(handler);
	}

	bool Connector::SendUser(std::span<const std::byte> message, double nowSeconds) {
		if (Phase != Stage::Admitted) {
			// No session to carry it on. Refused rather than queued: an outbox
			// here would hold payloads whose meaning this module is not allowed
			// to understand, which is the same reason `net` keeps none.
			return false;
		}

		core::ByteWriter writer;
		User payload;
		payload.Bytes.assign(message.begin(), message.end());
		WriteMessage(writer, payload);
		return Port->Send(writer.Bytes(), nowSeconds);
	}

	void Connector::Refuse() {
		Phase = Stage::Refused;
		Exchange.reset();
		if (Port == nullptr) {
			return;
		}
		if (Quic != nullptr) {
			Port->Disconnect(0.0);
			return;
		}
		// **`HandshakeFailed` and not `Requested`, and the distinction is the
		// whole of what `net/AGENTS.md` asks a lifecycle to preserve**: a peer
		// that left politely must be distinguishable from one that never got in.
		// `SessionPort::Disconnect` carries no reason on purpose - QUIC's is an
		// application error code and `net`'s is an enum - so the one caller that
		// needs the distinction reaches for the concrete type it already has.
		static_cast<Session *>(Port.get())->Link().Close(net::DisconnectReason::HandshakeFailed);
	}

	void Connector::Repeat(double nowSeconds) {
		if (Phase == Stage::Refused || Phase == Stage::Admitted) {
			return;
		}
		if (Spoken && nowSeconds - SpokeAt < Settings.RepeatEverySeconds) {
			return;
		}

		Spoken = true;
		SpokeAt = nowSeconds;

		core::ByteWriter body;
		if (Phase == Stage::Greeting) {
			Hello hello;
			hello.PublicKey = Mine;
			WriteAdmission(body, hello);
		} else {
			Answer answer;
			answer.PublicKey = Mine;
			answer.Cookie = Cookie_;
			WriteAdmission(body, answer);
		}

		core::ByteWriter datagram;
		if (!FrameAdmission(datagram, body.Bytes())) {
			return;
		}

		Transport_->Send(Server, datagram.Bytes());
	}

	void Connector::Consume(std::span<const std::byte> datagram, double nowSeconds) {
		if (Phase == Stage::Admitted || Phase == Stage::Refused) {
			Stats_.Refused++;
			return;
		}

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

		switch (message.Kind) {
		case AdmissionKind::Challenge:
			if (Phase != Stage::Greeting) {
				Stats_.Refused++;
				return;
			}

			Cookie_ = message.Challenge.Cookie;
			Phase = Stage::Answering;

			Spoken = false;
			Repeat(nowSeconds);
			return;

		case AdmissionKind::Welcome: {
			if (Phase != Stage::Answering || !Exchange.has_value()) {
				Stats_.Refused++;
				return;
			}

			if (!Exchange->Consume(message.Welcome.PublicKey)) {
				Stats_.Refused++;
				Refuse();
				return;
			}

			std::optional<net::Handshake::Session> keys = Exchange->TakeKeys();
			if (!keys.has_value()) {
				Stats_.Refused++;
				Refuse();
				return;
			}

			const auto transcript = AdmissionTranscript(Mine, message.Welcome.PublicKey, Cookie_);
			static_cast<Session *>(Port.get())->SetBinding(transcript);
			if (!keys->Receiving.Open(message.Welcome.Counter, message.Welcome.Confirmation, transcript)
					 .has_value()) {
				ENGINE_ERROR("replication: the server's key exchange did not verify, so nothing connects.");
				Stats_.Refused++;
				Refuse();
				return;
			}

			if (Settings.ServerIdentity.has_value()) {
				assets::SignatureBytes signature;
				for (size_t index = 0; index < signature.Value.size(); index++) {
					signature.Value[index] = static_cast<uint8_t>(message.Welcome.Identity[index]);
				}

				if (!assets::VerifySessionTranscript(transcript, signature, *Settings.ServerIdentity)) {
					ENGINE_ERROR(
						"replication: this is not the server that was pinned - refusing to connect. "
						"An unsigned or wrongly signed welcome is what a relay in the path looks like."
					);
					Stats_.Refused++;
					Refuse();
					return;
				}
			}

			if (!static_cast<Session *>(Port.get())->AdoptKeys(std::move(*keys))) {
				ENGINE_ERROR("replication: the session already held keys, so nothing connects.");
				Stats_.Refused++;
				Refuse();
				return;
			}

			Phase = Stage::Admitted;
			Exchange.reset();
			static_cast<Session *>(Port.get())->Link().CompleteHandshake(nowSeconds);
			Landed();

			if (Settings.ClientIdentity != nullptr) {
				Identify claim;
				claim.Key = Settings.ClientIdentity->Public();
				claim.Signature = Settings.ClientIdentity->SignSessionTranscript(transcript);

				core::ByteWriter writer;
				WriteMessage(writer, claim);
				if (!Port->Send(writer.Bytes(), nowSeconds)) {
					ENGINE_WARN("replication: the identity claim did not fit - the server may refuse us.");
				}
			}
			return;
		}

		case AdmissionKind::Refuse:
			// **The server saying which stack it does serve.** Acted on rather
			// than counted: the alternative is waiting out `AttemptSeconds` to
			// learn what this datagram already said.
			ENGINE_INFO(
				"replication: the server refused the datagram wire and serves {}.",
				net::Describe(message.Refusal.Wire)
			);
			Turned = true;
			return;

		case AdmissionKind::Hello:
		case AdmissionKind::Answer:
			Stats_.Refused++;
			return;
		}

		Stats_.Refused++;
	}

	void Connector::Poll(ecs::Store &store, double nowSeconds) {
		ENGINE_PROFILE_CAT("replica.poll", core::ProfileCategory::Network);

		if (!store.AdoptOnly()) {
			store.SetAdoptOnly(true);
		}

		// A connector whose first attempt could not even be built. Terminal, so
		// there is nothing to drain and nothing to fall back to.
		if (Port == nullptr) {
			return;
		}

		for (;;) {
			const net::Transport::Inbound inbound = Transport_->Receive(Datagram);
			if (inbound.Status != net::TransportStatus::Ok) {
				break;
			}

			// Before the source check: a rendezvous message arrives from a
			// coordination point rather than from the server, and would
			// otherwise be counted as a refusal.
			if (Foreign && Foreign(Datagram, inbound.From)) {
				continue;
			}

			if (!(inbound.From == Server)) {
				Stats_.Refused++;
				continue;
			}

			if (Attempting == net::WireKind::Quic) {
				// Including a Version Negotiation packet, which is how a server
				// that serves the other stack says so. `QuicSession::Refused`
				// is what `Reconsider` reads afterwards.
				Port->Receive(Datagram, nowSeconds);
				continue;
			}

			if (net::Packet::PeekChannel(Datagram) == net::ChannelKind::Handshake) {
				Consume(Datagram, nowSeconds);
				continue;
			}

			if (Phase != Stage::Admitted) {
				Stats_.Refused++;
				continue;
			}

			Port->Receive(Datagram, nowSeconds);
		}

		// **Before the fallback is considered.** A QUIC handshake commonly
		// finishes on the poll its last flight arrived in, and a connector that
		// asked "should I fall back" first would fall back off a deadline it had
		// already met.
		if (Attempting == net::WireKind::Quic) {
			Settle(nowSeconds);
		}

		Reconsider(nowSeconds);

		if (Phase != Stage::Admitted) {
			if (Phase != Stage::Refused) {
				if (Attempting == net::WireKind::Quic) {
					// Nothing to repeat: the handshake retransmits itself off
					// its own timers, which is what `Flush` drives.
					Port->Flush(nowSeconds);
				} else {
					Repeat(nowSeconds);
				}
			}
			return;
		}

		for (const std::vector<std::byte> &message : Port->Inbound()) {
			// Routed before the replica sees it, for `Listener::Poll`'s reason:
			// a user message is not this module's, and handing one to the
			// replica would count a failure for a message that arrived
			// perfectly well.
			if (PeekMessageKind(message) == MessageKind::User) {
				if (UserMessages) {
					core::ByteReader reader(message);
					Message read;
					if (ReadMessage(reader, read)) {
						UserMessages(read.User.Bytes);
					}
				}
				continue;
			}

			const ApplyStatus status = Replica_.Receive(store, message);
			if (status == ApplyStatus::Ok) {
				Stats_.Applied++;
			} else {
				Stats_.Refused++;
			}
		}
		Port->ClearInbound();

		std::vector<std::byte> acknowledgement = Replica_.Acknowledge();

		if (acknowledgement.empty()) {
			core::ByteWriter writer;
			WriteMessage(writer, replication::Applied{0});
			acknowledgement.assign(writer.Bytes().begin(), writer.Bytes().end());
		}

		Port->Send(acknowledgement, nowSeconds);

		// After the acknowledgement, because it is the message the server needs
		// and this is the message it can wait a tick for.
		if (const std::vector<std::byte> dispute = Replica_.Dispute(); !dispute.empty()) {
			Port->Send(dispute, nowSeconds);
		}

		Prediction_.Reconcile(Replica_.Applied());

		Port->Flush(nowSeconds);
	}

	net::Link *Connector::Link() {
		return Quic != nullptr || Port == nullptr ? nullptr : &static_cast<Session *>(Port.get())->Link();
	}

	net::ConnectionStats Connector::LinkStats() const {
		if (Port == nullptr) {
			return {};
		}
		if (Quic == nullptr) {
			return static_cast<const Session *>(Port.get())->Link().Stats();
		}

		// Refilled from ngtcp2 rather than restated, which is why
		// `ConnectionStats` survives the swap: a panel reads
		// the same fields whichever transport is underneath.
		const net::quic::Connection::Statistics stats = Quic->Stats();
		net::ConnectionStats out;
		out.RoundTripMilliseconds = static_cast<float>(stats.RoundTripMilliseconds);
		out.PacketsLost = stats.PacketsLost;
		out.PacketsSent = stats.Sent;
		// **`SendsOverBudget` keeps its meaning and gains no second one.** It is
		// a number somebody configured being enforced, so what fills it here is
		// the ceiling refusing rather than the path refusing - the distinction
		// `D00007`'s reopen trigger and `render`'s panel are both phrased
		// against.
		out.SendsOverBudget = stats.DatagramsRefused;
		return out;
	}

	void Connector::Advance(double nowSeconds) {
		if (Port == nullptr) {
			return;
		}
		Port->Advance(nowSeconds);
		if (Attempting == net::WireKind::Quic) {
			// A QUIC handshake can finish on a tick where nothing arrived,
			// because its last flight is driven by its own timers.
			Settle(nowSeconds);
		}
	}

	bool Connector::Submit(uint64_t tick, std::span<const std::byte> bytes, double nowSeconds) {
		Prediction_.Record(tick, bytes);

		if (Port == nullptr) {
			return false;
		}

		Input input;
		input.Tick = tick;
		input.Bytes.assign(bytes.begin(), bytes.end());

		core::ByteWriter writer;
		WriteMessage(writer, input);
		return Port->Send(writer.Bytes(), nowSeconds);
	}

	bool Connector::SubmitState(const Delta &delta, double nowSeconds) {
		// **The same message the server sends, going the other way**, which is
		// the whole reason this is four lines: a delta is a delta, and the
		// direction is carried by which end is reading it rather than by the
		// format. What differs is that the server checks the sender's right to
		// say it - see `Authority::SetOwnership` - and the client does not,
		// because the server has no right to check.
		if (Port == nullptr) {
			return false;
		}

		core::ByteWriter writer;
		WriteMessage(writer, delta);
		return Port->Send(writer.Bytes(), nowSeconds);
	}
}
