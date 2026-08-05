#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cdn/Origin.hpp>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("cdn.origin")
TEST_DEPENDS("cdn.gate")
TEST_DEPENDS("cdn.groupcodec")
TEST_DEPENDS("cdn.preparedcache")
TEST_DEPENDS("engine.assets.manifest")
TEST_DEPENDS("engine.core.framegraph")

using cdn::CDNSettings;
using cdn::ContentRoot;
using cdn::Origin;
using cdn::PayloadSource;
using cdn::PreparedFrame;
using cdn::Publication;
using cdn::RequestId;
using cdn::RequestState;
using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Grant;
using engine::assets::GrantKey;
using engine::assets::GrantScope;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	namespace fs = std::filesystem;

	constexpr uint64_t NOW = 1'000'000;
	constexpr uint64_t EXPIRY = NOW + 300;

	GrantKey Key(uint8_t fill = 1) {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(fill + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	ChunkEntry Chunk(std::string_view text) {
		ChunkEntry entry;
		entry.Hash = Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
		entry.Bytes = static_cast<uint32_t>(text.size());
		return entry;
	}

	// A directory to mount. The origin needs a real root because a publication
	// holds one; nothing in these cases reads a file through it.
	struct Tree {
		fs::path Root;

		Tree() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-origin-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
			fs::create_directories(Root);
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		ContentRoot Mount() const {
			auto root = ContentRoot::Mount(Root);
			REQUIRE(root.has_value());
			return *root;
		}
	};

	// A publication holding one bundle, whose root the caller gets back.
	std::shared_ptr<const Publication>
	Published(const Tree &tree, ContentHash &bundleOut, std::string_view marker = "one") {
		Manifest manifest;
		const ContentHash asset = manifest.AddAsset(
			std::string("meshes/rock-") + std::string(marker),
			AssetKind::Mesh,
			{Chunk(std::string("rock-") + std::string(marker))}
		);
		const auto bundle = manifest.AddBundle(std::vector<ContentHash>{asset});
		REQUIRE(bundle.has_value());
		bundleOut = *bundle;

		return std::make_shared<const Publication>(tree.Mount(), manifest);
	}

	std::vector<std::byte> Token(const GrantKey &key, const ContentHash &bundle) {
		GrantScope scope;
		scope.Session = 7;
		scope.Bundles = {bundle};
		scope.ExpiresAtSeconds = EXPIRY;
		scope.ByteBudget = 1024 * 1024;

		const auto grant = Grant::Issue(scope, key);
		REQUIRE(grant.has_value());
		return grant->Encode();
	}

	// Something compressible, so a prepared frame is meaningfully smaller than
	// its payload.
	PayloadSource Source(size_t records = 200) {
		return [records](const ContentHash &) -> std::optional<std::vector<std::byte>> {
			std::string text;
			for (size_t index = 0; index < records; ++index) {
				text += "{\"asset\":\"mesh\",\"format\":\"cooked-v1\",\"lod\":0}";
			}
			return Bytes(text);
		};
	}

	PayloadSource NoSource() {
		return [](const ContentHash &) -> std::optional<std::vector<std::byte>> { return std::nullopt; };
	}
}

TEST_CASE("a request is admitted, prepared and taken", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(id.IsValid());
	CHECK(origin.StateOf(id) == RequestState::Pending);

	CHECK(origin.Pump(Source()) == 1);
	CHECK(origin.StateOf(id) == RequestState::Ready);

	const PreparedFrame frame = origin.Take(id);
	REQUIRE(frame != nullptr);
	CHECK_FALSE(frame->empty());

	// The request is finished by the take: leaving it would grow the table for
	// the life of the process, and a second take answering the same bytes would
	// hide a caller taking one result twice.
	CHECK(origin.StateOf(id) == RequestState::Unknown);
	CHECK(origin.Take(id) == nullptr);
	CHECK(origin.Outstanding() == 0);
}

TEST_CASE("a request the gate refuses never prepares", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key(1));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	// A token from a different key.
	const RequestId id = origin.Submit(Token(Key(200), bundle), bundle, NOW);

	// Refused still gets a handle, so a caller can ask why once rather than
	// getting a bare failure that carries no state.
	CHECK(origin.StateOf(id) == RequestState::Refused);
	CHECK(origin.Pump(Source()) == 0);
	CHECK(origin.Take(id) == nullptr);
}

TEST_CASE("a request outside the grant's scope is refused", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const ContentHash elsewhere = Hasher::Of(Bytes("someone else's bundle"));
	const RequestId id = origin.Submit(Token(Key(), bundle), elsewhere, NOW);
	CHECK(origin.StateOf(id) == RequestState::Refused);
}

TEST_CASE("an expired grant is refused", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, EXPIRY);
	CHECK(origin.StateOf(id) == RequestState::Refused);
}

TEST_CASE("an origin with nothing published refuses everything", "[cdn][origin]") {
	ContentHash bundle = Hasher::Of(Bytes("anything"));
	Origin origin(Key());

	// An origin with nothing to serve refuses rather than serving nothing, and
	// the two are only distinguishable if that is stated.
	CHECK(origin.Current() == nullptr);
	CHECK_FALSE(origin.Publish(nullptr));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	CHECK(origin.StateOf(id) == RequestState::Refused);
}

TEST_CASE("a request is served against the publication it was admitted against", "[cdn][origin]") {
	Tree tree;
	ContentHash first;
	ContentHash second;
	Origin origin(Key());

	const auto original = Published(tree, first, "one");
	REQUIRE(origin.Publish(original));

	const RequestId id = origin.Submit(Token(Key(), first), first, NOW);
	REQUIRE(origin.StateOf(id) == RequestState::Pending);

	// The swap, mid-request. This is the whole point of publishing being atomic
	// rather than a mutation: a request already accepted keeps what it was
	// admitted against, and mutating a live publication would hand a client a
	// manifest naming chunks that are not there yet.
	REQUIRE(origin.Publish(Published(tree, second, "two")));
	CHECK(origin.Current() != original);

	CHECK(origin.Pump(Source()) == 1);
	CHECK(origin.StateOf(id) == RequestState::Ready);
	CHECK(origin.Take(id) != nullptr);
}

TEST_CASE("a cancelled request is never prepared", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Cancel(id));
	CHECK(origin.StateOf(id) == RequestState::Cancelled);

	// The absence of a cancel is what produces a game that hitches every time a
	// player turns around. A cancelled request costs no compression at all.
	CHECK(origin.Pump(Source()) == 0);
	CHECK(origin.Take(id) == nullptr);
}

TEST_CASE("a cancel is refused once a request has finished", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(Source()) == 1);

	// Cancelling a ready request would discard bytes a caller is entitled to.
	CHECK_FALSE(origin.Cancel(id));
	CHECK(origin.StateOf(id) == RequestState::Ready);

	CHECK_FALSE(origin.Cancel(RequestId{999}));
}

TEST_CASE("a second request for the same group is a cache hit", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId first = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(Source()) == 1);
	const PreparedFrame frameOne = origin.Take(first);
	REQUIRE(frameOne != nullptr);

	Metrics::Clear();
	const RequestId second = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(Source()) == 1);
	const PreparedFrame frameTwo = origin.Take(second);
	REQUIRE(frameTwo != nullptr);

	// Built once, streamed many times. Doing it per request would make an
	// origin's cost scale with its popularity rather than with its content,
	// which is exactly backwards.
	CHECK(frameTwo == frameOne);

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};
	CHECK(total("cdn.prepared.hit") == 1.0);
	CHECK(total("cdn.origin.prepared") == 0.0);
}

TEST_CASE("publishing clears the prepared cache", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(Source()) == 1);
	REQUIRE(origin.Take(id) != nullptr);
	REQUIRE(origin.Cache().Count() == 1);

	// The previous publication's groups were compressed against content and a
	// dictionary that are no longer current, and keeping them wastes the
	// capacity the new publication needs.
	ContentHash next;
	REQUIRE(origin.Publish(Published(tree, next, "two")));
	CHECK(origin.Cache().Count() == 0);
}

TEST_CASE("a bundle with no payload is refused rather than served empty", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	CHECK(origin.Pump(NoSource()) == 1);

	// An empty group is not a group. Serving one would hand a client bytes that
	// verify against nothing.
	CHECK(origin.StateOf(id) == RequestState::Refused);
	CHECK(origin.Take(id) == nullptr);
}

TEST_CASE("a pump prepares no more than its bound", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;

	CDNSettings settings;
	settings.PreparePerPump = 2;
	Origin origin(Key(), settings);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	std::vector<RequestId> ids;
	for (int index = 0; index < 5; ++index) {
		ids.push_back(origin.Submit(Token(Key(), bundle), bundle, NOW));
	}

	// A burst of requests must not make one pump run for an unbounded time and
	// starve whatever else the calling thread does.
	CHECK(origin.Pump(Source()) == 2);
	CHECK(origin.Pump(Source()) == 2);
	CHECK(origin.Pump(Source()) == 1);
	CHECK(origin.Pump(Source()) == 0);

	for (const RequestId id : ids) {
		CHECK(origin.StateOf(id) == RequestState::Ready);
	}
}

TEST_CASE("many groups prepare in one pump", "[cdn][origin]") {
	Tree tree;
	ContentHash bundle;

	CDNSettings settings;
	settings.PreparePerPump = 32;
	Origin origin(Key(), settings);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	// The one place a fan-out job is right here: compressing a known set of
	// groups is CPU work with a known end. Every one has to come back correct
	// whether the pool picked it up or the caller ran it inline.
	std::vector<RequestId> ids;
	for (int index = 0; index < 16; ++index) {
		ids.push_back(origin.Submit(Token(Key(), bundle), bundle, NOW));
	}

	CHECK(origin.Pump(Source()) == 16);
	for (const RequestId id : ids) {
		INFO("request " << id.Value);
		CHECK(origin.StateOf(id) == RequestState::Ready);
		CHECK(origin.Take(id) != nullptr);
	}
	CHECK(origin.Outstanding() == 0);
}

TEST_CASE("an unknown request is unknown", "[cdn][origin]") {
	Origin origin(Key());

	CHECK(origin.StateOf(RequestId{}) == RequestState::Unknown);
	CHECK(origin.StateOf(RequestId{12345}) == RequestState::Unknown);
	CHECK(origin.Take(RequestId{12345}) == nullptr);
	CHECK_FALSE(RequestId{}.IsValid());
}

TEST_CASE("invalid settings fall back to the defaults", "[cdn][origin]") {
	CDNSettings zeroed;
	zeroed.PreparePerPump = 0;

	// A pump that prepares nothing is an origin that never serves.
	const Origin origin(Key(), zeroed);
	CHECK(origin.Settings().PreparePerPump == CDNSettings{}.PreparePerPump);
	CHECK(CDNSettings{}.IsValid());
	CHECK_FALSE(zeroed.IsValid());
}

TEST_CASE("every request state has a name", "[cdn][origin]") {
	for (const RequestState state :
		 {RequestState::Unknown,
		  RequestState::Pending,
		  RequestState::Ready,
		  RequestState::Cancelled,
		  RequestState::Refused}) {
		CHECK(std::string(cdn::Describe(state)) != "?");
	}
}

TEST_CASE(
	"the pipeline reports itself to the frame graph and the metrics sink", "[cdn][origin][framegraph]"
) {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();

	const RequestId served = origin.Submit(Token(Key(), bundle), bundle, NOW);
	const RequestId refused = origin.Submit(Token(Key(200), bundle), bundle, NOW);
	const RequestId cancelled = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Cancel(cancelled));
	origin.Pump(Source());
	CHECK(origin.Take(served) != nullptr);
	CHECK(origin.StateOf(refused) == RequestState::Refused);

	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	CHECK(named("Origin::Submit"));
	CHECK(named("Origin::Pump"));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	CHECK(total("cdn.origin.published") == 0.0);
	CHECK(total("cdn.origin.refused") == 1.0);
	CHECK(total("cdn.origin.cancelled") == 1.0);
	CHECK(total("cdn.origin.prepared") == 1.0);
	CHECK(total("cdn.origin.served") == 1.0);
}

// ---------------------------------------------------------------------------
// Local first, then upstream — the cache-server deployment
// ---------------------------------------------------------------------------
//
// CDN.md §6 names three sources and this is where two of them meet: an origin
// that holds content of its own and can forward what it does not hold. Which
// one it is is a `CDNSettings` field rather than a different program.

namespace {
	// An upstream that answers, and counts how often it was asked.
	struct Upstream {
		size_t Asked = 0;
		std::vector<std::string> AskedBy;
		bool Answers = true;
		// Wrong length on purpose, for the verification case.
		bool Truthful = true;

		cdn::UpstreamFetch Fetch() {
			return [this](
					   const cdn::UpstreamOrigin &origin, const ContentHash &
				   ) -> std::optional<std::vector<std::byte>> {
				++Asked;
				AskedBy.push_back(origin.Name);
				if (!Answers) {
					return std::nullopt;
				}
				if (!Truthful) {
					return Bytes("not the length the manifest recorded");
				}
				return Bytes("rock-one");
			};
		}
	};

	// A local source that has nothing, so every request is a miss.
	PayloadSource EmptyLocal() {
		return [](const ContentHash &) -> std::optional<std::vector<std::byte>> { return std::nullopt; };
	}

	// The payload whose length matches what `Published` records for its bundle.
	PayloadSource TruthfulLocal() {
		return [](const ContentHash &) -> std::optional<std::vector<std::byte>> { return Bytes("rock-one"); };
	}

	CDNSettings CacheServer(std::vector<cdn::UpstreamOrigin> upstreams) {
		CDNSettings settings;
		settings.LocalFirst = true;
		settings.AllowUpstream = true;
		settings.Upstreams = std::move(upstreams);
		return settings;
	}
}

TEST_CASE("upstream is off by default", "[cdn][origin][upstream]") {
	// An origin that will fetch from elsewhere is an origin that can be pointed
	// at elsewhere, so turning it on is a deliberate act.
	CHECK_FALSE(CDNSettings{}.AllowUpstream);
	CHECK(CDNSettings{}.LocalFirst);
	CHECK(CDNSettings{}.CacheUpstream);
	CHECK(CDNSettings{}.VerifyUpstream);
}

TEST_CASE("forwarding with no upstreams configured is refused", "[cdn][origin][upstream]") {
	CDNSettings promises;
	promises.AllowUpstream = true;

	// Reads as "this will forward" and behaves as "this refuses every miss".
	CHECK_FALSE(promises.IsValid());

	CDNSettings none = CacheServer({{"edge", "http://example"}});
	none.UpstreamAttempts = 0;
	CHECK_FALSE(none.IsValid());

	CHECK(CacheServer({{"edge", "http://example"}}).IsValid());
}

TEST_CASE("a local hit never asks an upstream", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	Origin origin(Key(), CacheServer({{"edge", "http://example"}}));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(TruthfulLocal(), upstream.Fetch()) == 1);

	// The whole reason to deploy one of these next to a population of players:
	// a hit costs a lookup and no network at all.
	CHECK(upstream.Asked == 0);
	CHECK(origin.Take(id) != nullptr);
}

TEST_CASE("a local miss forwards upstream", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	Origin origin(Key(), CacheServer({{"edge", "http://example"}}));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);

	CHECK(upstream.Asked == 1);
	CHECK(origin.StateOf(id) == RequestState::Ready);
	CHECK(origin.Take(id) != nullptr);
}

TEST_CASE("a forwarded group is cached, so the second request is local", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	Origin origin(Key(), CacheServer({{"edge", "http://example"}}));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId first = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);
	REQUIRE(origin.Take(first) != nullptr);
	REQUIRE(upstream.Asked == 1);

	// This is the whole of "cache server". Without it, the origin fetches the
	// same bundle from the same upstream for every client that asks.
	const RequestId second = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);
	CHECK(upstream.Asked == 1);
	CHECK(origin.Take(second) != nullptr);
}

TEST_CASE("a pure proxy does not keep what it forwarded", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;

	CDNSettings proxy = CacheServer({{"edge", "http://example"}});
	proxy.CacheUpstream = false;
	Origin origin(Key(), proxy);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	for (int index = 0; index < 2; ++index) {
		const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
		REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);
		CHECK(origin.Take(id) != nullptr);
	}

	// A legitimate deployment and a different one: it forwards every time.
	CHECK(upstream.Asked == 2);
	CHECK(origin.Cache().Count() == 0);
}

TEST_CASE("upstream-first asks before looking locally", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;

	CDNSettings edge = CacheServer({{"edge", "http://example"}});
	edge.LocalFirst = false;
	Origin origin(Key(), edge);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(TruthfulLocal(), upstream.Fetch()) == 1);

	// An edge node holding no content of its own. Real, and not the default.
	CHECK(upstream.Asked == 1);
	CHECK(origin.Take(id) != nullptr);
}

TEST_CASE("upstream-first falls back to local when nobody answers", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	upstream.Answers = false;

	CDNSettings edge = CacheServer({{"edge", "http://example"}});
	edge.LocalFirst = false;
	Origin origin(Key(), edge);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(TruthfulLocal(), upstream.Fetch()) == 1);

	CHECK(origin.StateOf(id) == RequestState::Ready);
	CHECK(origin.Take(id) != nullptr);
}

TEST_CASE("upstreams are tried in order and bounded", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	upstream.Answers = false;

	CDNSettings many = CacheServer({{"first", "a"}, {"second", "b"}, {"third", "c"}});
	many.UpstreamAttempts = 2;
	Origin origin(Key(), many);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);

	// Bounded rather than "all of them": a request that walks ten dead upstreams
	// spends ten timeouts before refusing, and the client gave up long before.
	REQUIRE(upstream.Asked == 2);
	CHECK(upstream.AskedBy[0] == "first");
	CHECK(upstream.AskedBy[1] == "second");
	CHECK(origin.StateOf(id) == RequestState::Refused);
}

TEST_CASE("an upstream that answers wrongly is refused", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	upstream.Truthful = false;

	Origin origin(Key(), CacheServer({{"edge", "http://example"}}));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);

	// A proxy that forwards bytes it cannot check is a proxy that launders a
	// compromised upstream. The manifest is signed, so what it records the
	// bundle weighs is a fact rather than a hint from whoever answered.
	CHECK(origin.StateOf(id) == RequestState::Refused);
	CHECK(origin.Cache().Count() == 0);
}

TEST_CASE("verification can be turned off, and then it is not checked", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	upstream.Truthful = false;

	CDNSettings trusting = CacheServer({{"edge", "http://example"}});
	trusting.VerifyUpstream = false;
	Origin origin(Key(), trusting);
	REQUIRE(origin.Publish(Published(tree, bundle)));

	// Pinned so the flag is known to do something. Leaving it on is the advice;
	// a client verifies end to end regardless, which is why this is defence in
	// depth rather than the trust boundary.
	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);
	CHECK(origin.StateOf(id) == RequestState::Ready);
}

TEST_CASE("an origin with upstream off never forwards", "[cdn][origin][upstream]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;

	// The local-store deployment: the server serving its own disk.
	Origin origin(Key());
	REQUIRE(origin.Publish(Published(tree, bundle)));

	const RequestId id = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);

	CHECK(upstream.Asked == 0);
	CHECK(origin.StateOf(id) == RequestState::Refused);
}

TEST_CASE("where a payload came from is counted", "[cdn][origin][upstream][metrics]") {
	Tree tree;
	ContentHash bundle;
	Upstream upstream;
	Origin origin(Key(), CacheServer({{"edge", "http://example"}}));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	Metrics::Clear();
	const RequestId local = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(TruthfulLocal(), upstream.Fetch()) == 1);
	REQUIRE(origin.Take(local) != nullptr);
	origin.Cache().Clear();

	const RequestId forwarded = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), upstream.Fetch()) == 1);
	REQUIRE(origin.Take(forwarded) != nullptr);

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	// A local hit and a forward are different costs and want different fixes —
	// one is a warm cache, the other is a network round trip per request.
	CHECK(total("cdn.local.served") == 1.0);
	CHECK(total("cdn.upstream.served") == 1.0);
}

TEST_CASE("an upstream that fails and one that lies are counted apart", "[cdn][origin][upstream][metrics]") {
	Tree tree;
	ContentHash bundle;
	Origin origin(Key(), CacheServer({{"edge", "http://example"}}));
	REQUIRE(origin.Publish(Published(tree, bundle)));

	Upstream silent;
	silent.Answers = false;
	Upstream liar;
	liar.Truthful = false;

	Metrics::Clear();
	const RequestId a = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), silent.Fetch()) == 1);
	const RequestId b = origin.Submit(Token(Key(), bundle), bundle, NOW);
	REQUIRE(origin.Pump(EmptyLocal(), liar.Fetch()) == 1);

	CHECK(origin.StateOf(a) == RequestState::Refused);
	CHECK(origin.StateOf(b) == RequestState::Refused);

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	// An upstream that is down and an upstream that is serving wrong content
	// are not the same incident, and one counter for both would bury the second.
	CHECK(total("cdn.upstream.failed") == 1.0);
	CHECK(total("cdn.upstream.rejected") == 1.0);
}
