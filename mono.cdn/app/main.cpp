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

#include <cdn/Dashboard.hpp>
#include <cdn/Origin.hpp>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <cdn/Stream.hpp>
#include <cdn/Terminal.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
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

	// The dashboard's clock, and it is deliberately not the one above.
	//
	// A grant is checked against wall time because the server that issued it
	// used wall time. A *rate* must not be: a clock that steps back an hour
	// would put an hour of traffic into one bucket and draw a spike nothing
	// caused. `steady_clock` cannot step, so the only thing this loses is the
	// ability to say what o'clock a bucket was, which nothing here displays.
	uint64_t NowMilliseconds() {
		using namespace std::chrono;
		return static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
		);
	}

	// How often the dashboard is redrawn.
	//
	// Four a second: fast enough that a rate looks live, slow enough that the
	// redraw is nothing next to the serving it is watching. Sampling happens
	// every pump regardless — the history is built from differences, and
	// sampling at the redraw rate would put traffic in the wrong minute at
	// every bucket boundary.
	constexpr uint64_t REDRAW_MILLISECONDS = 250;

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
	arguments.Value(
		"ingest-key", "SECRET", "Accept uploads at /ingest from whoever sends this as x-atomic-ingest"
	);
	arguments.Value("inbox", "DIR", "Where uploads land (default: the store's raw/ beside --store)");
	arguments.Value("upstream", "NAME=HOST:PORT", "An origin to forward a miss to. Repeatable");
	arguments.Flag("allow-upstream", "Forward a miss to an upstream. Off unless asked for");
	arguments.Flag("no-local-first", "Always ask an upstream, even when the content is here — a pure proxy");
	arguments.Flag("no-cache-upstream", "Do not keep what an upstream returned");
	arguments.Value("compression-level", "N", "Zstd level groups are prepared at (default: 9)");
	arguments.Value("cache-bytes", "N", "What the prepared-group cache may hold");
	arguments.Flag(
		"advertise", "Announce this origin on the local subnet so clients find it without an address"
	);
	arguments.Value("stream-name", "NAME", "What to call this distribution stream");
	arguments.Value(
		"stream-key",
		"SECRET",
		"Make this a private stream: 64 hex characters, or a passphrase. Gates discovery, not delivery"
	);
	arguments.Value("rendezvous", "HOST:PORT", "Register this stream with a rendezvous point");
	arguments.Value("rendezvous-listen", "PORT", "Run a rendezvous point here, so peers can find each other");
	arguments.Value("frames", "N", "Serve this many pumps and exit. For a smoke test");
	arguments.Flag("gui", "Watch it serve in the terminal — content, traffic and rates");

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

	// **Uploads are off until a key is given, and the key is the whole of the
	// admission check.** `cdn::IngestSettings` carries what that buys and what
	// it does not: an inbox holds unsigned content that no client will look at
	// until a publisher has signed a manifest naming it, so losing this secret
	// costs disk rather than trust.
	//
	// Reachable from a flag as of v0.14. The origin has accepted uploads since
	// v0.10 and no program could be told to — so the editor's Upload button had
	// a write source it could configure, a key it could send, and nothing
	// anywhere that would take them.
	if (auto key = arguments.Get("ingest-key"); key.has_value() && !key->empty()) {
		service.Ingest.Key = std::string(*key);

		// **A store called `processed/` is a local store, and its inbox is the
		// `raw/` beside it** — which is where `PublishLocal` reads from, so an
		// upload lands somewhere a publish will find it. Anywhere else the
		// store is a bare directory of chunks with no such sibling, and putting
		// an inbox *inside* it is at least a path somebody can predict.
		service.Ingest.Inbox = arguments.Get("inbox").has_value()
								   ? std::filesystem::path(*arguments.Get("inbox"))
							   : storePath.filename() == "processed"
								   ? storePath.parent_path() / "raw"
								   : storePath / "inbox";
	} else if (arguments.Get("inbox").has_value()) {
		// An inbox with no key refuses every upload — `IngestSettings::Key`
		// says so — and an origin that looks configured for uploads and takes
		// none is the failure worth naming here rather than at request time.
		ENGINE_ERROR("cdn: --inbox needs --ingest-key, or nothing would be accepted");
		return 2;
	}

	// Read before the store is handed over, and read once: counting chunks
	// walks every fan-out directory in the tree, which is a start-up cost and
	// would be a per-redraw one.
	const cdn::StoreFootprint footprint{store->Bytes(), store->Count()};

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

	// --- the distribution stream, when one was asked for ---------------------

	std::unique_ptr<cdn::Stream> stream;
	if (arguments.Has("advertise") || arguments.Has("rendezvous") || arguments.Has("rendezvous-listen")) {
		cdn::StreamSettings offering;
		offering.Announce = arguments.Has("advertise");
		if (auto point = arguments.Get("rendezvous")) {
			offering.RendezvousAddress = std::string(*point);
		}
		if (auto listen = arguments.Get("rendezvous-listen")) {
			offering.RendezvousListenPort = static_cast<uint16_t>(std::atoi(std::string(*listen).c_str()));
		}
		if (auto secret = arguments.Get("stream-key")) {
			offering.Secret = std::string(*secret);
		}
		if (auto name = arguments.Get("stream-name")) {
			offering.Name = std::string(*name);
		}
		// The manifest this origin is currently serving, which is what somebody
		// looking at a list of streams needs in order to tell two apart.
		offering.Detail = origin.Current() != nullptr
							  ? origin.Current()->Contents().Root().ToHex().substr(0, 12)
							  : std::string();
		// **The port that was bound**, which is not the port that was
		// configured when it was zero.
		offering.Port = serving->Local().Port;

		std::string trouble;
		stream = cdn::Stream::Open(offering, trouble);
		if (!stream) {
			ENGINE_ERROR("cdn: {}", trouble);
			return 1;
		}
		if (stream->Announcing()) {
			ENGINE_INFO(
				"cdn: announcing stream {} ({}) on {}",
				stream->Advertised().Session.Text(),
				network::Describe(stream->Advertised().Admits),
				serving->Local().Text()
			);
		}
	}

	// --- the dashboard, when one was asked for -------------------------------

	std::unique_ptr<cdn::Terminal> screen;
	std::optional<cdn::Dashboard> dashboard;
	cdn::Viewport view;
	std::string typed;
	uint64_t drawnAtMilliseconds = 0;

	if (arguments.Has("gui")) {
		screen = cdn::Terminal::Open();
		if (!screen) {
			// Not a terminal: a pipe, a log file, a service manager. Serving is
			// the job and the dashboard is not, so this says so and carries on
			// rather than refusing to start.
			ENGINE_WARN("cdn: --gui needs a terminal on stdin and stdout — serving without it");
		} else {
			dashboard.emplace(*origin.Current(), footprint, serving->Local().Text());
			// The log and the dashboard share one screen and the log would win,
			// a line at a time, in the middle of a frame. Errors still get
			// through, because a frame with a stray line in it is better than an
			// origin that has stopped serving and cannot say so.
			engine::core::Log::SetLevel(engine::core::LogLevel::Error);
		}
	}

	for (long frame = 0; frames < 0 || frame < frames; ++frame) {
		const size_t answered = serving->Pump(NowSeconds());

		// Beside the content pump rather than on a thread of its own: an
		// announcement is one datagram a second and a rendezvous point is a
		// table lookup, and a second thread would buy nothing but a place for
		// two clocks to disagree.
		if (stream) {
			stream->Pump(NowSeconds());
		}

		if (dashboard) {
			const uint64_t nowMilliseconds = NowMilliseconds();
			const cdn::CacheUsage cache{
				origin.Cache().Bytes(), origin.Cache().Count(), origin.Cache().Capacity()
			};
			dashboard->Sample(serving->Counters(), cache, nowMilliseconds);

			const cdn::ScreenSize size = screen->Size();
			const size_t visibleRows = size.Rows > 1 ? size.Rows - 1 : 1;

			// Whatever is not a whole keypress yet stays in the buffer: an
			// arrow key is three bytes and they do not always arrive together.
			typed += screen->Read();
			bool leaving = false;
			for (;;) {
				const cdn::KeyPress press = cdn::DecodeKey(typed);
				if (press.Consumed == 0) {
					break;
				}
				typed.erase(0, press.Consumed);
				if (press.Pressed == cdn::Key::Quit) {
					leaving = true;
					break;
				}
				view.Apply(press.Pressed, dashboard->Lines(), visibleRows);
			}
			if (leaving) {
				break;
			}

			if (nowMilliseconds - drawnAtMilliseconds >= REDRAW_MILLISECONDS) {
				drawnAtMilliseconds = nowMilliseconds;
				screen->Present(cdn::RenderFrame(*dashboard, view, size));
			}
		}

		if (answered == 0) {
			std::this_thread::sleep_for(IDLE_SLEEP);
		}
	}

	if (screen) {
		// Put the terminal back before anything else is printed, so the summary
		// below lands on the operator's own screen rather than on one that is
		// about to be thrown away with the alternate buffer.
		screen->Close();
		engine::core::Log::SetLevel(
			arguments.Has("verbose") ? engine::core::LogLevel::Trace : engine::core::LogLevel::Info
		);
	}

	const cdn::ServiceCounters &counters = serving->Counters();
	ENGINE_INFO(
		"cdn: served {} bundles ({} bytes), refused {}, missing {}",
		counters.Bundles,
		counters.ServedBytes,
		counters.Refused,
		counters.Missing
	);
	ENGINE_INFO("cdn: {} bytes out, {} bytes in", counters.SentBytes, counters.ReceivedBytes);
	return 0;
}
