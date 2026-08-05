// A thin main over the cdn library, for the same reason the client's and the
// server's are thin: the test binary needs something to link, and a server
// serving its own assets links this library in-process rather than starting a
// second program. repo_layout.md §2, §11.
//
// **It serves, as of v0.9.** The warning this file carried since v0.2 — that
// nothing was served because the manifest and the HTTP layer did not exist — is
// gone with the thing it was warning about.
//
// Two modes, and they are deliberately one program:
//
//     cdn --publish CONTENT --store DIR --key HEX   build a store from files
//     cdn --store DIR --port 9080                   serve one
//
// Publishing is separate from serving because the signing key is separate.
// `assets/AGENTS.md` records the convention: a key belongs to whoever publishes
// the game, and **the origin holds none** — that is what makes it safe to
// deploy on hardware nobody here owns. A single mode that published on start-up
// would put a signing key on every serving box, permanently.
//
// The three deployments CDN.md §6 names are flag combinations rather than three
// programs, exactly as `CDNSettings` is one type rather than three.

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/delivery/Source.hpp>

#include <cdn/Origin.hpp>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {
	// Wall time, read in exactly one place in this program.
	//
	// Everything below takes `nowSeconds` as an argument — `assets::Grant` so
	// that neither end holds a clock of its own to drift, `net` so that a suite
	// can state a timeout rather than sleep for one. A program eventually has
	// to read a real clock, and this is the line that does.
	uint64_t NowSeconds() {
		using namespace std::chrono;
		return static_cast<uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
	}

	// How long a pump that did nothing waits before pumping again.
	//
	// The socket is non-blocking and polled, so without this the loop is a busy
	// spin that costs a core to serve nothing. Ten milliseconds is chosen
	// rather than measured: it is far below any request's own latency and far
	// above the cost of a poll.
	constexpr auto IDLE_SLEEP = std::chrono::milliseconds(10);

	std::optional<engine::assets::GrantKey> KeyFromHex(std::string_view text) {
		const std::string hex(text);
		if (hex.size() != engine::assets::GrantKey::BYTES * 2) {
			return std::nullopt;
		}
		std::array<std::byte, engine::assets::GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			const std::string byte = hex.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				return std::nullopt;
			}
			secret[index] = static_cast<std::byte>(value);
		}
		return engine::assets::GrantKey::FromSecret(secret);
	}

	std::optional<engine::assets::SigningKey> SigningKeyFromHex(std::string_view text) {
		const std::string hex(text);
		if (hex.size() != 64) {
			return std::nullopt;
		}
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); ++index) {
			const std::string byte = hex.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				return std::nullopt;
			}
			seed[index] = static_cast<std::byte>(value);
		}
		return engine::assets::SigningKey::FromSeed(seed);
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("cdn");

	engine::core::Arguments arguments("cdn", "atomic — serves a game's content.");

	arguments.Flag("verbose", "Log at trace level");
	arguments.Value(
		"store", "DIR", "The content store to serve or publish into (default: beside the binary)"
	);
	arguments.Value("publish", "DIR", "Publish this directory of files into the store, then exit");
	arguments.Value("signing-key", "HEX", "64 hex characters — the Ed25519 seed to sign a publish with");
	arguments.Value(
		"grant-key", "HEX", "64 hex characters — the secret shared with the server that issues grants"
	);
	arguments.Value("port", "N", "Port to listen on (default: 9080; 0 binds an ephemeral one)");
	arguments.Value("upstream", "NAME=HOST:PORT", "An origin to forward a miss to. Repeatable");
	arguments.Flag("allow-upstream", "Forward a miss to an upstream. Off unless asked for");
	arguments.Flag("no-local-first", "Always ask an upstream, even when the content is here — a pure proxy");
	arguments.Flag("no-cache-upstream", "Do not keep what an upstream returned");
	arguments.Value("compression-level", "N", "Zstd level groups are prepared at (default: 9)");
	arguments.Value("cache-bytes", "N", "What the prepared-group cache may hold");
	arguments.Value("frames", "N", "Serve this many pumps and exit. For a smoke test");

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}
	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}

	// Beside the binary by default, because the staged cdn/ directory is
	// runnable as it stands and the self-hosted case is a directory that ships
	// with it. The working directory is whoever launched the process.
	std::filesystem::path storePath = engine::core::Paths::Assets();
	if (auto chosen = arguments.Get("store")) {
		storePath = std::filesystem::path(*chosen);
	}

	// --- publishing ---------------------------------------------------------

	if (auto content = arguments.Get("publish")) {
		const auto seed = arguments.Get("signing-key");
		if (!seed) {
			ENGINE_ERROR("cdn: --publish needs --signing-key, and it is not optional");
			ENGINE_ERROR("cdn: a manifest nobody signed is a manifest no client can trust — CDN.md §2");
			return 2;
		}
		const auto key = SigningKeyFromHex(*seed);
		if (!key) {
			ENGINE_ERROR("cdn: --signing-key must be 64 lowercase hex characters");
			return 2;
		}

		const auto report = cdn::Publish(std::filesystem::path(*content), storePath, *key);
		if (!report) {
			return 1;
		}

		ENGINE_INFO(
			"cdn: published {} assets in {} bundles — {} bytes of content in {} bytes of chunks",
			report->Assets,
			report->Bundles,
			report->ContentBytes,
			report->StoredBytes
		);
		ENGINE_INFO("cdn: manifest root {}", report->Root.ToHex());
		ENGINE_INFO("cdn: publisher key {}", key->Public().ToHex());
		if (!report->DictionaryTrained) {
			ENGINE_INFO("cdn: no dictionary — groups will be compressed without one");
		}
		if (report->Oversized > 0) {
			// Said rather than hidden: a bound quietly broken reads as a bound
			// that held, and the first anyone hears of it is a client stalling.
			ENGINE_WARN(
				"cdn: {} groups are over the size ceiling because one affinity is", report->Oversized
			);
		}
		return 0;
	}

	// --- serving ------------------------------------------------------------

	auto store = engine::assets::ChunkStore::Open(storePath, false);
	if (!store) {
		ENGINE_ERROR("cdn: no content store at {}", storePath.string());
		ENGINE_ERROR(
			"cdn: publish one first — cdn --publish DIR --store {} --signing-key HEX", storePath.string()
		);
		return 1;
	}

	engine::assets::SignatureBytes signature;
	auto manifest = store->ReadManifest(signature);
	if (!manifest) {
		ENGINE_ERROR("cdn: {} holds no manifest", storePath.string());
		return 1;
	}

	const auto grantSecret = arguments.Get("grant-key");
	if (!grantSecret) {
		ENGINE_ERROR("cdn: --grant-key is required — it is the secret shared with the server");
		ENGINE_ERROR(
			"cdn: an origin that admitted everyone would be deciding who may have what, "
			"which is the server's job — CDN.md §4"
		);
		return 2;
	}
	auto grantKey = KeyFromHex(*grantSecret);
	if (!grantKey) {
		ENGINE_ERROR(
			"cdn: --grant-key must be {} lowercase hex characters", engine::assets::GrantKey::BYTES * 2
		);
		return 2;
	}

	cdn::CDNSettings settings;
	settings.LocalFirst = !arguments.Has("no-local-first");
	settings.AllowUpstream = arguments.Has("allow-upstream");
	settings.CacheUpstream = !arguments.Has("no-cache-upstream");
	if (auto level = arguments.Get("compression-level")) {
		settings.CompressionLevel = std::atoi(std::string(*level).c_str());
	}
	if (auto capacity = arguments.Get("cache-bytes")) {
		settings.CacheCapacityBytes = std::strtoull(std::string(*capacity).c_str(), nullptr, 10);
	}
	for (const std::string_view upstream : arguments.GetAll("upstream")) {
		const size_t equals = upstream.find('=');
		if (equals == std::string::npos) {
			ENGINE_ERROR("cdn: --upstream wants NAME=HOST:PORT, got '{}'", std::string(upstream));
			return 2;
		}
		settings.Upstreams.push_back(
			cdn::UpstreamOrigin{
				.Name = std::string(upstream.substr(0, equals)),
				.Endpoint = std::string(upstream.substr(equals + 1)),
			}
		);
	}
	if (!settings.IsValid()) {
		// Forwarding with no upstreams reads as "this will forward" and behaves
		// as "this refuses every miss", and the gap between those is a
		// deployment that looks healthy and serves nothing.
		ENGINE_ERROR("cdn: --allow-upstream with no --upstream would refuse every miss");
		return 2;
	}

	cdn::Origin origin(std::move(*grantKey), settings);
	if (!origin.Publish(
			std::make_shared<const cdn::Publication>(
				*cdn::ContentRoot::Mount(storePath), std::move(*manifest)
			)
		)) {
		ENGINE_ERROR("cdn: could not publish the manifest");
		return 1;
	}

	cdn::ServiceSettings service;
	service.Port = engine::delivery::DEFAULT_ORIGIN_PORT;
	if (auto port = arguments.Get("port")) {
		service.Port = static_cast<uint16_t>(std::atoi(std::string(*port).c_str()));
	}

	std::unique_ptr<cdn::Service> serving = cdn::Serve(origin, std::move(*store), service);
	if (!serving) {
		return 1;
	}

	// A bounded run for a smoke test, unbounded otherwise. Bounded rather than
	// "run for N seconds" so the check is a count a test can state instead of a
	// duration it has to wait out.
	long frames = -1;
	if (auto limit = arguments.Get("frames")) {
		frames = std::atol(std::string(*limit).c_str());
	}

	ENGINE_INFO(
		"cdn: local-first {}, upstream {}",
		settings.LocalFirst ? "on" : "off",
		settings.AllowUpstream ? "on" : "off"
	);

	for (long frame = 0; frames < 0 || frame < frames; ++frame) {
		if (serving->Pump(NowSeconds()) == 0) {
			std::this_thread::sleep_for(IDLE_SLEEP);
		}
	}

	const cdn::ServiceCounters &counters = serving->Counters();
	ENGINE_INFO(
		"cdn: served {} bundles ({} bytes), refused {}, missing {}",
		counters.Bundles,
		counters.ServedBytes,
		counters.Refused,
		counters.Missing
	);
	return 0;
}
