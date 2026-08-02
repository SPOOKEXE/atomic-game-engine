#include <engine/core/Log.hpp>
#include <engine/replication/Connector.hpp>

namespace engine::replication {

	Connector::Connector(
		net::Transport &transport,
		const net::Endpoint &server,
		double nowSeconds,
		const ConnectorSettings &settings
	)
		: Transport_(&transport),
		  Wire(transport, server, net::ConnectionId{1, 1}, nowSeconds, settings.Session),
		  Prediction_(settings.Prediction) {
		// No handshake, matching the listener — see the note at the top of
		// `Listener.hpp` on what that leaves open. A link left `Connecting`
		// refuses payload, so this is what lets the first datagram out, and it
		// is the line a real key exchange replaces.
		Wire.Link().CompleteHandshake(nowSeconds);
	}

	void Connector::Poll(ecs::Store &store, double nowSeconds) {
		// Set here rather than left to the caller, because the set of stores
		// that must not mint entities is exactly the set a connector writes
		// into. An entity minted in a replica takes an index the authority will
		// also hand out, and `Apply` cannot tell the two apart — so this is the
		// difference between a refusal at the call and two different things
		// silently becoming one. See `Store::SetAdoptOnly`.
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

			Wire.Receive(Datagram, nowSeconds);
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
			// **Not joined yet, so the client has to speak first.** A server on
			// a datagram socket cannot send to an address it has never heard
			// from, and `Acknowledge` is empty until the snapshot has landed —
			// so a client that only ever replied would sit silent forever
			// waiting for a stream that had nowhere to go. This was invisible in
			// a suite whose client submitted an input on every tick.
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
