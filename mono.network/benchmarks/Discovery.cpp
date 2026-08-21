// Being found on a subnet, at the rate a busy subnet is noisy.
//
// **Discovery is the one path in this engine where every byte comes from a
// stranger and none of it was asked for.** A game link is dialled by somebody
// who already knows the address; a content fetch answers a request this process
// made. A broadcast listener answers nothing - it sits on a port and decodes
// whatever the switch delivers, which on a real network includes every other
// process's discovery traffic, every misconfigured device, and anybody who
// feels like sending a megabyte of noise to the broadcast address.
//
// So the interesting figures are the refusals rather than the successes. A
// browser open on a LAN party is decoding a few hundred adverts a second and
// discarding far more than it keeps; what decides whether that browser stays
// responsive is what a *rejected* datagram costs, not what an accepted one
// does.
//
// **The key trial is the row with the real risk.** A private session is
// authenticated against the keys this process holds, and `Decode` is given all
// of them because it cannot know which one applies. That is a MAC per key in
// the worst case, run on data a stranger sent, at whatever rate the stranger
// chooses - which is a denial of service with an amplification factor equal to
// the number of keys the browser has been given. The rows walk the key count so
// the factor is a measured number rather than a suspicion.
//
// **The directory rows are about a table that must not grow.** It is bounded at
// `MaximumListings` and expires by wall time, and both of those are walks. A
// refresh of a session already listed is what almost every announcement is, so
// it is the row that runs; the overflow and expiry rows are what happens when
// somebody points a script at the broadcast address.
//
// Nothing here opens a socket. `Beacon::Pump` and `Directory::Observe` need a
// transport and what they add over these rows is `net`'s send and receive,
// which `engine.net.bench.framing` already measures and measures better.

#include <engine/net/Endpoint.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <network/Advert.hpp>
#include <network/Directory.hpp>
#include <network/Enums.hpp>
#include <network/SessionKey.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("network.bench.discovery")

using engine::net::Endpoint;
using engine::testing::Consume;
using network::Access;
using network::Advert;
using network::DecodedAdvert;
using network::Directory;
using network::DirectorySettings;
using network::Purpose;
using network::Reach;
using network::SessionId;
using network::SessionKey;

namespace discovery_bench {
	// Datagrams per sample. A busy LAN party is a few hundred a second, so this
	// is two orders of magnitude past the worst honest load and the right order
	// for a flood.
	constexpr size_t DATAGRAMS = 100'000;

	// A full table, which is what a browser has after a script has pointed
	// itself at the broadcast address for a second.
	constexpr size_t LISTINGS = 256;

	// A deterministic session id. **Not `SessionId::Draw`**: a benchmark that
	// drew entropy would report the entropy source in whichever row happened to
	// exhaust a pool, and two runs would not be comparable.
	SessionId SessionOf(uint64_t index) {
		SessionId id;
		for (size_t byte = 0; byte < id.Value.size(); byte++) {
			id.Value[byte] = static_cast<std::byte>((index >> (byte % 8 * 8)) ^ (byte * 13) ^ 0x5A);
		}
		return id;
	}

	// What a host announces about itself. The wildcard address, because that is
	// what a host that bound every interface actually knows.
	Advert AdvertOf(uint64_t index, Access admits = Access::Public) {
		Advert advert;
		advert.Session = SessionOf(index);
		advert.Use = Purpose::Game;
		advert.Admits = admits;
		advert.Protocol = 0;
		advert.At = Endpoint::FromIPv4({0, 0, 0, 0}, 7777);
		advert.Name = "a session " + std::to_string(index);
		advert.Detail = "somewhere in particular";
		advert.Peers = static_cast<uint16_t>(index % 32);
		advert.PeerLimit = 32;
		return advert;
	}

	// Where a datagram arrived from - the one address known to route, because a
	// datagram came over it.
	Endpoint FromOf(uint64_t index) {
		return Endpoint::FromIPv4(
			{192, 168, static_cast<uint8_t>(index / 250 % 250), static_cast<uint8_t>(index % 250 + 1)}, 7777
		);
	}

	// The key the tagged datagrams below are actually signed with.
	const SessionKey &Signing() {
		static const SessionKey key = std::move(*SessionKey::FromPassphrase("the right one"));
		return key;
	}

	// A ring of `count` keys, none of which is `Signing`.
	//
	// **Separate from the signing key on purpose.** A ring that happened to
	// contain the right key would make the key-trial rows measure the lucky
	// case, and the row that matters is the one where every key is tried and
	// none of them verifies. Passphrases rather than raw bytes because that is
	// how a person shares one.
	//
	// A deque, so a reference handed out earlier survives a later ring being
	// built.
	const std::vector<SessionKey> &Decoys(size_t count) {
		static std::deque<std::vector<SessionKey>> built;
		while (built.size() <= count) {
			std::vector<SessionKey> keys;
			for (size_t index = 0; index < built.size(); index++) {
				keys.push_back(std::move(*SessionKey::FromPassphrase("decoy " + std::to_string(index))));
			}
			built.push_back(std::move(keys));
		}
		return built[count];
	}

	// `count` keys with the signing key last, which is the worst arrangement a
	// ring that is searched in order can be in.
	std::span<const SessionKey> RingEndingInTheRightKey(size_t count) {
		static std::deque<std::vector<SessionKey>> built;
		while (built.size() <= count) {
			std::vector<SessionKey> keys;
			for (size_t index = 0; index + 1 < built.size(); index++) {
				keys.push_back(std::move(*SessionKey::FromPassphrase("decoy " + std::to_string(index))));
			}
			if (!built.empty()) {
				keys.push_back(std::move(*SessionKey::FromPassphrase("the right one")));
			}
			built.push_back(std::move(keys));
		}
		return built[count];
	}

	// One encoded advert per index, built once.
	const std::vector<std::vector<std::byte>> &Datagrams(bool signedWithKey) {
		static std::vector<std::vector<std::byte>> plain;
		static std::vector<std::vector<std::byte>> tagged;
		std::vector<std::vector<std::byte>> &built = signedWithKey ? tagged : plain;
		if (built.empty()) {
			built.reserve(LISTINGS);
			for (uint64_t index = 0; index < LISTINGS; index++) {
				const Advert advert = AdvertOf(index, signedWithKey ? Access::Private : Access::Public);
				built.push_back(
					signedWithKey ? network::Encode(advert, &Signing()) : network::Encode(advert, nullptr)
				);
			}
		}
		return built;
	}

	// Decodes the prepared datagrams round-robin and counts what came back, so
	// the accepted and refused branches are both live.
	size_t DecodeAll(const std::vector<std::vector<std::byte>> &datagrams, std::span<const SessionKey> keys) {
		size_t accepted = 0;
		for (size_t attempt = 0; attempt < DATAGRAMS; attempt++) {
			const std::optional<DecodedAdvert> decoded =
				network::Decode(datagrams[attempt % datagrams.size()], keys);
			accepted += decoded.has_value() ? 1 : 0;
		}
		return accepted;
	}

	// A directory holding a full table, rebuilt per call so a row that mutates
	// it does not change the row after it.
	Directory Full(size_t listings = LISTINGS) {
		DirectorySettings settings;
		settings.MaximumListings = LISTINGS;
		settings.ForgetAfterSeconds = 5.0;
		Directory directory(settings);
		for (uint64_t index = 0; index < listings; index++) {
			directory.Offer(AdvertOf(index), Reach::Lan, FromOf(index), 100.0);
		}
		return directory;
	}
}

using namespace discovery_bench;

// --- encoding -----------------------------------------------------------------
//
// A host announces once every second or two, so this is not a hot path for the
// sender. It is here because the decoder's figure means nothing without it: the
// difference between the two is what the format costs to *check* over what it
// costs to *build*, and a decoder that is not several times the encoder is a
// decoder that is trusting something.

BENCH("Encode · 100k unsigned adverts", DATAGRAMS) {
	size_t bytes = 0;
	for (size_t call = 0; call < DATAGRAMS; call++) {
		bytes += network::Encode(AdvertOf(call % LISTINGS), nullptr).size();
	}
	Consume(bytes);
}

BENCH("Encode · 100k adverts tagged with a session key", DATAGRAMS) {
	const SessionKey &key = Signing();
	size_t bytes = 0;
	for (size_t call = 0; call < DATAGRAMS; call++) {
		bytes += network::Encode(AdvertOf(call % LISTINGS, Access::Private), &key).size();
	}
	Consume(bytes);
}

// --- decoding what a stranger sent --------------------------------------------

BENCH("Decode · 100k public adverts, no keys held", DATAGRAMS) {
	// The ordinary case for a client browsing an open LAN. No key, so nothing
	// is authenticated and nothing is tried.
	Consume(DecodeAll(Datagrams(false), {}));
}

BENCH("Decode · 100k public adverts against 8 keys", DATAGRAMS) {
	// **The row that shows whether an untagged advert costs a key trial.** It
	// must not: a public announcement carries no tag, so there is nothing for a
	// key to check and a decoder that tried anyway would multiply the cost of
	// the most common datagram on the subnet by the size of the key ring.
	Consume(DecodeAll(Datagrams(false), Decoys(8)));
}

BENCH("Decode · 100k tagged adverts against the 1 key that verifies", DATAGRAMS) {
	Consume(DecodeAll(Datagrams(true), RingEndingInTheRightKey(1)));
}

BENCH("Decode · 100k tagged adverts against 8 keys, the right one last", DATAGRAMS) {
	// Seven that do not verify before the one that does, which is the worst a
	// ring searched in order can do for a session that *is* known.
	Consume(DecodeAll(Datagrams(true), RingEndingInTheRightKey(8)));
}

BENCH("Decode · 100k tagged adverts against 64 keys, none of them right", DATAGRAMS) {
	// **The amplification factor, and the row this file exists for.** Every key
	// in the ring is tried and none verifies, on data a stranger chose to send,
	// at a rate the stranger chooses. Read it against the one-key row: the
	// ratio is what an attacker gets for free by broadcasting noise at a
	// browser, and it is why a browser is given the keys for the sessions a
	// person cares about rather than every key it has ever seen.
	Consume(DecodeAll(Datagrams(true), Decoys(64)));
}

// --- decoding what is not an advert at all ------------------------------------

BENCH("Decode · 100k datagrams that are not adverts", DATAGRAMS) {
	// The format version is in the frame and is checked before any field is
	// read, so this should be the cheapest row in the file. It is also the most
	// frequently executed one on a shared network, where other people's
	// protocols use the same broadcast address.
	static const std::vector<std::vector<std::byte>> rubbish = [] {
		std::vector<std::vector<std::byte>> built;
		for (size_t index = 0; index < 16; index++) {
			built.emplace_back(64 + index * 16, std::byte{0xC3});
		}
		return built;
	}();
	Consume(DecodeAll(rubbish, Decoys(8)));
}

BENCH("Decode · 100k truncated adverts", DATAGRAMS) {
	// A real advert cut short, which is what a fragmented or clipped datagram
	// looks like and what a fuzzer produces first. Refused at a length check
	// rather than by reading past the end.
	static const std::vector<std::vector<std::byte>> clipped = [] {
		std::vector<std::vector<std::byte>> built;
		for (const std::vector<std::byte> &whole : Datagrams(false)) {
			built.push_back(std::vector<std::byte>(whole.begin(), whole.begin() + whole.size() / 2));
		}
		return built;
	}();
	Consume(DecodeAll(clipped, Decoys(8)));
}

// --- the table ----------------------------------------------------------------

BENCH("Offer · 100k refreshes of sessions already listed", DATAGRAMS) {
	// **What almost every announcement is.** A session announces every second
	// or two and is listed once, so a directory watching thirty sessions
	// handles thirty refreshes for every new listing. This is the row that
	// runs.
	static Directory directory = Full();
	size_t listed = 0;
	for (size_t announcement = 0; announcement < DATAGRAMS; announcement++) {
		const uint64_t index = announcement % LISTINGS;
		listed +=
			directory.Offer(AdvertOf(index), Reach::Lan, FromOf(index), 100.0 + double(announcement)) ? 1 : 0;
	}
	Consume(listed);
}

BENCH("Offer · 100k new sessions into a table that is already full", DATAGRAMS) {
	// The flood. Every offer is a session the table has never seen and the
	// table is at its cap, so every one of them is refused and counted - which
	// has to happen without the table growing and without the refusal costing
	// more than the accept.
	static Directory directory = Full();
	size_t listed = 0;
	for (uint64_t announcement = 0; announcement < DATAGRAMS; announcement++) {
		listed += directory.Offer(AdvertOf(LISTINGS + announcement), Reach::Lan, FromOf(announcement), 100.0)
					  ? 1
					  : 0;
	}
	Consume(listed);
}

BENCH("Find · 100k lookups in a full table", DATAGRAMS) {
	// What a browser does per row per frame while somebody scrolls.
	static const Directory directory = Full();
	size_t found = 0;
	for (size_t lookup = 0; lookup < DATAGRAMS; lookup++) {
		found += directory.Find(SessionOf(lookup % LISTINGS)) != nullptr ? 1 : 0;
	}
	Consume(found);
}

BENCH("Forget · 100k sweeps of a full table with nothing due", DATAGRAMS) {
	// Called at the same barrier as `Observe`, so it runs every tick whether or
	// not anything has gone quiet - and nothing has gone quiet almost every
	// tick. The sweep that finds nothing is the one that is always paid, so it
	// is the one measured: the table is a second old against a five-second
	// expiry, which leaves it unchanged and leaves this row measuring the walk
	// rather than the removal.
	static Directory directory = Full();
	size_t dropped = 0;
	for (size_t sweep = 0; sweep < DATAGRAMS; sweep++) {
		dropped += directory.Forget(101.0);
	}
	Consume(dropped);
}
