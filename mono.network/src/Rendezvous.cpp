#include "Codec.hpp"

#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <array>
#include <cryptopp/osrng.h>
#include <network/Rendezvous.hpp>
#include <utility>

// The message set, and both ends of it, in one file.
//
// **One file on purpose.** A wire format written in two places is a format that
// grows a dialect - one side gains a field, the other keeps reading the old
// shape - and the failure is invisible until two builds meet. The point and the
// client are small enough that keeping them together costs nothing and removes
// the only way this could drift.

namespace network {

	namespace {
		// The frame's magic. `ATN1` is a packet, `ATNA` is an advert, and this
		// is `ATNR` - three formats and three first words, which is what lets a
		// program share one socket between them. See `IsRendezvousMessage`.
		constexpr uint32_t RENDEZVOUS_MAGIC = 0x524E5441; // "ATNR"

		// The frame version, refused when unknown.
		constexpr uint16_t RENDEZVOUS_VERSION = 1;

		// Magic, version, kind.
		constexpr size_t HEADER_BYTES = 4 + 2 + 1;

		// What one message is.
		//
		// A closed list whose ordinals reach a wire: append only.
		enum class MessageKind : uint8_t {
			// Host to point: hold this session, and tell me how you saw me.
			Register = 1,

			// Point to host: held, and here is your reflexive address.
			Enrolled = 2,

			// Client to point: what public sessions do you hold?
			Browse = 3,

			// Point to client: these, with the addresses I saw them from.
			Listed = 4,

			// Client to point: introduce me to this session.
			Reach = 5,

			// Point to both: here is the other side, and the nonce for this
			// meeting.
			Punch = 6,

			// Point to client: I do not hold that session.
			Unknown = 7,

			// Host to point: forget this session.
			Withdraw = 8,

			// Peer to peer: the datagram that opens a router's mapping.
			Poke = 9,
		};

		// The largest a browse reply's advert may be before it is skipped, so a
		// reply cannot be pushed past one datagram by a registrant with two
		// long strings. Derived from the datagram rather than picked.
		constexpr size_t MAXIMUM_ADVERT_BYTES = 512;

		void WriteHeader(engine::core::ByteWriter &writer, MessageKind kind) {
			writer.WriteUInt32(RENDEZVOUS_MAGIC);
			writer.WriteUInt16(RENDEZVOUS_VERSION);
			writer.WriteUInt8(static_cast<uint8_t>(kind));
		}

		// Reads and checks the frame, refusing anything this build cannot
		// parse.
		std::optional<MessageKind> ReadHeader(engine::core::ByteReader &reader) {
			if (reader.ReadUInt32() != RENDEZVOUS_MAGIC || reader.ReadUInt16() != RENDEZVOUS_VERSION) {
				return std::nullopt;
			}
			const uint8_t kind = reader.ReadUInt8();
			if (reader.Failed() || kind < static_cast<uint8_t>(MessageKind::Register) ||
				kind > static_cast<uint8_t>(MessageKind::Poke)) {
				return std::nullopt;
			}
			return static_cast<MessageKind>(kind);
		}

		// A length-prefixed block of opaque bytes - an advert, repeated
		// unchanged so that a point neither re-encodes one nor invalidates a
		// tag it cannot verify.
		void WriteBlock(engine::core::ByteWriter &writer, std::span<const std::byte> bytes) {
			writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
			writer.WriteRaw(bytes.data(), bytes.size());
		}

		std::span<const std::byte> ReadBlock(engine::core::ByteReader &reader) {
			const uint32_t bytes = reader.ReadUInt32();
			if (reader.Failed() || bytes > MAXIMUM_ADVERT_BYTES) {
				return {};
			}
			return reader.ReadRawView(bytes);
		}

		void WriteNonce(
			engine::core::ByteWriter &writer, const std::array<std::byte, MEETING_NONCE_BYTES> &nonce
		) {
			writer.WriteRaw(nonce.data(), nonce.size());
		}

		std::array<std::byte, MEETING_NONCE_BYTES> ReadNonce(engine::core::ByteReader &reader) {
			std::array<std::byte, MEETING_NONCE_BYTES> nonce{};
			if (!reader.ReadRaw(nonce.data(), nonce.size())) {
				return {};
			}
			return nonce;
		}

		// A meeting's nonce, from the operating system.
		//
		// **What it stops is a replayed poke.** Without it a poke recorded from
		// one meeting opens the next one between the same two peers, and the
		// key would be proving possession of a recording rather than of a
		// secret. A zero nonce is what a machine with no entropy gets, and it
		// is still better than a counter - it fails closed against everything
		// except a replay, where a counter fails open against a guess.
		std::array<std::byte, MEETING_NONCE_BYTES> DrawNonce() {
			std::array<std::byte, MEETING_NONCE_BYTES> nonce{};
			try {
				CryptoPP::OS_GenerateRandomBlock(
					false, reinterpret_cast<CryptoPP::byte *>(nonce.data()), nonce.size()
				);
			} catch (const CryptoPP::Exception &) {
				return {};
			}
			return nonce;
		}
	}

	const char *Describe(ReachState state) {
		switch (state) {
		case ReachState::Idle:
			return "idle";
		case ReachState::Locating:
			return "locating";
		case ReachState::Punching:
			return "punching";
		case ReachState::Reached:
			return "reached";
		case ReachState::Failed:
			return "failed";
		}
		// No default label, so adding a state is a warning here.
		return "?";
	}

	bool IsRendezvousMessage(std::span<const std::byte> datagram) {
		if (datagram.size() < HEADER_BYTES) {
			return false;
		}
		engine::core::ByteReader reader(datagram);
		return ReadHeader(reader).has_value();
	}

	// -----------------------------------------------------------------------
	// The client
	// -----------------------------------------------------------------------

	RendezvousClient::RendezvousClient(
		engine::net::Transport &transport,
		const engine::net::Endpoint &point,
		const RendezvousSettings &settings
	)
		: Wire(transport), Point(point), Limits(settings) {
		if (Limits.RepeatEverySeconds <= 0.0) {
			Limits.RepeatEverySeconds = RendezvousSettings{}.RepeatEverySeconds;
		}
		if (Limits.RegisterEverySeconds <= 0.0) {
			Limits.RegisterEverySeconds = RendezvousSettings{}.RegisterEverySeconds;
		}
		if (Limits.GiveUpAfterSeconds <= 0.0) {
			Limits.GiveUpAfterSeconds = RendezvousSettings{}.GiveUpAfterSeconds;
		}
	}

	void RendezvousClient::Register(const Advert &advert, std::optional<SessionKey> key) {
		Registering = advert;
		HostKey = std::move(key);
		Registrable = advert.IsValid();
		Acknowledged = false;
		// Due immediately: a host that waited a full interval to say it exists
		// is a host nobody can find for that interval, and the interval is
		// sized for keeping a router's mapping alive rather than for the first
		// one.
		RegisterDueAt = 0.0;
	}

	void RendezvousClient::SetAdvert(const Advert &advert) {
		if (!Registering) {
			return;
		}
		Registering = advert;
		Registrable = advert.IsValid();
	}

	void RendezvousClient::Withdraw(double nowSeconds) {
		(void)nowSeconds;
		if (!Registering || !Registrable) {
			Registering.reset();
			Registrable = false;
			return;
		}

		engine::core::ByteWriter writer;
		WriteHeader(writer, MessageKind::Withdraw);
		WriteSessionId(writer, Registering->Session);
		if (Wire.Send(Point, writer.Bytes()) != engine::net::TransportStatus::Ok) {
			Tally.Undelivered++;
		}

		Registering.reset();
		Registrable = false;
		Acknowledged = false;
		// The punches this host was answering go with it. A meeting for a
		// session that no longer exists is a poke to somebody who has stopped
		// waiting.
		Introductions.clear();
	}

	void RendezvousClient::Browse(Purpose use, double nowSeconds) {
		(void)nowSeconds;
		engine::core::ByteWriter writer;
		WriteHeader(writer, MessageKind::Browse);
		writer.WriteUInt8(static_cast<uint8_t>(use));

		if (Wire.Send(Point, writer.Bytes()) != engine::net::TransportStatus::Ok) {
			Tally.Undelivered++;
			return;
		}
		Tally.Browses++;
	}

	bool RendezvousClient::Reach(const SessionId &session, std::optional<SessionKey> key, double nowSeconds) {
		if (!session.IsValid()) {
			return false;
		}

		Target = session;
		ReachKey = std::move(key);
		Peer = {};
		Nonce = {};
		Phase = ReachState::Locating;
		AttemptStartedAt = nowSeconds;
		// Due now rather than after an interval, for the reason a registration
		// is: the first one is the one somebody is waiting on.
		RepeatDueAt = nowSeconds;
		return true;
	}

	engine::net::Endpoint RendezvousClient::Reached() const {
		return Phase == ReachState::Reached ? Peer : engine::net::Endpoint{};
	}

	void RendezvousClient::Pump(Directory *directory, double nowSeconds) {
		while (true) {
			const engine::net::Transport::Inbound inbound = Wire.Receive(Scratch);
			if (inbound.Status != engine::net::TransportStatus::Ok) {
				break;
			}
			Handle(Scratch, inbound.From, directory, nowSeconds);
		}
		Repeat(nowSeconds);
	}

	bool RendezvousClient::Deliver(
		std::span<const std::byte> datagram,
		const engine::net::Endpoint &from,
		Directory *directory,
		double nowSeconds
	) {
		bool mine = false;
		if (!datagram.empty() && IsRendezvousMessage(datagram)) {
			Handle(datagram, from, directory, nowSeconds);
			mine = true;
		}
		Repeat(nowSeconds);
		return mine;
	}

	void RendezvousClient::SendRegistration(double nowSeconds) {
		RegisterDueAt = nowSeconds + Limits.RegisterEverySeconds;

		const std::vector<std::byte> advert = Encode(*Registering, HostKey ? &*HostKey : nullptr);

		engine::core::ByteWriter writer;
		WriteHeader(writer, MessageKind::Register);
		WriteBlock(writer, advert);

		if (Wire.Send(Point, writer.Bytes()) != engine::net::TransportStatus::Ok) {
			Tally.Undelivered++;
			return;
		}
		Tally.Registrations++;
	}

	void RendezvousClient::SendPoke(
		const engine::net::Endpoint &to,
		const SessionId &session,
		const std::array<std::byte, MEETING_NONCE_BYTES> &nonce,
		const SessionKey *key,
		bool answering
	) {
		if (!to.IsValid()) {
			return;
		}

		engine::core::ByteWriter writer;
		WriteHeader(writer, MessageKind::Poke);
		WriteSessionId(writer, session);
		WriteNonce(writer, nonce);
		writer.WriteUInt8(answering ? 1u : 0u);

		std::vector<std::byte> datagram(writer.Bytes().begin(), writer.Bytes().end());
		if (key != nullptr) {
			// Over everything before it, which includes the nonce - so a poke
			// recorded from one meeting proves nothing about the next.
			const std::array<std::byte, SessionKey::TAG_BYTES> tag = key->Tag(datagram);
			datagram.insert(datagram.end(), tag.begin(), tag.end());
		}

		if (Wire.Send(to, datagram) != engine::net::TransportStatus::Ok) {
			Tally.Undelivered++;
			return;
		}
		Tally.Poked++;
	}

	void RendezvousClient::Repeat(double nowSeconds) {
		if (Registering && Registrable && nowSeconds >= RegisterDueAt) {
			SendRegistration(nowSeconds);
		}

		if (Phase == ReachState::Locating || Phase == ReachState::Punching) {
			if (nowSeconds - AttemptStartedAt >= Limits.GiveUpAfterSeconds) {
				// Nothing answered. Failed rather than retried for ever: a
				// pair of routers that will not cooperate will not start, and
				// a spinner that never resolves is worse than a refusal.
				Phase = ReachState::Failed;
			} else if (nowSeconds >= RepeatDueAt) {
				RepeatDueAt = nowSeconds + Limits.RepeatEverySeconds;
				if (Phase == ReachState::Locating) {
					engine::core::ByteWriter writer;
					WriteHeader(writer, MessageKind::Reach);
					WriteSessionId(writer, Target);
					if (Wire.Send(Point, writer.Bytes()) != engine::net::TransportStatus::Ok) {
						Tally.Undelivered++;
					} else {
						Tally.Reaches++;
					}
				} else {
					SendPoke(Peer, Target, Nonce, ReachKey ? &*ReachKey : nullptr, false);
				}
			}
		}

		// The host's side of any punch it was told about. Expired ones are
		// dropped rather than poked for ever - a guest that gave up leaves a
		// host poking an address nobody is listening on.
		for (Introduction &meeting : Introductions) {
			if (nowSeconds >= meeting.RepeatDueAt && nowSeconds < meeting.ExpiresAtSeconds) {
				meeting.RepeatDueAt = nowSeconds + Limits.RepeatEverySeconds;
				SendPoke(
					meeting.Peer,
					Registering ? Registering->Session : SessionId{},
					meeting.Nonce,
					HostKey ? &*HostKey : nullptr,
					false
				);
			}
		}
		Introductions.erase(
			std::remove_if(
				Introductions.begin(),
				Introductions.end(),
				[nowSeconds](const Introduction &meeting) { return nowSeconds >= meeting.ExpiresAtSeconds; }
			),
			Introductions.end()
		);
	}

	void RendezvousClient::Handle(
		std::span<const std::byte> datagram,
		const engine::net::Endpoint &from,
		Directory *directory,
		double nowSeconds
	) {
		engine::core::ByteReader reader(datagram);
		const std::optional<MessageKind> kind = ReadHeader(reader);
		if (!kind) {
			Tally.Malformed++;
			return;
		}

		switch (*kind) {
		case MessageKind::Enrolled: {
			const SessionId session = ReadSessionId(reader);
			const engine::net::Endpoint reflexive = ReadEndpoint(reader);
			if (reader.Failed() || !Registering || session != Registering->Session) {
				Tally.Malformed++;
				return;
			}
			Acknowledged = true;
			Public = reflexive;
			Tally.Acknowledged++;
			return;
		}

		case MessageKind::Listed: {
			const uint8_t count = reader.ReadUInt8();
			for (uint8_t index = 0; index < count; ++index) {
				const std::span<const std::byte> block = ReadBlock(reader);
				const engine::net::Endpoint seen = ReadEndpoint(reader);
				if (reader.Failed() || block.empty()) {
					Tally.Malformed++;
					return;
				}
				if (directory == nullptr) {
					continue;
				}

				// Decoded against no keys: a browse reply lists public
				// sessions, and a private one is never in it. A caller holding
				// a key still verifies it - through `Directory`, which holds
				// them - when the same session is heard on the subnet.
				const std::optional<DecodedAdvert> decoded = Decode(block, {});
				if (!decoded) {
					Tally.Malformed++;
					continue;
				}
				if (directory->Offer(decoded->Session, Reach::Peer, seen, nowSeconds)) {
					Tally.Listed++;
				}
			}
			return;
		}

		case MessageKind::Punch: {
			const SessionId session = ReadSessionId(reader);
			const engine::net::Endpoint peer = ReadEndpoint(reader);
			const std::array<std::byte, MEETING_NONCE_BYTES> nonce = ReadNonce(reader);
			if (reader.Failed() || !peer.IsValid()) {
				Tally.Malformed++;
				return;
			}
			Tally.Punches++;

			if (Phase == ReachState::Locating && session == Target) {
				// The guest's side: the point answered, so start poking.
				Phase = ReachState::Punching;
				Peer = peer;
				Nonce = nonce;
				RepeatDueAt = nowSeconds;
				return;
			}

			if (Registering && session == Registering->Session) {
				// The host's side: somebody wants in. Poking is what opens this
				// router's mapping for them; whether they are allowed in is the
				// key's answer and then the connection's.
				for (Introduction &existing : Introductions) {
					if (existing.Peer == peer) {
						existing.Nonce = nonce;
						existing.ExpiresAtSeconds = nowSeconds + Limits.GiveUpAfterSeconds;
						existing.RepeatDueAt = nowSeconds;
						return;
					}
				}
				if (Introductions.size() >= MAXIMUM_INTRODUCTIONS) {
					// Dropped rather than evicting one in progress, for
					// `Directory`'s reason: a bound that lets a flood push out
					// the meeting somebody is waiting on is not a bound.
					return;
				}
				Introduction meeting;
				meeting.Peer = peer;
				meeting.Nonce = nonce;
				meeting.ExpiresAtSeconds = nowSeconds + Limits.GiveUpAfterSeconds;
				meeting.RepeatDueAt = nowSeconds;
				Introductions.push_back(meeting);
			}
			return;
		}

		case MessageKind::Unknown: {
			const SessionId session = ReadSessionId(reader);
			if (reader.Failed()) {
				Tally.Malformed++;
				return;
			}
			Tally.Unknown++;
			if (Phase == ReachState::Locating && session == Target) {
				Phase = ReachState::Failed;
			}
			return;
		}

		case MessageKind::Poke: {
			const SessionId session = ReadSessionId(reader);
			const std::array<std::byte, MEETING_NONCE_BYTES> nonce = ReadNonce(reader);
			const uint8_t answering = reader.ReadUInt8();
			if (reader.Failed()) {
				Tally.Malformed++;
				return;
			}

			const size_t body = reader.Position();
			const std::span<const std::byte> covered = datagram.first(body);
			const std::span<const std::byte> tag = datagram.subspan(body);

			const bool guesting = Phase == ReachState::Punching && session == Target;
			const bool hosting = Registering.has_value() && session == Registering->Session;
			if (!guesting && !hosting) {
				Tally.Malformed++;
				return;
			}

			const SessionKey *key =
				guesting ? (ReachKey ? &*ReachKey : nullptr) : (HostKey ? &*HostKey : nullptr);
			if (key != nullptr && !key->Admits(covered, tag)) {
				// A poke that cannot prove it holds the key is dropped. This is
				// possession plus return routability and nothing more - the
				// connection made over this address authenticates on its own
				// terms, one layer up.
				Tally.Refused++;
				return;
			}

			Tally.Answered++;

			if (guesting) {
				if (nonce != Nonce) {
					Tally.Refused++;
					return;
				}
				// The address it actually came from, which is the one that
				// works: a router may have mapped the peer to a port the point
				// never saw.
				Peer = from;
				Phase = ReachState::Reached;
				if (answering == 0) {
					SendPoke(from, session, nonce, key, true);
				}
				return;
			}

			for (Introduction &meeting : Introductions) {
				if (meeting.Nonce != nonce) {
					continue;
				}
				meeting.Peer = from;
				if (answering == 0) {
					SendPoke(from, session, nonce, key, true);
				}
				return;
			}
			// A poke for a meeting this host was never told about. Dropped: the
			// point is what introduces, and answering an uninvited poke would
			// let anybody who guessed a session id open a mapping here.
			Tally.Refused++;
			return;
		}

		case MessageKind::Register:
		case MessageKind::Browse:
		case MessageKind::Reach:
		case MessageKind::Withdraw:
			// A point's messages, arriving at a client. Somebody has pointed a
			// registration at the wrong port, which is a misconfiguration and
			// not an attack, and it is counted rather than answered.
			Tally.Malformed++;
			return;
		}
	}

	// -----------------------------------------------------------------------
	// The point
	// -----------------------------------------------------------------------

	RendezvousPoint::RendezvousPoint(const PointSettings &settings) : Limits(settings) {
		if (Limits.ForgetAfterSeconds <= 0.0) {
			Limits.ForgetAfterSeconds = PointSettings{}.ForgetAfterSeconds;
		}
		if (Limits.ListingsPerReply == 0) {
			Limits.ListingsPerReply = PointSettings{}.ListingsPerReply;
		}
	}

	RendezvousPoint::Enrolment *RendezvousPoint::Find(const SessionId &session) {
		for (Enrolment &held : Sessions) {
			if (held.Session == session) {
				return &held;
			}
		}
		return nullptr;
	}

	size_t RendezvousPoint::Serve(engine::net::Transport &transport, double nowSeconds) {
		size_t understood = 0;

		while (true) {
			const engine::net::Transport::Inbound inbound = transport.Receive(Scratch);
			if (inbound.Status != engine::net::TransportStatus::Ok) {
				break;
			}

			engine::core::ByteReader reader(Scratch);
			const std::optional<MessageKind> kind = ReadHeader(reader);
			if (!kind) {
				Tally.Malformed++;
				continue;
			}

			switch (*kind) {
			case MessageKind::Register: {
				const std::span<const std::byte> block = ReadBlock(reader);
				if (reader.Failed() || block.empty()) {
					Tally.Malformed++;
					continue;
				}

				// Decoded against no keys, because a point holds none and must
				// not - see the header. What it needs out of an advert is the
				// id, the purpose and whether it is private; the bytes are kept
				// as they arrived so a browse reply repeats them unchanged.
				const std::optional<DecodedAdvert> decoded = Decode(block, {});
				if (!decoded) {
					Tally.Malformed++;
					continue;
				}

				Enrolment *held = Find(decoded->Session.Session);
				if (held == nullptr) {
					if (Sessions.size() >= Limits.MaximumSessions) {
						Tally.Full++;
						continue;
					}
					Sessions.push_back({});
					held = &Sessions.back();
					held->Session = decoded->Session.Session;
					Tally.Registrations++;
				} else {
					Tally.Refreshes++;
				}

				held->Use = decoded->Session.Use;
				held->Admits = decoded->Session.Admits;
				held->From = inbound.From;
				held->AdvertBytes.assign(block.begin(), block.end());
				held->LastSeenSeconds = nowSeconds;

				engine::core::ByteWriter writer;
				WriteHeader(writer, MessageKind::Enrolled);
				WriteSessionId(writer, held->Session);
				// The address the registration arrived from - the reflexive
				// one. A host that finds this equal to its own local address is
				// a host with nothing in front of it, and a guest can dial it
				// without a punch.
				WriteEndpoint(writer, inbound.From);
				if (transport.Send(inbound.From, writer.Bytes()) != engine::net::TransportStatus::Ok) {
					Tally.Undelivered++;
				}
				understood++;
				continue;
			}

			case MessageKind::Withdraw: {
				const SessionId session = ReadSessionId(reader);
				if (reader.Failed()) {
					Tally.Malformed++;
					continue;
				}
				for (size_t index = 0; index < Sessions.size(); ++index) {
					// The address has to match. Otherwise anybody who learned a
					// session id could take it off the point, which is a denial
					// of service costing one datagram.
					if (Sessions[index].Session == session && Sessions[index].From == inbound.From) {
						Sessions.erase(Sessions.begin() + static_cast<ptrdiff_t>(index));
						Tally.Withdrawals++;
						break;
					}
				}
				understood++;
				continue;
			}

			case MessageKind::Browse: {
				const uint8_t use = reader.ReadUInt8();
				if (reader.Failed()) {
					Tally.Malformed++;
					continue;
				}

				engine::core::ByteWriter writer;
				WriteHeader(writer, MessageKind::Listed);

				uint8_t listed = 0;
				engine::core::ByteWriter body;
				for (const Enrolment &held : Sessions) {
					if (listed >= Limits.ListingsPerReply) {
						break;
					}
					if (static_cast<uint8_t>(held.Use) != use) {
						continue;
					}
					// A private session is absent rather than refused. The
					// header carries the argument: a point cannot check a key
					// it does not hold, so possession of the id is what reaches
					// one.
					if (held.Admits == Access::Private) {
						continue;
					}
					WriteBlock(body, held.AdvertBytes);
					WriteEndpoint(body, held.From);
					listed++;
				}

				writer.WriteUInt8(listed);
				writer.WriteRaw(body.Bytes().data(), body.Bytes().size());
				if (transport.Send(inbound.From, writer.Bytes()) != engine::net::TransportStatus::Ok) {
					Tally.Undelivered++;
				}
				Tally.Browses++;
				understood++;
				continue;
			}

			case MessageKind::Reach: {
				const SessionId session = ReadSessionId(reader);
				if (reader.Failed()) {
					Tally.Malformed++;
					continue;
				}

				const Enrolment *held = Find(session);
				if (held == nullptr) {
					engine::core::ByteWriter writer;
					WriteHeader(writer, MessageKind::Unknown);
					WriteSessionId(writer, session);
					if (transport.Send(inbound.From, writer.Bytes()) != engine::net::TransportStatus::Ok) {
						Tally.Undelivered++;
					}
					Tally.Unknown++;
					understood++;
					continue;
				}

				// One nonce for both halves of one meeting, drawn here because
				// this is the only party both sides trust to have drawn it
				// fresh.
				const std::array<std::byte, MEETING_NONCE_BYTES> nonce = DrawNonce();

				engine::core::ByteWriter toHost;
				WriteHeader(toHost, MessageKind::Punch);
				WriteSessionId(toHost, session);
				WriteEndpoint(toHost, inbound.From);
				WriteNonce(toHost, nonce);

				engine::core::ByteWriter toGuest;
				WriteHeader(toGuest, MessageKind::Punch);
				WriteSessionId(toGuest, session);
				WriteEndpoint(toGuest, held->From);
				WriteNonce(toGuest, nonce);

				if (transport.Send(held->From, toHost.Bytes()) != engine::net::TransportStatus::Ok) {
					Tally.Undelivered++;
				}
				if (transport.Send(inbound.From, toGuest.Bytes()) != engine::net::TransportStatus::Ok) {
					Tally.Undelivered++;
				}
				Tally.Introductions++;
				understood++;
				continue;
			}

			case MessageKind::Enrolled:
			case MessageKind::Listed:
			case MessageKind::Punch:
			case MessageKind::Unknown:
			case MessageKind::Poke:
				// A client's messages, arriving at a point. Counted rather than
				// answered - a point that replied to a reply is a reflector.
				Tally.Malformed++;
				continue;
			}
		}

		return understood;
	}

	size_t RendezvousPoint::Forget(double nowSeconds) {
		const size_t before = Sessions.size();
		const double oldest = nowSeconds - Limits.ForgetAfterSeconds;

		Sessions.erase(
			std::remove_if(
				Sessions.begin(),
				Sessions.end(),
				[oldest](const Enrolment &held) { return held.LastSeenSeconds < oldest; }
			),
			Sessions.end()
		);

		const size_t dropped = before - Sessions.size();
		Tally.Forgotten += dropped;
		return dropped;
	}
}
