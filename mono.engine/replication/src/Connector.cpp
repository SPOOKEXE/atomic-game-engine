#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Connector.hpp>

#include <algorithm>
#include <utility>

namespace engine::replication {

	Connector::Connector(
		net::Transport &transport,
		const net::Endpoint &server,
		double nowSeconds,
		const ConnectorSettings &settings
	)
		: Transport_(&transport),
		  Wire(transport, server, net::ConnectionId{1, 1}, nowSeconds, settings.Session),
		  Prediction_(settings.Prediction), Settings(settings) {
		// **The link stays `Connecting` until the welcome verifies**, which is
		// what stops payload leaving before there is anybody proven to be on the
		// other end. This used to be an unconditional `CompleteHandshake`.
		// **Said once, when the connection is attempted, and it is worth the
		// line.** A client with no pinned identity is safe against a listener
		// and not against a relay, and a mode that weak should be visible in a
		// log rather than inferred from a setting nobody set.
		if (!Settings.ServerIdentity.has_value()) {
			ENGINE_WARN(
				"replication: no server identity pinned — the exchange is encrypted but "
				"authenticates nobody, so a relay in the path can read everything."
			);
		}

		Exchange = net::Handshake::Begin(net::HandshakeRole::Initiator);
		if (!Exchange.has_value()) {
			// No entropy is a refusal to connect, never a fallback to a weaker
			// key. A predictable ephemeral key is not an ephemeral key.
			ENGINE_ERROR("replication: no operating system entropy for a key exchange, so nothing connects.");
			Refuse();
			return;
		}

		const std::span<const std::byte> mine = Exchange->Message();
		std::copy(mine.begin(), mine.end(), Mine.begin());
	}

	void Connector::Refuse() {
		Phase = Stage::Refused;
		Exchange.reset();
		Wire.Link().Close(net::DisconnectReason::HandshakeFailed);
	}

	void Connector::Repeat(double nowSeconds) {
		if (Phase == Stage::Refused || Phase == Stage::Admitted) {
			return;
		}
		if (Wire.Link().State() != net::ConnectionState::Connecting) {
			// `Advance` gave up on the handshake. Saying it again for ever after
			// that is a client shouting at a server that is not answering, on a
			// timer nothing stops — and the link has already recorded
			// `HandshakeFailed` for whoever asks.
			Phase = Stage::Refused;
			Exchange.reset();
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

		// Straight to the transport. `Link::Reserve` refuses everything while
		// the link is `Connecting`, which is correct for payload and is exactly
		// the state this datagram exists to leave — so the budget it would be
		// paced against is not the one that applies. What bounds this is the
		// repeat timer above and the handshake timeout at the other end of it.
		Transport_->Send(Wire.Peer(), datagram.Bytes());
	}

	void Connector::Consume(std::span<const std::byte> datagram, double nowSeconds) {
		if (Phase == Stage::Admitted || Phase == Stage::Refused) {
			// The exchange is over either way. A handshake datagram arriving
			// afterwards is a replay or somebody trying to restart an agreement
			// on a connection that already has one.
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
				// The first cookie only. A second one is either a duplicated
				// datagram or somebody feeding a cookie the server will refuse,
				// and taking it would trade a working answer for one that
				// stalls until the handshake times out.
				Stats_.Refused++;
				return;
			}

			Cookie_ = message.Challenge.Cookie;
			Phase = Stage::Answering;

			// Answered on this poll rather than on the next repeat: the cookie
			// has a lifetime and the round trip has already been paid for.
			Spoken = false;
			Repeat(nowSeconds);
			return;

		case AdmissionKind::Welcome: {
			if (Phase != Stage::Answering || !Exchange.has_value()) {
				Stats_.Refused++;
				return;
			}

			if (!Exchange->Consume(message.Welcome.PublicKey)) {
				// A key this build will not agree with: the wrong length is
				// impossible here, so it is a low-order point, a reflection of
				// our own key, or a second welcome on a spent exchange. All
				// three are terminal in `net::Handshake` and are terminal here.
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

			// **The one check that makes a tampered exchange fail rather than
			// half-succeed.** Both public keys and the cookie are the associated
			// data, so a rewritten key in either direction, or a tag lifted from
			// another exchange, does not open.
			const auto transcript = AdmissionTranscript(Mine, message.Welcome.PublicKey, Cookie_);
			if (!keys->Receiving.Open(message.Welcome.Counter, message.Welcome.Confirmation, transcript)
					 .has_value()) {
				ENGINE_ERROR("replication: the server's key exchange did not verify, so nothing connects.");
				Stats_.Refused++;
				Refuse();
				return;
			}

			// **And who the server is, which the tag above cannot say.** The
			// confirmation proves the sender reached these keys; a relay does
			// too, because it reached them by holding one exchange with each
			// side and reading everything that passes between. A signature over
			// the same transcript proves *which* server reached them, because a
			// relay cannot produce one over a transcript carrying its own
			// ephemeral key that verifies under the pinned identity.
			//
			// **Checked after the tag and before the keys are adopted.** After,
			// because a peer that did not even reach the same keys is a duller
			// failure and deserves the duller message; before, because adopting
			// the keys is the step that makes this a connection.
			if (Settings.ServerIdentity.has_value()) {
				assets::SignatureBytes signature;
				for (size_t index = 0; index < signature.Value.size(); index++) {
					signature.Value[index] = static_cast<uint8_t>(message.Welcome.Identity[index]);
				}

				if (!assets::VerifySessionTranscript(transcript, signature, *Settings.ServerIdentity)) {
					// **Refused rather than downgraded.** A client that pinned a
					// key and then connected anyway would have a setting that
					// looks like security and is not — which is worse than not
					// having the setting, because somebody would rely on it.
					ENGINE_ERROR(
						"replication: this is not the server that was pinned — refusing to connect. "
						"An unsigned or wrongly signed welcome is what a relay in the path looks like."
					);
					Stats_.Refused++;
					Refuse();
					return;
				}
			}

			// **The ciphers move into the session and stay there.** From here
			// every payload in both directions is sealed, and the session
			// refuses to send or accept anything that is not — a client that
			// reached this line and then fell back to plaintext would be doing
			// the downgrade itself.
			//
			// This end's sending half has sealed nothing yet, so its first
			// payload goes out at counter zero; the server's is at one, because
			// the welcome above spent its zero. The two directions have
			// different keys and their counters are unrelated.
			if (!Wire.AdoptKeys(std::move(*keys))) {
				ENGINE_ERROR("replication: the session already held keys, so nothing connects.");
				Stats_.Refused++;
				Refuse();
				return;
			}

			Phase = Stage::Admitted;
			Exchange.reset();
			Wire.Link().CompleteHandshake(nowSeconds);
			return;
		}

		case AdmissionKind::Hello:
		case AdmissionKind::Answer:
			// Client to server only. A server sending one is a server with a
			// bug, or something on the path guessing at the protocol.
			Stats_.Refused++;
			return;
		}

		Stats_.Refused++;
	}

	void Connector::Poll(ecs::Store &store, double nowSeconds) {
		// The client's whole receive path, under one name. Above this there
		// was a single `replication` span in the client's frame loop and
		// nothing at all below it, so a link that had gone bad and a store
		// that had gone slow produced the same reading.
		ENGINE_PROFILE_CAT("replica.poll", core::ProfileCategory::Network);

		// Set here rather than left to the caller, because the set of stores
		// that must not mint *authoritative* entities is exactly the set a
		// connector writes into. An authoritative index minted in a replica is
		// one the server will also hand out, and `Apply` cannot tell the two
		// apart — so this is the difference between a refusal at the call and
		// two different things silently becoming one.
		//
		// `Store::CreatePredicted` stays open, and that is the point: a replica
		// mints from the reserved high range, which the authority never
		// allocates from. See `Store::SetAdoptOnly`.
		if (!store.AdoptOnly()) {
			store.SetAdoptOnly(true);
		}

		for (;;) {
			const net::Transport::Inbound inbound = Transport_->Receive(Datagram);
			if (inbound.Status != net::TransportStatus::Ok) {
				break;
			}

			// The sender is checked rather than trusted. A datagram from
			// somewhere else on an open port is somebody else's traffic or
			// somebody's attempt at injecting into this stream, and the session
			// is the wrong place to find that out — it would have already
			// advanced its acknowledgement window by then.
			if (!(inbound.From == Wire.Peer())) {
				Stats_.Refused++;
				continue;
			}

			if (net::Packet::PeekChannel(Datagram) == net::ChannelKind::Handshake) {
				Consume(Datagram, nowSeconds);
				continue;
			}

			if (Phase != Stage::Admitted) {
				// Nothing but the exchange is expected before the welcome, and
				// nothing the server sends before admitting is applicable.
				Stats_.Refused++;
				continue;
			}

			Wire.Receive(Datagram, nowSeconds);
		}

		if (Phase != Stage::Admitted) {
			// Say it again if the timer says so, and nothing else. There is no
			// world to apply, no tick to acknowledge and a link that refuses
			// payload — the only useful thing left this poll is the exchange.
			Repeat(nowSeconds);
			return;
		}

		for (const std::vector<std::byte> &message : Wire.Inbound()) {
			const ApplyStatus status = Replica_.Receive(store, message);
			if (status == ApplyStatus::Ok) {
				Stats_.Applied++;
			} else {
				// Counted rather than logged per message: a peer sending
				// rubbish at line rate would otherwise write a log line per
				// datagram, which is a denial of service with extra steps.
				Stats_.Refused++;
			}
		}
		Wire.ClearInbound();

		// Sent from here rather than left to the caller. A server stops
		// resending once a client says what it applied, and a client that never
		// says stalls its own stream and is then re-snapshotted for being
		// behind — a failure with a dozen other plausible causes.
		std::vector<std::byte> acknowledgement = Replica_.Acknowledge();

		if (acknowledgement.empty()) {
			// **Not joined yet, so the client still has to keep speaking.** The
			// admission exchange is what told the server where to send, but
			// `Acknowledge` is empty until the snapshot has landed — and a
			// client that then fell silent would be one the keep-alive had to
			// carry, on a link whose idle timeout is the only thing watching.
			//
			// `Applied{0}` rather than a hello of its own: "I have applied
			// nothing" is exactly what a joining client has to say, and a second
			// message kind that meant the same thing is a second thing to keep
			// in step.
			core::ByteWriter writer;
			WriteMessage(writer, replication::Applied{0});
			acknowledgement.assign(writer.Bytes().begin(), writer.Bytes().end());
		}

		Wire.Send(acknowledgement, nowSeconds);

		// Retire what the server has confirmed consuming. What is left is
		// exactly what has to be replayed, which is the whole point of keeping
		// it.
		Prediction_.Reconcile(Replica_.Applied());

		Wire.Flush(nowSeconds);
	}

	void Connector::Advance(double nowSeconds) {
		Wire.Link().Advance(nowSeconds);
		Wire.Link().ResetBudget();
	}

	bool Connector::Submit(uint64_t tick, std::span<const std::byte> bytes, double nowSeconds) {
		// Recorded before it is sent, and recorded even when the send fails.
		// What prediction replays is what the client *did*, and a refused send
		// is exactly the case where the server has not seen it and the replay
		// matters most.
		Prediction_.Record(tick, bytes);

		Input input;
		input.Tick = tick;
		input.Bytes.assign(bytes.begin(), bytes.end());

		core::ByteWriter writer;
		WriteMessage(writer, input);
		return Wire.Send(writer.Bytes(), nowSeconds);
	}
}
