// The one route that says what an origin holds, and everything that keeps it
// shut.
//
// **The default is the thing under test here.** A listing route is not
// dangerous because of what it does; it is dangerous because of what it does
// when nobody configured it, and that is a property no amount of care at the
// call site can restore afterwards — a name that has been scraped stays
// scraped. So the first two cases are an origin that was never told to
// enumerate and an origin that was told to without a key, and both of them must
// answer as though the route did not exist.
//
// The paging is the other half: a manifest is unbounded, so what is pinned is
// that a cursor walks the whole of one and that the pages join back up into
// exactly what the manifest holds.

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/ContentRoot.hpp>
#include <cdn/Origin.hpp>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("cdn.catalogue")
TEST_DEPENDS("cdn.service")
TEST_DEPENDS("cdn.publisher")

using cdn::Origin;
using cdn::Publication;
using cdn::Service;
using cdn::ServiceSettings;
using engine::assets::ChunkStore;
using engine::assets::GrantKey;
using engine::assets::Manifest;
using engine::assets::SigningKey;
using engine::net::Endpoint;
using engine::net::http::FetchId;
using engine::net::http::FetchState;
using engine::net::http::MakeClient;
using engine::net::http::Method;
using engine::net::http::Request;
using engine::net::http::Response;
using engine::net::http::Status;

namespace {
	namespace fs = std::filesystem;

	constexpr uint64_t NOW = 2'000'000;
	constexpr int MAXIMUM_POLLS = 20000;
	constexpr const char *KEY = "an-origin-secret";

	// The three names every case below expects to find, in the order a manifest
	// keeps them: sorted, which is what makes an offset cursor meaningful.
	constexpr const char *NAMES[] = {"audio/bark.wav", "meshes/rock.mesh", "textures/grass.png"};

	GrantKey Secret() {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(13 + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	SigningKey Publisher() {
		std::array<std::byte, 32> seed{};
		seed.fill(std::byte{31});
		auto key = SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// A published store of three assets, served with whatever listing settings
	// a case wants.
	struct Host {
		fs::path Root;
		std::optional<Manifest> Catalogue;
		std::unique_ptr<Origin> Serving;
		std::unique_ptr<Service> Listening;
		Endpoint Address;

		explicit Host(const cdn::CatalogueSettings &listing, const char *key = KEY) {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-catalogue-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);

			for (const char *name : NAMES) {
				Write(name, std::string("the bytes of ") + name);
			}

			REQUIRE(cdn::Publish(Root / "content", Root / "store", Publisher()).has_value());

			auto store = ChunkStore::Open(Root / "store", false);
			REQUIRE(store.has_value());

			engine::assets::SignatureBytes signature;
			Catalogue = store->ReadManifest(signature);
			REQUIRE(Catalogue.has_value());

			Serving = std::make_unique<Origin>(Secret());
			auto mounted = cdn::ContentRoot::Mount(Root / "store");
			REQUIRE(mounted.has_value());
			REQUIRE(Serving->Publish(std::make_shared<const Publication>(*mounted, *Catalogue)));

			ServiceSettings settings;
			settings.Port = 0;
			settings.Ingest.Key = key;
			settings.Catalogue = listing;

			Listening = cdn::Serve(*Serving, std::move(*store), settings);
			REQUIRE(Listening != nullptr);
			Address = Endpoint::LoopbackIPv4(Listening->Local().Port);
		}

		~Host() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		Host(const Host &) = delete;
		Host &operator=(const Host &) = delete;

		void Write(const std::string &name, std::string_view body) {
			const fs::path path = Root / "content" / name;
			std::error_code failure;
			fs::create_directories(path.parent_path(), failure);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(body.data(), static_cast<std::streamsize>(body.size()));
		}

		// One request, with both ends driven until it settles.
		std::optional<Response> Ask(const std::string &target, const char *key = KEY) {
			Request request;
			request.Verb = Method::Get;
			request.Target = target;
			if (key != nullptr) {
				request.Headers.push_back({.Name = "x-atomic-ingest", .Value = key});
			}

			auto fetcher = MakeClient({});
			const FetchId id = fetcher->Submit(Address, request, "origin");
			REQUIRE(id.IsValid());

			for (int poll = 0; poll < MAXIMUM_POLLS; ++poll) {
				Listening->Pump(NOW);
				fetcher->Pump();
				if (fetcher->StateOf(id) != FetchState::Pending) {
					break;
				}
			}
			if (fetcher->StateOf(id) != FetchState::Ready) {
				return std::nullopt;
			}
			return fetcher->Take(id);
		}
	};

	std::string Text(const Response &response) {
		return {
			reinterpret_cast<const char *>(response.Body.data()),
			reinterpret_cast<const char *>(response.Body.data()) + response.Body.size()
		};
	}

	// Every `asset` line's name, in the order the page listed them.
	std::vector<std::string> NamesIn(const std::string &page) {
		std::vector<std::string> names;
		size_t at = 0;
		while (at < page.size()) {
			const size_t end = page.find('\n', at);
			const std::string line = page.substr(at, end == std::string::npos ? end : end - at);
			at = end == std::string::npos ? page.size() : end + 1;

			if (!line.starts_with("asset ")) {
				continue;
			}
			// `asset <root> <kind> <bytes> <name>` — the name is the rest of
			// the line, which is the whole reason it is last.
			size_t field = std::string("asset ").size();
			for (int skipped = 0; skipped < 3; skipped++) {
				field = line.find(' ', field);
				REQUIRE(field != std::string::npos);
				field++;
			}
			names.push_back(line.substr(field));
		}
		return names;
	}

	std::optional<std::string> ValueOf(const std::string &page, std::string_view key) {
		size_t at = 0;
		while (at < page.size()) {
			const size_t end = page.find('\n', at);
			const std::string line = page.substr(at, end == std::string::npos ? end : end - at);
			at = end == std::string::npos ? page.size() : end + 1;

			if (line.starts_with(std::string(key) + " ")) {
				return line.substr(key.size() + 1);
			}
		}
		return std::nullopt;
	}
}

TEST_CASE("an origin nobody configured to list answers as though there were no route", "[cdn][catalogue]") {
	// **The default, and the reason this suite exists.** Every other refusal
	// here can be fixed after the fact; this one cannot, because names that
	// have been scraped stay scraped.
	Host host(cdn::CatalogueSettings{});

	const auto answer = host.Ask("/catalogue");
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::NotFound);
	CHECK(answer->Body.empty());
	CHECK(host.Listening->Counters().CatalogueRefused == 1);
	CHECK(host.Listening->Counters().Catalogues == 0);
}

TEST_CASE("switched on with no key to admit it, the route stays shut", "[cdn][catalogue]") {
	// Half-configured reads as off — `IngestSettings::Accepts`'s rule on the
	// read side. A flag with no key would be an origin enumerating for anybody,
	// which is precisely what the flag exists to keep deliberate.
	Host host(cdn::CatalogueSettings{.Enabled = true}, "");

	const auto answer = host.Ask("/catalogue", nullptr);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::NotFound);
	CHECK(host.Listening->Counters().CatalogueRefused == 1);
}

TEST_CASE("a listing origin refuses a caller with no key and one with the wrong key", "[cdn][catalogue]") {
	Host host(cdn::CatalogueSettings{.Enabled = true});

	const auto silent = host.Ask("/catalogue", nullptr);
	REQUIRE(silent.has_value());
	CHECK(silent->Code == Status::Forbidden);
	// A refusal carries no reason: one returned to a client is an oracle.
	CHECK(silent->Body.empty());

	const auto wrong = host.Ask("/catalogue", "not-the-secret");
	REQUIRE(wrong.has_value());
	CHECK(wrong->Code == Status::Forbidden);

	// **Both counted as the same event**, because they are: something asked
	// what this origin holds and was not allowed to know.
	CHECK(host.Listening->Counters().CatalogueRefused == 2);
	CHECK(host.Listening->Counters().Catalogues == 0);
}

TEST_CASE("a listing origin says what it holds", "[cdn][catalogue]") {
	Host host(cdn::CatalogueSettings{.Enabled = true});

	const auto answer = host.Ask("/catalogue");
	REQUIRE(answer.has_value());
	REQUIRE(answer->Code == Status::Ok);

	const std::string page = Text(*answer);
	CHECK(ValueOf(page, "total") == std::to_string(std::size(NAMES)));

	// The publication's root travels with every page, so a reader can tell a
	// publish that swapped underneath it from a list that changed.
	CHECK(ValueOf(page, "root") == host.Catalogue->Root().ToHex());

	// One page holds all three, so there is nothing to continue to.
	CHECK_FALSE(ValueOf(page, "next").has_value());

	const std::vector<std::string> names = NamesIn(page);
	REQUIRE(names.size() == std::size(NAMES));
	for (size_t index = 0; index < std::size(NAMES); index++) {
		CHECK(names[index] == NAMES[index]);
	}

	CHECK(host.Listening->Counters().Catalogues == 1);
	CHECK(host.Listening->Counters().CatalogueRefused == 0);
}

TEST_CASE("a cursor walks a manifest a page at a time", "[cdn][catalogue]") {
	// **What paging is for**, at the smallest size that shows it: a manifest
	// has no bound, and a route that serialised all of one would make a single
	// request's cost a property of somebody else's content.
	Host host(cdn::CatalogueSettings{.Enabled = true, .PageEntries = 2});

	std::vector<std::string> collected;
	std::string target = "/catalogue";
	int pages = 0;

	while (pages < 10) {
		const auto answer = host.Ask(target);
		REQUIRE(answer.has_value());
		REQUIRE(answer->Code == Status::Ok);
		pages++;

		const std::string page = Text(*answer);
		for (std::string &name : NamesIn(page)) {
			collected.push_back(std::move(name));
		}

		const std::optional<std::string> next = ValueOf(page, "next");
		if (!next) {
			break;
		}
		target = "/catalogue/" + *next;
	}

	CHECK(pages == 2);
	REQUIRE(collected.size() == std::size(NAMES));
	for (size_t index = 0; index < std::size(NAMES); index++) {
		CHECK(collected[index] == NAMES[index]);
	}

	// A cursor past the end is an empty page rather than an error: it is the
	// answer a reader gets when content was removed while it was walking.
	const auto beyond = host.Ask("/catalogue/900");
	REQUIRE(beyond.has_value());
	CHECK(beyond->Code == Status::Ok);
	CHECK(NamesIn(Text(*beyond)).empty());
}

TEST_CASE("a cursor that is not one of this origin's numbers is refused", "[cdn][catalogue]") {
	Host host(cdn::CatalogueSettings{.Enabled = true});

	for (const std::string &target : {
			 std::string("/catalogue/"),
			 std::string("/catalogue/../manifest"),
			 std::string("/catalogue/2a"),
			 std::string("/catalogue/-1"),
			 std::string("/catalogue/99999999999999999999999"),
		 }) {
		INFO("target " << target);
		const auto answer = host.Ask(target);
		REQUIRE(answer.has_value());
		CHECK(answer->Code == Status::BadRequest);
	}

	// **A word that merely starts the same way is not this route.** A `400`
	// there would tell an unauthenticated caller that something answers near
	// that name.
	const auto nearby = host.Ask("/cataloguery", nullptr);
	REQUIRE(nearby.has_value());
	CHECK(nearby->Code == Status::NotFound);
}
