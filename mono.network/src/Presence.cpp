#include <network/Presence.hpp>
#include <utility>

namespace network {

	namespace {
		// The sockets a presence opens, and the settings that shape them.
		//
		// Both are small: a discovery datagram is a few hundred bytes and a
		// browser drains every tick, so the default quarter-megabyte receive
		// buffer would be a quarter of a megabyte per program for nothing.
		constexpr size_t DISCOVERY_QUEUE_BYTES = 64u * 1024u;

		engine::net::TransportSettings AnnouncerSettings() {
			engine::net::TransportSettings settings;
			settings.Broadcast = true;
			settings.ReceiveQueueBytes = DISCOVERY_QUEUE_BYTES;
			return settings;
		}

		engine::net::TransportSettings ListenerSettings() {
			engine::net::TransportSettings settings;
			settings.ReuseAddress = true;
			settings.ReceiveQueueBytes = DISCOVERY_QUEUE_BYTES;
			return settings;
		}
	}

	const char *Describe(PresenceFault fault) {
		switch (fault) {
		case PresenceFault::None:
			return "none";
		case PresenceFault::NoBeaconSocket:
			return "no beacon socket";
		case PresenceFault::NoDiscoverySocket:
			return "no discovery socket";
		case PresenceFault::BadRendezvousAddress:
			return "bad rendezvous address";
		}
		// No default label, so adding a fault is a warning here.
		return "?";
	}

	std::unique_ptr<Presence> Presence::Open(
		const PresenceSettings &settings,
		const Advert &advert,
		std::optional<SessionKey> key,
		engine::net::Transport *session
	) {
		DirectorySettings table = settings.Directory;
		table.Protocol = settings.Protocol;
		table.Use = settings.Use;

		std::unique_ptr<Presence> presence(new Presence(table));
		presence->Saying = advert;
		presence->Saying.Protocol = settings.Protocol;
		presence->Saying.Use = settings.Use;

		const bool wantsPoint = !settings.RendezvousAddress.empty();

		// One socket serves both announcing and the rendezvous, and the header
		// says why it has to: a punched hole belongs to the port that punched
		// it. An ephemeral port, because the well-known one is the listener's
		// and a machine has to be able to host more than one session.
		//
		// A caller that supplied a session transport needs the announcing
		// socket only for the beacon, because the rendezvous is going to run
		// over theirs.
		const bool needsAnnouncer = settings.Announce || (wantsPoint && session == nullptr);
		if (needsAnnouncer) {
			presence->Announcer = engine::net::MakeUdpTransport(0, AnnouncerSettings());
			if (!presence->Announcer) {
				presence->Trouble = PresenceFault::NoBeaconSocket;
			}
		}

		if (settings.Announce && presence->Announcer) {
			BeaconSettings beacon = settings.Beacon;
			beacon.Port = settings.DiscoveryPort;
			presence->Broadcasting.emplace(*presence->Announcer, presence->Saying, std::move(key), beacon);
		}

		if (settings.Discover) {
			presence->Listening = engine::net::MakeUdpTransport(settings.DiscoveryPort, ListenerSettings());
			if (!presence->Listening) {
				// Almost always another program holding the port without
				// `ReuseAddress`. The announcement half still runs — a partial
				// success is a success, and a program that refused to start
				// over this would be refusing over a feature nobody asked to be
				// essential.
				presence->Trouble = PresenceFault::NoDiscoverySocket;
			}
		}

		engine::net::Transport *rendezvousWire = session != nullptr ? session : presence->Announcer.get();
		if (wantsPoint && rendezvousWire != nullptr) {
			const std::optional<engine::net::Endpoint> point =
				engine::net::Endpoint::Parse(settings.RendezvousAddress);
			if (!point) {
				presence->Trouble = PresenceFault::BadRendezvousAddress;
			} else {
				presence->Sharing = session != nullptr;
				presence->Meeting =
					std::make_unique<RendezvousClient>(*rendezvousWire, *point, settings.Rendezvous);
				// **Registering is decided by having something to register, not
				// by whether the subnet is being broadcast to.** Those are
				// different questions and tying them together made the case the
				// point exists for — a host reachable through a rendezvous and
				// announcing to nobody, which is every dedicated server on the
				// internet — the one case that silently registered nothing while
				// still logging that it had.
				//
				// The same advert either way, and with no key: the beacon took
				// ownership of the one that was passed in, and a host that is
				// both LAN-visible and registered is one session either way.
				if (presence->Saying.IsValid()) {
					presence->Meeting->Register(presence->Saying);
				}
			}
		}

		return presence;
	}

	Presence::~Presence() = default;

	void Presence::Pump(double nowSeconds) {
		if (Broadcasting) {
			Broadcasting->Pump(nowSeconds);
		}
		if (Listening) {
			Table.Observe(*Listening, nowSeconds);
		}
		if (Meeting) {
			if (Sharing) {
				// The session's owner drains that transport. All that is left
				// here is the repeating — a registration due, a poke due — and
				// the repeats are what a punch is made of.
				Meeting->Deliver({}, {}, &Table, nowSeconds);
			} else {
				Meeting->Pump(&Table, nowSeconds);
			}
		}
		Table.Forget(nowSeconds);
	}

	bool Presence::Deliver(
		std::span<const std::byte> datagram, const engine::net::Endpoint &from, double nowSeconds
	) {
		if (!Meeting || datagram.empty()) {
			return false;
		}
		return Meeting->Deliver(datagram, from, &Table, nowSeconds);
	}

	void Presence::SetAdvert(const Advert &advert) {
		Saying = advert;
		if (Broadcasting) {
			Broadcasting->SetAdvert(Saying);
		}
		if (Meeting) {
			Meeting->SetAdvert(Saying);
		}
	}

	void Presence::Withdraw(double nowSeconds) {
		if (Meeting) {
			Meeting->Withdraw(nowSeconds);
		}
	}

	void Presence::Browse(double nowSeconds) {
		if (Meeting) {
			Meeting->Browse(Saying.Use, nowSeconds);
		}
	}

	bool Presence::Reach(const SessionId &session, std::optional<SessionKey> key, double nowSeconds) {
		if (!Meeting) {
			return false;
		}
		return Meeting->Reach(session, std::move(key), nowSeconds);
	}

	ReachState Presence::Reaching() const {
		return Meeting ? Meeting->State() : ReachState::Idle;
	}

	engine::net::Endpoint Presence::Reached() const {
		return Meeting ? Meeting->Reached() : engine::net::Endpoint{};
	}

	engine::net::Endpoint Presence::Reflexive() const {
		return Meeting ? Meeting->Reflexive() : engine::net::Endpoint{};
	}

	engine::net::Endpoint Presence::AnnouncingFrom() const {
		return Announcer ? Announcer->Local() : engine::net::Endpoint{};
	}

	bool Presence::Announcing() const {
		return Broadcasting.has_value() && Broadcasting->Announcing();
	}
}
