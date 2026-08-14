// Content going the other way: an origin that accepts writes, and the uploader
// that fills it.
//
// **In `mono.cdn` for `Delivery.cpp`'s reason** - it needs both halves and this
// is the side that links the other. Nothing is mocked: a real listener on a real
// port, a real `PUT` across a socket, and the file checked on disk afterwards.
//
// What it is really pinning is that the two ends agree about the things each
// defines once and both rely on:
//
// - the route, `/ingest/<hash>`, and the two headers that go with it
// - that the *body* decides the filename and nothing either side typed does
// - that a read-only origin is indistinguishable from one with no such route
// - that the split is a configuration and not two programs

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/Origin.hpp>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

TEST_SUITE_ID("cdn.ingest")
TEST_DEPENDS("cdn.service")
TEST_DEPENDS("engine.delivery.source")
TEST_DEPENDS("engine.net.http.message")

using cdn::IngestSettings;
using cdn::Origin;
using cdn::Publication;
using cdn::Service;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::GrantKey;
using engine::assets::Hasher;
using engine::assets::SigningKey;
using engine::delivery::DeliverySettings;
using engine::delivery::MakeUploader;
using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::delivery::SourceRole;
using engine::delivery::Uploader;
using engine::delivery::UploadOutcome;

namespace {
	namespace fs = std::filesystem;

	constexpr int MAXIMUM_POLLS = 40000;
	constexpr const char *KEY = "an-ingest-secret";

	GrantKey Secret() {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(7 + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	SigningKey Publisher() {
		std::array<std::byte, 32> seed{};
		seed.fill(std::byte{21});
		auto key = SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		return {
			reinterpret_cast<const std::byte *>(text.data()),
			reinterpret_cast<const std::byte *>(text.data()) + text.size()
		};
	}

	fs::path WriteFile(const fs::path &where, std::string_view text) {
		fs::create_directories(where.parent_path());
		std::ofstream file(where, std::ios::binary);
		file << text;
		return where;
	}

	// An origin on a port, with writes on or off.
	//
	// It serves an empty published store, because nothing here fetches: what is
	// under test is the inbox, and a publication only exists so `Origin` has
	// one.
	struct Host {
		fs::path Root;
		std::unique_ptr<Origin> Serving;
		std::unique_ptr<Service> Listening;
		engine::net::Endpoint Address;
		fs::path Inbox;

		explicit Host(const char *name, bool accepts, uint64_t ceiling = 16ull * 1024u * 1024u) {
			static int serial = 0;
			Root =
				fs::temp_directory_path() / ("atomic-ingest-" + std::string(name) + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);

			Inbox = Root / "inbox";
			WriteFile(Root / "content" / "seed.txt", "a store has to have something in it");
			REQUIRE(cdn::Publish(Root / "content", Root / "store", Publisher()).has_value());

			auto store = ChunkStore::Open(Root / "store", false);
			REQUIRE(store.has_value());
			engine::assets::SignatureBytes signature;
			const auto catalogue = store->ReadManifest(signature);
			REQUIRE(catalogue.has_value());

			Serving = std::make_unique<Origin>(Secret());
			auto mounted = cdn::ContentRoot::Mount(Root / "store");
			REQUIRE(mounted.has_value());
			REQUIRE(Serving->Publish(std::make_shared<const Publication>(*mounted, *catalogue)));

			cdn::ServiceSettings settings;
			settings.Port = 0;
			if (accepts) {
				settings.Ingest.Inbox = Inbox;
				settings.Ingest.Key = KEY;
				settings.Ingest.MaximumFileBytes = ceiling;
			}

			Listening = cdn::Serve(*Serving, std::move(*store), settings);
			REQUIRE(Listening != nullptr);
			Address = engine::net::Endpoint::LoopbackIPv4(Listening->Local().Port);
		}

		~Host() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		Host(const Host &) = delete;
		Host &operator=(const Host &) = delete;

		Source Descriptor(SourceRole role = SourceRole::Write, const char *key = KEY) const {
			return Source{
				.Name = "write-origin",
				.Kind = SourceKind::Http,
				.Location = Address.Text(),
				.Enabled = true,
				.Role = role,
				.IngestKey = key,
			};
		}

		// Drives both ends until the uploader has nothing left.
		//
		// **Both**, because neither moves on its own: the service answers
		// inside its own pump and the uploader reads inside its own, so a loop
		// that turned one crank would deadlock against the other.
		std::vector<UploadOutcome> Drain(Uploader &uploader) {
			std::vector<UploadOutcome> outcomes;
			for (int poll = 0; poll < MAXIMUM_POLLS && uploader.Remaining() > 0; poll++) {
				Listening->Pump(1000);
				uploader.Pump();
				for (UploadOutcome &outcome : uploader.Take()) {
					outcomes.push_back(std::move(outcome));
				}
			}
			for (UploadOutcome &outcome : uploader.Take()) {
				outcomes.push_back(std::move(outcome));
			}
			CHECK(uploader.Remaining() == 0);
			return outcomes;
		}
	};

	std::unique_ptr<Uploader> UploaderFor(const Source &source) {
		DeliverySettings settings;
		settings.Sources.push_back(source);
		return MakeUploader(settings);
	}
}

TEST_CASE("a file uploaded over HTTP lands in the inbox under its own hash", "[cdn][ingest]") {
	Host host("basic", true);

	const fs::path source = WriteFile(host.Root / "out" / "diffuse.png", "some pixels");
	const ContentHash expected = Hasher::Of(Bytes("some pixels"));

	std::unique_ptr<Uploader> uploader = UploaderFor(host.Descriptor());
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(source));

	const std::vector<UploadOutcome> outcomes = host.Drain(*uploader);
	REQUIRE(outcomes.size() == 1);
	CHECK(outcomes[0].Delivered);
	CHECK(outcomes[0].Detail == "stored");
	CHECK(outcomes[0].Root == expected);

	// **The name on disk is the hash, and the extension came along.** The
	// extension is not decoration: `Publish` derives an asset's kind from the
	// name through `KindOfName`, so a file that landed without it would publish
	// as `Unknown` and no subsystem would claim it.
	CHECK(fs::exists(host.Inbox / (expected.ToHex() + ".png")));

	CHECK(uploader->Counters().Stored == 1);
	CHECK(uploader->Counters().SentBytes == 11);
	CHECK(host.Listening->Counters().Ingested == 1);
	CHECK(host.Listening->Counters().IngestedBytes == 11);
}

TEST_CASE("uploading the same bytes twice costs a probe and not a transfer", "[cdn][ingest]") {
	Host host("duplicate", true);

	const fs::path first = WriteFile(host.Root / "out" / "a.txt", "identical");

	// **A second file with the same bytes and a different name.** The bytes are
	// the identity, so this is the same upload - the property that makes
	// re-uploading a content tree cheap, and the one an operator is watching
	// `IngestDuplicates` for.
	const fs::path second = WriteFile(host.Root / "out" / "b.txt", "identical");

	std::unique_ptr<Uploader> uploader = UploaderFor(host.Descriptor());
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(first));
	REQUIRE(uploader->Add(second));

	const std::vector<UploadOutcome> outcomes = host.Drain(*uploader);
	REQUIRE(outcomes.size() == 2);
	CHECK(outcomes[0].Detail == "stored");
	CHECK(outcomes[1].Detail == "already there");

	// Both delivered: "already there" is a success, not a failure.
	CHECK(outcomes[0].Delivered);
	CHECK(outcomes[1].Delivered);

	CHECK(uploader->Counters().Stored == 1);
	CHECK(uploader->Counters().Skipped == 1);

	// The second never sent a body, which is the whole point of the probe.
	CHECK(uploader->Counters().SentBytes == 9);
	CHECK(host.Listening->Counters().Ingested == 1);
	CHECK(host.Listening->Counters().IngestedBytes == 9);
}

TEST_CASE("an origin with writes off answers as though the route were not there", "[cdn][ingest]") {
	// **The default, and the reason it is the default.** An origin reachable on
	// a network that accepts writes because a field was left blank is an open
	// dropbox - so no key means no writes rather than no check.
	Host host("readonly", false);

	const fs::path source = WriteFile(host.Root / "out" / "a.txt", "not going anywhere");

	std::unique_ptr<Uploader> uploader = UploaderFor(host.Descriptor());
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(source));

	const std::vector<UploadOutcome> outcomes = host.Drain(*uploader);
	REQUIRE(outcomes.size() == 1);
	CHECK_FALSE(outcomes[0].Delivered);
	CHECK(uploader->Counters().Refused == 1);
	CHECK(uploader->Counters().Stored == 0);

	CHECK_FALSE(fs::exists(host.Inbox));
	CHECK(host.Listening->Counters().Ingested == 0);
	CHECK(host.Listening->Counters().IngestRefused > 0);
}

TEST_CASE("a wrong ingest key is refused and stores nothing", "[cdn][ingest]") {
	Host host("wrongkey", true);

	const fs::path source = WriteFile(host.Root / "out" / "a.txt", "nope");

	std::unique_ptr<Uploader> uploader = UploaderFor(host.Descriptor(SourceRole::Write, "not-the-secret"));
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(source));

	const std::vector<UploadOutcome> outcomes = host.Drain(*uploader);
	REQUIRE(outcomes.size() == 1);
	CHECK_FALSE(outcomes[0].Delivered);
	CHECK(uploader->Counters().Refused == 1);
	CHECK(host.Listening->Counters().Ingested == 0);

	// **A key that is a prefix of the real one is refused too**, which is what
	// the constant-time compare's length check is for.
	std::unique_ptr<Uploader> prefix = UploaderFor(host.Descriptor(SourceRole::Write, "an-ingest"));
	REQUIRE(prefix != nullptr);
	REQUIRE(prefix->Add(source));
	const std::vector<UploadOutcome> again = host.Drain(*prefix);
	REQUIRE(again.size() == 1);
	CHECK_FALSE(again[0].Delivered);
}

TEST_CASE("a file past the origin's ceiling is refused with a reason", "[cdn][ingest]") {
	// **`413` and not a dropped socket**, which is what raising the connection
	// buffer alongside the body limit buys: an uploader is told to split the
	// file rather than being told the network failed.
	Host host("toolarge", true, 64);

	const fs::path source = WriteFile(host.Root / "out" / "big.bin", std::string(4096, 'x'));

	std::unique_ptr<Uploader> uploader = UploaderFor(host.Descriptor());
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(source));

	const std::vector<UploadOutcome> outcomes = host.Drain(*uploader);
	REQUIRE(outcomes.size() == 1);
	CHECK_FALSE(outcomes[0].Delivered);
	CHECK(outcomes[0].Detail == "larger than this origin accepts");
	CHECK(host.Listening->Counters().Ingested == 0);
}

TEST_CASE("a directory write source is filled without a socket", "[cdn][ingest]") {
	// Writing to a directory source is writing to a filesystem this process
	// already has, so there is no key, no probe and no round trip.
	const fs::path root = fs::temp_directory_path() / "atomic-ingest-directory";
	std::error_code failure;
	fs::remove_all(root, failure);

	const fs::path source = WriteFile(root / "out" / "clip.wav", "samples");
	const ContentHash expected = Hasher::Of(Bytes("samples"));

	std::unique_ptr<Uploader> uploader = UploaderFor(
		Source{
			.Name = "on-disk",
			.Kind = SourceKind::Directory,
			.Location = (root / "inbox").string(),
			.Enabled = true,
			.Role = SourceRole::Write,
			.IngestKey = {},
		}
	);
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(source));

	// One pump finishes it, because nothing is in flight.
	CHECK(uploader->Pump() == 1);
	CHECK(uploader->Remaining() == 0);
	CHECK(uploader->Counters().Stored == 1);
	CHECK(fs::exists(root / "inbox" / (expected.ToHex() + ".wav")));

	fs::remove_all(root, failure);
}

TEST_CASE("a read source is never uploaded to and a write source is never fetched from", "[cdn][ingest]") {
	// The whole of "one server takes the writes, another serves the reads", and
	// the only place both halves of it are stated together.
	DeliverySettings settings;
	settings.Sources.push_back(
		Source{
			.Name = "reader",
			.Kind = SourceKind::Http,
			.Location = "127.0.0.1:9080",
			.Enabled = true,
			.Role = SourceRole::Read,
			.IngestKey = "would-be-ignored",
		}
	);
	settings.Sources.push_back(
		Source{
			.Name = "writer",
			.Kind = SourceKind::Http,
			.Location = "127.0.0.1:9081",
			.Enabled = true,
			.Role = SourceRole::Write,
			.IngestKey = KEY,
		}
	);

	const std::vector<Source> readable = settings.Usable();
	REQUIRE(readable.size() == 1);
	CHECK(readable[0].Name == "reader");

	const std::vector<Source> writable = settings.Writable();
	REQUIRE(writable.size() == 1);
	CHECK(writable[0].Name == "writer");

	// **A write source with no key is not writable**, so a half-filled row does
	// not silently become an upload that will be refused at the far end.
	settings.Sources[1].IngestKey.clear();
	CHECK(settings.Writable().empty());
	CHECK(MakeUploader(settings) == nullptr);
}

TEST_CASE("two write destinations both receive every file", "[cdn][ingest]") {
	// **The opposite of a fetch, and the right opposite.** A fetch stops at the
	// first source that answers because it wants one copy of the bytes; a
	// publish wants each write origin to end up holding them.
	Host first("mirror-a", true);
	Host second("mirror-b", true);

	const fs::path source = WriteFile(first.Root / "out" / "shared.txt", "to both");
	const ContentHash expected = Hasher::Of(Bytes("to both"));

	DeliverySettings settings;
	Source one = first.Descriptor();
	one.Name = "first";
	Source two = second.Descriptor();
	two.Name = "second";
	settings.Sources.push_back(one);
	settings.Sources.push_back(two);

	std::unique_ptr<Uploader> uploader = MakeUploader(settings);
	REQUIRE(uploader != nullptr);
	REQUIRE(uploader->Add(source));

	// Both origins have to be pumped, so this cannot use either host's Drain.
	std::vector<UploadOutcome> outcomes;
	for (int poll = 0; poll < MAXIMUM_POLLS && uploader->Remaining() > 0; poll++) {
		first.Listening->Pump(1000);
		second.Listening->Pump(1000);
		uploader->Pump();
		for (UploadOutcome &outcome : uploader->Take()) {
			outcomes.push_back(std::move(outcome));
		}
	}
	for (UploadOutcome &outcome : uploader->Take()) {
		outcomes.push_back(std::move(outcome));
	}

	REQUIRE(outcomes.size() == 2);
	CHECK(outcomes[0].Delivered);
	CHECK(outcomes[1].Delivered);
	CHECK(uploader->Counters().Stored == 2);

	CHECK(fs::exists(first.Inbox / (expected.ToHex() + ".txt")));
	CHECK(fs::exists(second.Inbox / (expected.ToHex() + ".txt")));
}

TEST_CASE("what an origin ingests can then be published and fetched", "[cdn][ingest]") {
	// **The point of the whole route.** An inbox is not content until somebody
	// signs a manifest naming it - CDN.md §1 - so this pins that what lands
	// there is in the shape `Publish` reads, extension and all.
	Host host("publishable", true);

	REQUIRE(UploaderFor(host.Descriptor()) != nullptr);

	std::unique_ptr<Uploader> uploader = UploaderFor(host.Descriptor());
	REQUIRE(uploader->Add(WriteFile(host.Root / "out" / "rock.mesh", "vertices and indices")));
	REQUIRE(uploader->Add(WriteFile(host.Root / "out" / "grass.png", "texels")));
	host.Drain(*uploader);

	const auto report = cdn::Publish(host.Inbox, host.Root / "published", Publisher());
	REQUIRE(report.has_value());
	CHECK(report->Assets == 2);

	auto store = ChunkStore::Open(host.Root / "published", false);
	REQUIRE(store.has_value());
	engine::assets::SignatureBytes signature;
	const auto catalogue = store->ReadManifest(signature);
	REQUIRE(catalogue.has_value());

	// **The kinds survived the trip**, which is what the suffix header exists
	// for: without it both of these would publish as `Unknown`.
	size_t meshes = 0;
	size_t textures = 0;
	for (const auto &asset : catalogue->Assets()) {
		if (asset.Kind == engine::assets::AssetKind::Mesh) {
			meshes++;
		}
		if (asset.Kind == engine::assets::AssetKind::Texture) {
			textures++;
		}
	}
	CHECK(meshes == 1);
	CHECK(textures == 1);
}
