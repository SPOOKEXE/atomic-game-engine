#include <engine/net/Endpoint.hpp>

#include <network/Beacon.hpp>
#include <utility>

namespace network {

	Beacon::Beacon(
		engine::net::Transport &transport,
		const Advert &advert,
		std::optional<SessionKey> key,
		const BeaconSettings &settings
	)
		: Wire(transport), Saying(advert), Key(std::move(key)), Limits(settings) {
		if (Limits.AnnounceEverySeconds <= 0.0) {
			Limits.AnnounceEverySeconds = BeaconSettings{}.AnnounceEverySeconds;
		}
	}

	bool Beacon::Announcing() const {
		if (!Saying.IsValid()) {
			return false;
		}
		// A private session with no key announces nothing. The header carries
		// the argument: an untagged private advert is a public one wearing a
		// label, and a host that has misconfigured its key should be invisible
		// rather than impersonable.
		return Saying.Admits != Access::Private || Key.has_value();
	}

	void Beacon::SetAdvert(const Advert &advert) {
		Saying = advert;
	}

	void Beacon::Pump(double nowSeconds) {
		if (Scheduled && nowSeconds < DueAt) {
			return;
		}
		Announce(nowSeconds);
	}

	bool Beacon::Announce(double nowSeconds) {
		// Scheduled whatever happens, including for a refusal. A beacon that
		// only rescheduled on success would re-check a misconfigured advert
		// every tick, which is a busy loop that also fills the counter.
		DueAt = nowSeconds + Limits.AnnounceEverySeconds;
		Scheduled = true;

		if (!Announcing()) {
			Tally.Refused++;
			return false;
		}

		const std::vector<std::byte> datagram = Encode(Saying, Key ? &*Key : nullptr);
		const engine::net::Endpoint everywhere = engine::net::Endpoint::BroadcastIPv4(Limits.Port);

		if (Wire.Send(everywhere, datagram) != engine::net::TransportStatus::Ok) {
			// Counted rather than retried. The next announcement is a quarter
			// of a second away and carries newer numbers than this one, so a
			// retry would put a stale advert on a wire that is already
			// congested - which is why `net` keeps no outbox either.
			Tally.Undelivered++;
			return false;
		}

		Tally.Announcements++;
		Tally.Bytes += datagram.size();
		return true;
	}
}
