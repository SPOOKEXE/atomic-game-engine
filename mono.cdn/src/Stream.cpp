#include <engine/core/Log.hpp>

#include <algorithm>
#include <cdn/Stream.hpp>
#include <optional>
#include <utility>

namespace cdn {

	namespace {
		// Hex when it is exactly that, words otherwise. A person who was given
		// words types words, and a launcher that generated a key passes the
		// key - and the two are not ambiguous, because a 64-character
		// passphrase of nothing but hex digits is not a sentence anybody typed.
		std::optional<network::SessionKey> ReadSecret(const std::string &secret) {
			if (secret.empty()) {
				return std::nullopt;
			}
			std::optional<network::SessionKey> key = network::SessionKey::FromText(secret);
			if (!key) {
				key = network::SessionKey::FromPassphrase(secret);
			}
			return key;
		}

		// The protocol a content stream speaks.
		//
		// **Not the replication protocol.** A content origin and a game server
		// change their wires for different reasons and at different times, and
		// one number for both would make a client refuse to see an origin
		// because a component encoding moved. This is the version of what
		// `cdn::Service` answers, and it moves when a route does.
		constexpr uint32_t STREAM_PROTOCOL = 1;
	}

	std::unique_ptr<Stream> Stream::Open(const StreamSettings &settings, std::string &error) {
		std::optional<network::SessionKey> key;
		if (!settings.Secret.empty()) {
			key = ReadSecret(settings.Secret);
			if (!key) {
				error = "the stream secret is neither 64 hex characters nor a passphrase";
				return nullptr;
			}
		}

		std::unique_ptr<Stream> stream(new Stream());

		stream->Announcement.Session = network::SessionId::Draw();
		if (!stream->Announcement.Session.IsValid()) {
			error = "no entropy for a session id, so this stream cannot be announced";
			return nullptr;
		}
		stream->Announcement.Use = network::Purpose::Content;
		stream->Announcement.Admits = key ? network::Access::Private : network::Access::Public;
		stream->Announcement.Protocol = STREAM_PROTOCOL;
		// The wildcard address with the bound port, exactly as a game server
		// announces: the browser at the other end resolves it against where the
		// datagram came from, which is the one route known to work.
		stream->Announcement.At = engine::net::Endpoint::FromIPv4({0, 0, 0, 0}, settings.Port);
		stream->Announcement.Name = settings.Name.empty() ? std::string("atomic content") : settings.Name;
		stream->Announcement.Detail = settings.Detail;

		if (settings.Announce || !settings.RendezvousAddress.empty()) {
			network::PresenceSettings presence;
			presence.Announce = settings.Announce;
			// An origin does not browse. It is the thing being found.
			presence.Discover = false;
			presence.RendezvousAddress = settings.RendezvousAddress;
			presence.Protocol = STREAM_PROTOCOL;
			presence.Use = network::Purpose::Content;

			// **No session transport, and that is not the omission it is on a
			// game server.** What a client needs from a content stream is a
			// *TCP* address to fetch over; the punch that would matter is one
			// through a UDP hole, and HTTP does not travel through it. So the
			// rendezvous here is a directory - it tells a client where the
			// origin is - and an origin behind a NAT still needs a forwarded
			// port. Saying that plainly beats a punch that opens a mapping
			// nothing uses.
			stream->Finding = network::Presence::Open(presence, stream->Announcement, std::move(key));
			if (stream->Finding->Fault() != network::PresenceFault::None) {
				ENGINE_WARN("cdn: stream discovery: {}", network::Describe(stream->Finding->Fault()));
			}
		}

		if (settings.RendezvousListenPort != 0) {
			engine::net::TransportSettings socket;
			stream->PointSocket = engine::net::MakeUdpTransport(settings.RendezvousListenPort, socket);
			if (stream->PointSocket == nullptr) {
				error = "could not bind the rendezvous port";
				return nullptr;
			}
			stream->Point = std::make_unique<network::RendezvousPoint>();
			ENGINE_INFO("cdn: rendezvous point on {}", stream->PointSocket->Local().Text());
		}

		return stream;
	}

	Stream::~Stream() = default;

	bool Stream::Announcing() const {
		return Finding != nullptr && Finding->Announcing();
	}

	size_t Stream::Hosting() const {
		return Point == nullptr ? 0 : Point->Holding();
	}

	void Stream::Pump(double nowSeconds) {
		if (Finding != nullptr) {
			Finding->Pump(nowSeconds);
		}
		if (Point != nullptr) {
			Point->Serve(*PointSocket, nowSeconds);
			Point->Forget(nowSeconds);
			// Copied out so a caller can read them without the point's header
			// being complete at every call site, and so a dashboard sampling
			// them is reading one consistent set rather than a table mid-serve.
			PointTally = Point->Counters();
		}
	}

	void Stream::Withdraw(double nowSeconds) {
		if (Finding != nullptr) {
			Finding->Withdraw(nowSeconds);
		}
	}

	std::unique_ptr<StreamFinder> StreamFinder::Open(const StreamSearch &search) {
		std::unique_ptr<StreamFinder> finder(new StreamFinder());

		network::PresenceSettings presence;
		presence.Announce = false;
		presence.Discover = search.Browse;
		presence.RendezvousAddress = search.RendezvousAddress;
		presence.Protocol = STREAM_PROTOCOL;
		presence.Use = network::Purpose::Content;
		presence.Directory.ForgetAfterSeconds = search.ForgetAfterSeconds;

		finder->Looking = network::Presence::Open(presence);
		for (const std::string &secret : search.Secrets) {
			std::optional<network::SessionKey> key = ReadSecret(secret);
			if (key) {
				finder->Looking->Seen().Trust(std::move(*key));
			} else {
				// Named rather than dropped. A secret that was meant to unlock
				// a stream and did not is the reason somebody cannot see it,
				// and silence here is the reason they never find out.
				ENGINE_WARN("cdn: a stream secret is neither 64 hex characters nor a passphrase");
			}
		}
		return finder;
	}

	StreamFinder::~StreamFinder() = default;

	void StreamFinder::Pump(double nowSeconds) {
		Looking->Pump(nowSeconds);
	}

	void StreamFinder::Ask(double nowSeconds) {
		Looking->Browse(nowSeconds);
	}

	const network::Directory &StreamFinder::Seen() const {
		return Looking->Seen();
	}

	network::Directory &StreamFinder::Seen() {
		return Looking->Seen();
	}

	std::vector<engine::delivery::Source> StreamFinder::Sources() const {
		std::vector<const network::Listing *> rows;
		for (const network::Listing &row : Looking->Seen().Listings()) {
			if (!row.Joinable() || !row.Dial().IsValid()) {
				continue;
			}
			rows.push_back(&row);
		}

		// Nearest first, which is the order `delivery::AssetClient` walks. A
		// stable sort so two streams at the same reach keep the order they were
		// found in, rather than swapping places between frames and making a
		// preferences list unreadable.
		std::stable_sort(
			rows.begin(), rows.end(), [](const network::Listing *left, const network::Listing *right) {
				return left->Via < right->Via;
			}
		);

		std::vector<engine::delivery::Source> sources;
		sources.reserve(rows.size());
		for (const network::Listing *row : rows) {
			engine::delivery::Source source;
			source.Name = row->Session.Name;
			source.Kind = engine::delivery::SourceKind::Http;
			source.Location = row->Dial().Text();
			source.Enabled = true;
			// **Read, never write.** A discovered origin is one this process
			// found rather than one an operator configured, and uploading to
			// whatever answered a broadcast is how content reaches a machine
			// nobody meant to publish from.
			source.Role = engine::delivery::SourceRole::Read;
			sources.push_back(std::move(source));
		}
		return sources;
	}
}
