// The editor's content-source list, and the file it survives in.
//
// **Reachable headlessly because none of it needs a frame.** The preferences
// page that edits this list is imgui; the list itself is ordinary data with a
// text format either side of it, which is the half that can be wrong without a
// window - a dropped row, a lost `off`, a priority order that comes back
// reversed.
//
// The order is the feature, so most of what is pinned here is order:
// `delivery::AssetClient` walks the list and stops at the first source that
// answers, which makes "second" a different meaning from "first" rather than a
// presentation detail.
//
// **The HTTP tab's listing is here too, and it is a source's feature rather than
// a catalogue's.** What decides whether an origin can be enumerated is the row
// somebody typed on the Content page - the address, and the key beside it - and
// what the panel must never do is answer the question from somewhere else. The
// last group of cases is that rule: every way of not knowing is drawn as its own
// sentence, and none of them is drawn as an empty table.

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/LocalStore.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cdn/ContentRoot.hpp>
#include <cdn/Origin.hpp>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <studio/AssetCatalogue.hpp>
#include <studio/ContentSources.hpp>
#include <thread>

TEST_SUITE_ID("studio.contentsources")
TEST_DEPENDS("studio.assetcatalogue")
TEST_DEPENDS("cdn.catalogue")

using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::delivery::SourceRole;
using studio::CatalogueEntry;
using studio::CatalogueTab;
using studio::ContentSources;
using studio::ListingOutcome;
using studio::OriginLister;
using studio::OriginListing;

namespace {
	std::filesystem::path ScratchFile(const char *leaf) {
		return std::filesystem::temp_directory_path() / leaf;
	}

	Source Origin(const char *name, const char *location, bool enabled = true) {
		Source source;
		source.Name = name;
		source.Kind = SourceKind::Http;
		source.Location = location;
		source.Enabled = enabled;
		return source;
	}

	// A lister that answers whatever a case wants, so the note logic is
	// checkable without a port.
	class StubLister final : public OriginLister {
	  public:
		explicit StubLister(OriginListing answer) : Answer(std::move(answer)) {}

		OriginListing List(const Source &source) override {
			Asked.push_back(source.Name);
			return Answer;
		}

		OriginListing Answer;
		std::vector<std::string> Asked;
	};

	const CatalogueTab *TabTitled(const std::vector<CatalogueTab> &tabs, std::string_view title) {
		const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const CatalogueTab &tab) {
			return tab.Title == title;
		});
		return found == tabs.end() ? nullptr : &*found;
	}

	ContentSources ListOf(Source source) {
		ContentSources sources;
		sources.Sources.push_back(std::move(source));
		return sources;
	}
}

TEST_CASE("a fresh install starts with the store on this machine", "[studio][contentsources]") {
	const ContentSources sources = ContentSources::Default();

	// **Two rows, and the first is the local folder.** This used to be one HTTP
	// origin and no publisher key, and "no key" is what `DeliverySettings::
	// IsValid` refuses - so a fresh editor built no delivery client and fetched
	// nothing at all. A `MeshPart` drew the fallback cube however good its
	// `MeshId` was.
	REQUIRE(sources.Sources.size() == 2);
	CHECK(sources.Sources.front().Enabled);
	CHECK(sources.Sources.front().Kind == engine::delivery::SourceKind::Directory);

	// **A key by default, which the comment here used to forbid.** "Defaulting
	// one would be defaulting who this editor believes" is still true and is
	// exactly why this one is `engine::assets::DevelopmentSigningKey`'s public half: it
	// believes the store this machine publishes to, which is the folder on the
	// row above it. Pointing a row anywhere else means supplying the real key -
	// see `engine/assets/LocalStore.hpp` for where that stops.
	CHECK_FALSE(sources.PublisherKey.empty());
}

TEST_CASE("a saved list comes back in the same order", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-roundtrip.ini");
	std::filesystem::remove(path);

	ContentSources written;
	written.Sources = {Origin("near", "127.0.0.1:9080"), Origin("far", "10.0.0.4:9080")};
	written.Sources.push_back(Origin("off-site", "10.0.0.9:9080", false));
	written.Sources[2].Kind = SourceKind::Directory;
	written.Sources[2].Location = "/var/content";
	written.CachePath = "/tmp/atomic-cache";
	written.PublisherKey = std::string(64, 'a');

	REQUIRE(written.Save(path));

	ContentSources read;
	REQUIRE(read.Load(path));

	REQUIRE(read.Sources.size() == 3);
	CHECK(read.Sources[0].Name == "near");
	CHECK(read.Sources[1].Name == "far");
	CHECK(read.Sources[2].Name == "off-site");

	// **The disabled row survives, address and all.** The header promises this
	// explicitly: somebody working out which of three origins is broken
	// switches one off and back on, and a file that forgot the address in
	// between would make that a retyping exercise.
	CHECK_FALSE(read.Sources[2].Enabled);
	CHECK(read.Sources[2].Location == "/var/content");
	CHECK(read.Sources[2].Kind == SourceKind::Directory);

	CHECK(read.CachePath == std::filesystem::path("/tmp/atomic-cache"));
	CHECK(read.PublisherKey == std::string(64, 'a'));

	std::filesystem::remove(path);
}

TEST_CASE("a missing file leaves the list alone", "[studio][contentsources]") {
	// Not an error, and - the part worth pinning - not a reset either. `Load`
	// returns before it clears anything, so a caller that loads over a list it
	// already has keeps that list rather than being silently emptied.
	ContentSources sources;
	sources.Sources = {Origin("kept", "127.0.0.1:9080")};

	CHECK_FALSE(sources.Load(ScratchFile("atomic-sources-does-not-exist.ini")));
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Name == "kept");
}

TEST_CASE("a half-written row is skipped rather than half-read", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-malformed.ini");
	std::filesystem::remove(path);

	{
		std::ofstream out(path, std::ios::trunc);
		out << "# a comment\n";
		out << "\n";
		out << "not-a-key-value-line\n";
		out << "source = only-a-name\n";
		out << "source = two | http\n";
		out << "source = good | http | 127.0.0.1:9080 | on\n";
		out << "unknown = ignored\n";
	}

	ContentSources sources;
	REQUIRE(sources.Load(path));

	// A source with no location would sit in the list looking configured and
	// refuse every fetch, which is worse than not being there.
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Name == "good");

	std::filesystem::remove(path);
}

TEST_CASE("Move refuses to walk off either end", "[studio][contentsources]") {
	ContentSources sources;
	sources.Sources = {Origin("first", "a"), Origin("second", "b")};

	// Wrong: past the end, and the two ends themselves.
	CHECK_FALSE(sources.Move(2, -1));
	CHECK_FALSE(sources.Move(0, -1));
	CHECK_FALSE(sources.Move(1, 1));

	// Nothing moved on any refusal.
	CHECK(sources.Sources[0].Name == "first");
	CHECK(sources.Sources[1].Name == "second");

	REQUIRE(sources.Move(0, 1));
	CHECK(sources.Sources[0].Name == "second");
	CHECK(sources.Sources[1].Name == "first");
}

TEST_CASE("Move on an empty list does nothing", "[studio][contentsources]") {
	ContentSources sources;
	CHECK_FALSE(sources.Move(0, 1));
	CHECK(sources.Sources.empty());
}

TEST_CASE("an unparsable publisher key yields settings with no trust", "[studio][contentsources]") {
	// **The failure has to be visible as "not configured" rather than as a
	// key.** `MakeAssetClient` is what refuses, and it can only refuse if a
	// half-typed key does not arrive looking like a whole one.
	ContentSources sources = ContentSources::Default();
	sources.PublisherKey = "not-hex";

	const engine::delivery::DeliverySettings settings = sources.ToSettings();
	CHECK(settings.Sources.size() == sources.Sources.size());

	// And the list a person typed carries no host restriction: that check is
	// for a source list a *server* sent, not one from these preferences.
	CHECK(settings.AllowedHosts.empty());
}

// --- roles: which direction a source is used in ----------------------------

TEST_CASE("a role and an ingest key survive a save and a load", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-roles.ini");
	std::filesystem::remove(path);

	ContentSources sources;
	sources.PublisherKey = std::string(64, 'a');

	Source reader = Origin("reads", "127.0.0.1:9080");
	reader.Role = SourceRole::Read;

	Source writer = Origin("writes", "127.0.0.1:9081");
	writer.Role = SourceRole::Write;
	writer.IngestKey = "a shared secret";

	sources.Sources = {reader, writer};
	REQUIRE(sources.Save(path));

	ContentSources loaded;
	REQUIRE(loaded.Load(path));
	REQUIRE(loaded.Sources.size() == 2);

	CHECK(loaded.Sources[0].Role == SourceRole::Read);
	CHECK(loaded.Sources[0].IngestKey.empty());
	CHECK(loaded.Sources[1].Role == SourceRole::Write);

	// **A key with a space in it**, which is why the field is the line's
	// remainder rather than one more pipe-separated column: a secret somebody
	// pasted is not this parser's business to constrain.
	CHECK(loaded.Sources[1].IngestKey == "a shared secret");

	std::filesystem::remove(path);
}

TEST_CASE("a list written before roles existed loads as both", "[studio][contentsources]") {
	// **Not compatibility for its own sake.** The absent case has an obviously
	// right answer: a list written when there was no choice meant `Both`, and
	// reading it as anything else would silently stop a working single-origin
	// setup from either fetching or uploading.
	const std::filesystem::path path = ScratchFile("atomic-sources-old.ini");
	std::filesystem::remove(path);

	{
		std::ofstream out(path, std::ios::trunc);
		out << "source = old | http | 127.0.0.1:9080 | on\n";
	}

	ContentSources sources;
	REQUIRE(sources.Load(path));
	REQUIRE(sources.Sources.size() == 1);
	CHECK(sources.Sources.front().Role == SourceRole::Both);
	CHECK(sources.Sources.front().Enabled);

	std::filesystem::remove(path);
}

TEST_CASE("reads and writes go to different origins", "[studio][contentsources]") {
	// The point of the whole feature, from the editor's end: one list, one
	// order, and each row saying which directions it is in.
	ContentSources sources;
	sources.PublisherKey = std::string(64, 'a');

	Source reader = Origin("reads", "127.0.0.1:9080");
	reader.Role = SourceRole::Read;

	Source writer = Origin("writes", "127.0.0.1:9081");
	writer.Role = SourceRole::Write;
	writer.IngestKey = "secret";

	sources.Sources = {reader, writer};

	const engine::delivery::DeliverySettings settings = sources.ToSettings();

	// Both rows reach the settings - the split happens when they are asked for,
	// not when they are written down, so a row keeps its address whichever
	// direction it is in.
	REQUIRE(settings.Sources.size() == 2);

	const std::vector<Source> readable = settings.Usable();
	REQUIRE(readable.size() == 1);
	CHECK(readable[0].Name == "reads");

	const std::vector<Source> writable = settings.Writable();
	REQUIRE(writable.size() == 1);
	CHECK(writable[0].Name == "writes");
}

TEST_CASE("an unrecognised role reads as both rather than dropping the row", "[studio][contentsources]") {
	const std::filesystem::path path = ScratchFile("atomic-sources-badrole.ini");
	std::filesystem::remove(path);

	{
		std::ofstream out(path, std::ios::trunc);
		out << "source = odd | http | 127.0.0.1:9080 | on | sideways |\n";
	}

	ContentSources sources;
	REQUIRE(sources.Load(path));
	REQUIRE(sources.Sources.size() == 1);

	// The permissive answer is the one that keeps a list working - a row
	// dropped over a word nobody recognises is an origin that vanished.
	CHECK(sources.Sources.front().Role == SourceRole::Both);

	std::filesystem::remove(path);
}

// --- what a fresh editor can reach -------------------------------------------

TEST_CASE("a fresh editor can fetch from the store on this machine", "[studio][content]") {
	// **The default was an address nobody is listening on and no publisher
	// key**, which `DeliverySettings::IsValid` refuses - so `MakeAssetClient`
	// was never called and the editor fetched *nothing at all*. A `MeshPart`
	// drew the fallback cube however good its `MeshId` was, a `ColorMap` drew
	// nothing, and the assets panel happily listed a store full of content none
	// of which the viewport could show. Nothing said why: an unconfigured
	// delivery client and one whose fetches fail look identical from a viewport.
	const studio::ContentSources sources = studio::ContentSources::Default();
	const engine::delivery::DeliverySettings settings = sources.ToSettings();

	CHECK(settings.IsValid());
	REQUIRE_FALSE(settings.Usable().empty());

	// The store this machine's client publishes to and reads from, first.
	CHECK(settings.Usable().front().Kind == engine::delivery::SourceKind::Directory);
	CHECK(settings.Usable().front().Location == engine::assets::DefaultLocalPaths().Processed.string());

	// **The key that can verify it.** Trusting the development identity is what
	// makes the local folder usable with nothing typed; it verifies only what
	// this editor publishes, and any other origin needs the real one.
	CHECK(sources.PublisherKey == engine::assets::DevelopmentPublisher().ToHex());
}

TEST_CASE("the remote row is kept and turned off", "[studio][content]") {
	// **Kept rather than dropped**, which is `ContentSources.hpp`'s own rule one
	// row over: a disabled source is one somebody can switch on, and deleting
	// the address a deployment uses would make the common remote case something
	// to retype.
	const studio::ContentSources sources = studio::ContentSources::Default();
	REQUIRE(sources.Sources.size() >= 2);

	bool sawDisabledHttp = false;
	for (const engine::delivery::Source &source : sources.Sources) {
		if (source.Kind == engine::delivery::SourceKind::Http) {
			CHECK_FALSE(source.Enabled);
			sawDisabledHttp = true;
		}
	}
	CHECK(sawDisabledHttp);
}

TEST_CASE("the default survives a save and a load", "[studio][content]") {
	// The key is written and read back, which the round trip did not cover
	// before because the default had none.
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "atomic-studio-content-default.ini";
	std::filesystem::remove(path);

	const studio::ContentSources written = studio::ContentSources::Default();
	REQUIRE(written.Save(path));

	studio::ContentSources read;
	REQUIRE(read.Load(path));
	CHECK(read.PublisherKey == written.PublisherKey);
	REQUIRE(read.Sources.size() == written.Sources.size());
	CHECK(read.Sources[0].Location == written.Sources[0].Location);
	CHECK(read.Sources[0].Enabled);

	std::filesystem::remove(path);
}

TEST_CASE("raw folders and the memory-only flag survive a save", "[studio][contentsources]") {
	const std::filesystem::path file = ScratchFile("atomic-content-raw.ini");
	std::filesystem::remove(file);

	ContentSources written;
	written.Sources.push_back(Origin("origin", "127.0.0.1:9080"));
	written.RawFolders.emplace_back("/art/characters");
	written.RawFolders.emplace_back("/art/props");
	written.MemoryOnly = false;
	REQUIRE(written.Save(file));

	ContentSources read;
	REQUIRE(read.Load(file));
	REQUIRE(read.RawFolders.size() == 2);
	REQUIRE(read.RawFolders[0] == std::filesystem::path("/art/characters"));
	REQUIRE(read.RawFolders[1] == std::filesystem::path("/art/props"));
	REQUIRE_FALSE(read.MemoryOnly);

	// **A raw folder is not an origin**, and reading one back as a source would
	// put a folder of PNGs into the list `delivery::AssetClient` fetches from -
	// where every name has to be one a signed manifest carries.
	REQUIRE(read.Sources.size() == 1);

	std::filesystem::remove(file);
}

TEST_CASE("a file written before raw folders existed still loads", "[studio][contentsources]") {
	const std::filesystem::path file = ScratchFile("atomic-content-old.ini");
	{
		std::ofstream out(file, std::ios::trunc);
		out << "publisher = ab\n";
		out << "source = origin | http | 127.0.0.1:9080 | on\n";
	}

	ContentSources read;
	read.RawFolders.emplace_back("/left/over");
	read.MemoryOnly = false;
	REQUIRE(read.Load(file));

	// **Cleared, and the flag goes back to on rather than staying off.** The
	// safe answer is the one that writes nothing to somebody's disk, and a
	// leftover `false` from a previous load would make an editor start baking
	// into the content store because of a file that never mentioned it.
	REQUIRE(read.RawFolders.empty());
	REQUIRE(read.MemoryOnly);

	std::filesystem::remove(file);
}

// --- what an HTTP origin's tab says ------------------------------------------

namespace {
	// A real origin on a real port, pumped on its own thread.
	//
	// **A thread rather than a poll loop**, which is the one place this suite
	// differs from `cdn/tests/Catalogue.cpp`: the lister waits for its answer
	// inside `BuildCatalogue`, so nothing on this thread is left to turn the
	// origin's crank while it does. The service is owned by the pumping thread
	// for its whole life and read here only after the join.
	struct ListingHost {
		std::filesystem::path Root;
		std::unique_ptr<cdn::Origin> Serving;
		std::unique_ptr<cdn::Service> Listening;
		std::string Address;
		std::atomic<bool> Stopping{false};
		std::thread Pumping;

		ListingHost(bool lists, const char *key, const std::vector<std::string> &names) {
			static int serial = 0;
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-studio-listing-" + std::to_string(++serial));
			std::error_code failure;
			std::filesystem::remove_all(Root, failure);

			for (const std::string &name : names) {
				const std::filesystem::path path = Root / "content" / name;
				std::filesystem::create_directories(path.parent_path(), failure);
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
				file << "the bytes of " << name;
			}

			std::array<std::byte, 32> seed{};
			seed.fill(std::byte{41});
			const auto signing = engine::assets::SigningKey::FromSeed(seed);
			REQUIRE(signing.has_value());
			REQUIRE(cdn::Publish(Root / "content", Root / "store", *signing).has_value());

			auto store = engine::assets::ChunkStore::Open(Root / "store", false);
			REQUIRE(store.has_value());
			engine::assets::SignatureBytes signature;
			const auto catalogue = store->ReadManifest(signature);
			REQUIRE(catalogue.has_value());

			std::array<std::byte, engine::assets::GrantKey::BYTES> secret{};
			secret.fill(std::byte{17});
			auto grants = engine::assets::GrantKey::FromSecret(secret);
			REQUIRE(grants.has_value());

			Serving = std::make_unique<cdn::Origin>(std::move(*grants));
			auto mounted = cdn::ContentRoot::Mount(Root / "store");
			REQUIRE(mounted.has_value());
			REQUIRE(Serving->Publish(std::make_shared<const cdn::Publication>(*mounted, *catalogue)));

			cdn::ServiceSettings settings;
			settings.Port = 0;
			settings.Ingest.Key = key;
			settings.Catalogue.Enabled = lists;

			Listening = cdn::Serve(*Serving, std::move(*store), settings);
			REQUIRE(Listening != nullptr);
			Address = engine::net::Endpoint::LoopbackIPv4(Listening->Local().Port).Text();

			Pumping = std::thread([this]() {
				while (!Stopping.load(std::memory_order_relaxed)) {
					Listening->Pump(1000);
					std::this_thread::sleep_for(std::chrono::microseconds(200));
				}
			});
		}

		~ListingHost() {
			Stopping.store(true, std::memory_order_relaxed);
			if (Pumping.joinable()) {
				Pumping.join();
			}
			std::error_code failure;
			std::filesystem::remove_all(Root, failure);
		}

		ListingHost(const ListingHost &) = delete;
		ListingHost &operator=(const ListingHost &) = delete;

		Source Row(const char *key) const {
			Source source = Origin("origin", Address.c_str());
			source.Location = Address;
			source.IngestKey = key;
			return source;
		}
	};

	std::vector<std::string> NamesOf(const std::vector<CatalogueEntry> &entries) {
		std::vector<std::string> names;
		names.reserve(entries.size());
		for (const CatalogueEntry &entry : entries) {
			names.push_back(entry.Name);
		}
		return names;
	}
}

TEST_CASE(
	"an origin that will not enumerate is a sentence, never an empty list", "[studio][contentsources]"
) {
	// **The rule D00111 was filed to protect.** Each of these is a different
	// fact about one origin, and a blank table under a named tab would read as
	// the first one - "this origin holds nothing" - whichever of them was true.
	const ListingOutcome cannot[] = {
		ListingOutcome::NotAsked,
		ListingOutcome::NoKey,
		ListingOutcome::Unreachable,
		ListingOutcome::NotOffered,
		ListingOutcome::Refused,
		ListingOutcome::Unreadable,
	};

	std::vector<std::string> said;
	for (const ListingOutcome outcome : cannot) {
		StubLister lister(OriginListing{.Outcome = outcome, .Entries = {}});
		const std::vector<CatalogueTab> tabs =
			studio::BuildCatalogue(ListOf(Origin("far", "10.0.0.4:9080")), &lister);

		const CatalogueTab *tab = TabTitled(tabs, "far");
		REQUIRE(tab != nullptr);
		CHECK(tab->Entries.empty());

		// Said, and said differently: two outcomes sharing a sentence would be
		// two situations somebody cannot tell apart, which is the whole
		// complaint the deferred entry made about the old tab.
		REQUIRE_FALSE(tab->Note.empty());
		CHECK(std::find(said.begin(), said.end(), tab->Note) == said.end());
		said.push_back(tab->Note);
	}
}

TEST_CASE("an origin's names are listed under its own tab", "[studio][contentsources]") {
	StubLister lister(
		OriginListing{
			.Outcome = ListingOutcome::Listed,
			.Entries = {
				CatalogueEntry{
					.Name = "meshes/rock.amesh",
					.Kind = engine::assets::AssetKind::Mesh,
					.Root = {},
					.Source = "far",
					.Unbaked = {},
				},
				CatalogueEntry{
					.Name = "textures/grass.atex",
					.Kind = engine::assets::AssetKind::Texture,
					.Root = {},
					.Source = "far",
					.Unbaked = {},
				},
			},
		}
	);

	const std::vector<CatalogueTab> tabs =
		studio::BuildCatalogue(ListOf(Origin("far", "10.0.0.4:9080")), &lister);

	const CatalogueTab *tab = TabTitled(tabs, "far");
	REQUIRE(tab != nullptr);
	REQUIRE(tab->Entries.size() == 2);
	CHECK(tab->Note.empty());
	CHECK(lister.Asked == std::vector<std::string>{"far"});

	// **And every row says it came from there.** The merged tab is what makes
	// two origins holding one name visible, and it can only do that if each row
	// carries which origin answered for it.
	const std::vector<CatalogueEntry> &merged = tabs.front().Entries;
	const auto rock = std::find_if(merged.begin(), merged.end(), [](const CatalogueEntry &entry) {
		return entry.Name == "meshes/rock.amesh";
	});
	REQUIRE(rock != merged.end());
	CHECK(rock->Source == "far");
}

TEST_CASE("an origin that answered and holds nothing says that instead", "[studio][contentsources]") {
	StubLister lister(OriginListing{.Outcome = ListingOutcome::Listed, .Entries = {}});
	const std::vector<CatalogueTab> tabs =
		studio::BuildCatalogue(ListOf(Origin("far", "10.0.0.4:9080")), &lister);

	const CatalogueTab *tab = TabTitled(tabs, "far");
	REQUIRE(tab != nullptr);
	CHECK(tab->Entries.empty());
	// A different sentence from "it would not tell me", because it is a
	// different fact and it is the one an author acts on.
	CHECK(tab->Note == "this origin answered, and has published nothing");
}

TEST_CASE("a directory source is never asked over the wire", "[studio][contentsources]") {
	// A published tree is listed by reading its manifest - asking it anything
	// would be a second reader of a format that already has one.
	StubLister lister(OriginListing{.Outcome = ListingOutcome::Listed, .Entries = {}});

	Source folder = Origin("local", "");
	folder.Kind = SourceKind::Directory;
	folder.Location = std::filesystem::temp_directory_path().string();

	studio::BuildCatalogue(ListOf(folder), &lister);
	CHECK(lister.Asked.empty());
}

TEST_CASE("the assets panel lists what a listing origin holds", "[studio][content]") {
	// End to end, over a socket: a published store, an origin told to
	// enumerate, and the tab filled from what it said rather than from anything
	// this process already knew.
	const std::vector<std::string> published = {"audio/bark.wav", "meshes/rock.mesh"};
	ListingHost host(true, "an-origin-secret", published);

	const std::unique_ptr<OriginLister> lister = studio::MakeOriginLister();
	const std::vector<CatalogueTab> tabs =
		studio::BuildCatalogue(ListOf(host.Row("an-origin-secret")), lister.get());

	const CatalogueTab *tab = TabTitled(tabs, "origin");
	REQUIRE(tab != nullptr);
	CHECK(tab->Note.empty());
	REQUIRE(tab->Entries.size() == published.size());
	CHECK(NamesOf(tab->Entries) == published);

	for (const CatalogueEntry &entry : tab->Entries) {
		INFO(entry.Name);
		// **A content address per row, from the origin's own manifest.** A name
		// with no address would be a row nothing could be fetched by.
		CHECK_FALSE(entry.Root.IsZero());
		CHECK(entry.Source == "origin");
	}
}

TEST_CASE("an origin with enumeration off says so rather than showing nothing", "[studio][content]") {
	ListingHost host(false, "an-origin-secret", {"audio/bark.wav"});

	const std::unique_ptr<OriginLister> lister = studio::MakeOriginLister();
	const std::vector<CatalogueTab> tabs =
		studio::BuildCatalogue(ListOf(host.Row("an-origin-secret")), lister.get());

	const CatalogueTab *tab = TabTitled(tabs, "origin");
	REQUIRE(tab != nullptr);
	CHECK(tab->Entries.empty());
	CHECK(tab->Note == std::string(studio::Describe(ListingOutcome::NotOffered)));
}

TEST_CASE("a listing origin refuses the wrong key, and the tab says which", "[studio][content]") {
	ListingHost host(true, "an-origin-secret", {"audio/bark.wav"});

	const std::unique_ptr<OriginLister> lister = studio::MakeOriginLister();

	const std::vector<CatalogueTab> refused =
		studio::BuildCatalogue(ListOf(host.Row("not-the-secret")), lister.get());
	const CatalogueTab *wrong = TabTitled(refused, "origin");
	REQUIRE(wrong != nullptr);
	CHECK(wrong->Entries.empty());
	CHECK(wrong->Note == std::string(studio::Describe(ListingOutcome::Refused)));

	// **No key means no round trip at all**, which is why this case is not the
	// same as the one above: an origin will not enumerate for an
	// unauthenticated caller, so there is nothing to learn by asking.
	const std::vector<CatalogueTab> unkeyed = studio::BuildCatalogue(ListOf(host.Row("")), lister.get());
	const CatalogueTab *none = TabTitled(unkeyed, "origin");
	REQUIRE(none != nullptr);
	CHECK(none->Note == std::string(studio::Describe(ListingOutcome::NoKey)));
}

TEST_CASE("a page that is not a listing is refused whole", "[studio][contentsources]") {
	// An origin is something anybody can run, so a body that is not this format
	// is reported rather than half-read - half a page of invented names under a
	// named origin's tab is the failure this whole feature exists to avoid.
	CHECK_FALSE(studio::ParseCataloguePage("", "far").has_value());
	CHECK_FALSE(studio::ParseCataloguePage("<html>nope</html>\n", "far").has_value());
	CHECK_FALSE(studio::ParseCataloguePage("catalogue 2\ntotal 0\n", "far").has_value());
	CHECK_FALSE(studio::ParseCataloguePage("catalogue 1\ntotal not-a-number\n", "far").has_value());
	CHECK_FALSE(studio::ParseCataloguePage("catalogue 1\nasset nothex mesh 4 a.mesh\n", "far").has_value());
	CHECK_FALSE(studio::ParseCataloguePage("catalogue 1\nasset\n", "far").has_value());

	const std::string root(64, 'a');
	const std::optional<studio::CataloguePage> page = studio::ParseCataloguePage(
		"catalogue 1\nroot " + root + "\ntotal 9\nnext 2\nasset " + root + " mesh 40 meshes/a b.mesh\n", "far"
	);
	REQUIRE(page.has_value());
	CHECK(page->Root == root);
	CHECK(page->Total == 9);
	REQUIRE(page->Next.has_value());
	CHECK(*page->Next == 2);
	REQUIRE(page->Entries.size() == 1);

	// **The name is the rest of the line, spaces and all.** It is the one field
	// with no bound on what is inside it, which is why it is written last.
	CHECK(page->Entries.front().Name == "meshes/a b.mesh");
	CHECK(page->Entries.front().Kind == engine::assets::AssetKind::Mesh);
	CHECK(page->Entries.front().Source == "far");
}
