#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The origin over a real socket, asked directly.
//
// **No delivery client here on purpose.** This suite is the answer to "create a
// cdn and request directly to it": it speaks the HTTP surface by hand, so what
// is being checked is the *protocol* - the routes, the statuses, the refusals
// and the compression - rather than whether two pieces of our own code agree
// with each other. `Delivery.cpp` is the other half and uses the real client.

TEST_SUITE_ID("cdn.service")
TEST_DEPENDS("cdn.origin")
TEST_DEPENDS("cdn.publisher")
TEST_DEPENDS("engine.net.http.transfer")
TEST_DEPENDS("engine.delivery.groupcodec")

using cdn::CDNSettings;
using cdn::ContentRoot;
using cdn::Origin;
using cdn::Publication;
using cdn::Service;
using cdn::ServiceSettings;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::Grant;
using engine::assets::GrantKey;
using engine::assets::GrantScope;
using engine::assets::Manifest;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;
using engine::delivery::GroupCodec;
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

	constexpr uint64_t NOW = 1'000'000;
	constexpr uint64_t EXPIRY = NOW + 300;
	constexpr int MAXIMUM_POLLS = 20000;

	GrantKey Secret(uint8_t fill = 5) {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(fill + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	SigningKey Publisher() {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); ++index) {
			seed[index] = static_cast<std::byte>(index + 11);
		}
		auto key = SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// Records that share a shape and differ in their numbers - compressible for
	// the reason real content is, rather than because every byte is the same.
	std::string Structured(std::string_view label, size_t records) {
		std::string text;
		text.reserve(records * 48);
		for (size_t index = 0; index < records; ++index) {
			text += "{\"";
			text += label;
			text += "\":";
			text += std::to_string(index);
			text += ",\"lod\":";
			text += std::to_string(index % 4);
			text += ",\"flags\":";
			text += std::to_string((index * 2654435761u) % 65536);
			text += "}\n";
		}
		return text;
	}

	std::string ToHex(const std::vector<std::byte> &bytes) {
		static constexpr char DIGITS[] = "0123456789abcdef";
		std::string hex;
		for (const std::byte value : bytes) {
			hex.push_back(DIGITS[(static_cast<unsigned>(value) >> 4) & 0xF]);
			hex.push_back(DIGITS[static_cast<unsigned>(value) & 0xF]);
		}
		return hex;
	}

	// A published store, an origin over it, and a service on a port.
	struct Deployment {
		fs::path Root;
		std::optional<Manifest> Catalogue;
		std::unique_ptr<Origin> Serving;
		std::unique_ptr<Service> Listening;
		Endpoint Address;

		explicit Deployment(const CDNSettings &settings = {}) {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-service-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);

			// **Repetitive but varied, the way cooked content actually is.** A
			// buffer of one repeated byte compresses to about forty bytes,
			// which makes "the frame was smaller than the payload" true for a
			// reason that has nothing to do with the codec working - and it
			// leaves nothing for a range request to slice. Structured records
			// with changing numbers in them compress well and not absurdly.
			Write("meshes/rock.mesh", Structured("vert", 3000));
			Write("textures/grass.png", Structured("texel", 2000));
			Write("audio/bark.wav", Structured("sample", 1000));

			REQUIRE(cdn::Publish(Root / "content", Root / "store", Publisher()).has_value());

			auto store = ChunkStore::Open(Root / "store", false);
			REQUIRE(store.has_value());

			SignatureBytes signature;
			Catalogue = store->ReadManifest(signature);
			REQUIRE(Catalogue.has_value());

			Serving = std::make_unique<Origin>(Secret(), settings);
			auto mounted = ContentRoot::Mount(Root / "store");
			REQUIRE(mounted.has_value());
			REQUIRE(Serving->Publish(std::make_shared<const Publication>(*mounted, *Catalogue)));

			ServiceSettings service;
			service.Port = 0;
			Listening = cdn::Serve(*Serving, std::move(*store), service);
			REQUIRE(Listening != nullptr);
			Address = Endpoint::LoopbackIPv4(Listening->Local().Port);
		}

		~Deployment() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		void Write(const std::string &name, std::string_view body) {
			const fs::path path = Root / "content" / name;
			std::error_code failure;
			fs::create_directories(path.parent_path(), failure);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(body.data(), static_cast<std::streamsize>(body.size()));
		}

		std::vector<std::byte> Token(const ContentHash &bundle, uint64_t expiry = EXPIRY) {
			GrantScope scope;
			scope.Session = 42;
			scope.Bundles = {bundle};
			scope.ExpiresAtSeconds = expiry;
			scope.ByteBudget = 16 * 1024 * 1024;

			const GrantKey key = Secret();
			const auto grant = Grant::Issue(scope, key);
			REQUIRE(grant.has_value());
			return grant->Encode();
		}

		// Sends one request and drives both ends until it settles.
		std::optional<Response> Ask(const Request &request) {
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

		Request Get(std::string target) const {
			Request request;
			request.Verb = Method::Get;
			request.Target = std::move(target);
			return request;
		}
	};
}

TEST_CASE("health says what is being served", "[cdn][service]") {
	Deployment origin;
	const auto answer = origin.Ask(origin.Get("/health"));
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Ok);
	CHECK_FALSE(answer->Body.empty());
	CHECK(origin.Listening->Counters().Health == 1);
}

TEST_CASE("the manifest is served with its signature and verifies", "[cdn][service]") {
	// The published file verbatim, not a re-serialisation that is *supposed* to
	// be identical - so a client verifies what the publisher actually signed.
	Deployment origin;
	const auto answer = origin.Ask(origin.Get("/manifest"));
	REQUIRE(answer.has_value());
	REQUIRE(answer->Code == Status::Ok);
	REQUIRE(answer->Body.size() > SignatureBytes::BYTES);

	SignatureBytes signature;
	std::memcpy(signature.Value.data(), answer->Body.data(), SignatureBytes::BYTES);

	engine::core::ByteReader reader(
		std::span<const std::byte>(
			answer->Body.data() + SignatureBytes::BYTES, answer->Body.size() - SignatureBytes::BYTES
		)
	);
	const auto parsed = Manifest::Read(reader);
	REQUIRE(parsed.has_value());
	CHECK(parsed->Root() == origin.Catalogue->Root());
	CHECK(engine::assets::VerifyManifestRoot(parsed->Root(), signature, Publisher().Public()));
}

TEST_CASE("a group is served compressed and expands to what the manifest says", "[cdn][service]") {
	// **The compression check, at the wire.** The frame that crossed the socket
	// must be smaller than the payload it expands to, and it must expand to
	// exactly the length the *signed manifest* records - never to whatever the
	// frame header claims.
	Deployment origin;
	REQUIRE(origin.Catalogue->Bundles().size() >= 1);
	const auto &bundle = origin.Catalogue->Bundles()[0];

	Request request = origin.Get("/bundle/" + bundle.Root.ToHex());
	request.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(origin.Token(bundle.Root))});

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	REQUIRE(answer->Code == Status::Ok);
	REQUIRE_FALSE(answer->Body.empty());

	// Compressed on the wire.
	CHECK(answer->Body.size() < bundle.TotalBytes);

	const auto payload = GroupCodec::Decompress(answer->Body, bundle.TotalBytes);
	REQUIRE(payload.has_value());
	CHECK(payload->size() == bundle.TotalBytes);

	// And the bytes are the ones the store holds - the round trip is lossless.
	auto store = ChunkStore::Open(origin.Root / "store", false);
	REQUIRE(store.has_value());
	const auto direct = store->ReadBundle(*origin.Catalogue, bundle);
	REQUIRE(direct.has_value());
	CHECK(*payload == *direct);
}

TEST_CASE("a request with no grant is refused", "[cdn][service]") {
	Deployment origin;
	const auto &bundle = origin.Catalogue->Bundles()[0];

	const auto answer = origin.Ask(origin.Get("/bundle/" + bundle.Root.ToHex()));
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Forbidden);
	// A refusal carries no reason: one returned to a client is an oracle.
	CHECK(answer->Body.empty());
	CHECK(origin.Listening->Counters().Refused == 1);
}

TEST_CASE("a grant from the wrong key is refused", "[cdn][service]") {
	Deployment origin;
	const auto &bundle = origin.Catalogue->Bundles()[0];

	GrantScope scope;
	scope.Session = 42;
	scope.Bundles = {bundle.Root};
	scope.ExpiresAtSeconds = EXPIRY;
	scope.ByteBudget = 1024 * 1024;
	const GrantKey wrong = Secret(200);
	const auto forged = Grant::Issue(scope, wrong);
	REQUIRE(forged.has_value());

	Request request = origin.Get("/bundle/" + bundle.Root.ToHex());
	request.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(forged->Encode())});

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Forbidden);
}

TEST_CASE("an expired grant is refused", "[cdn][service]") {
	Deployment origin;
	const auto &bundle = origin.Catalogue->Bundles()[0];

	Request request = origin.Get("/bundle/" + bundle.Root.ToHex());
	request.Headers.push_back({
		.Name = "x-atomic-grant",
		.Value = ToHex(origin.Token(bundle.Root, NOW - 1)),
	});

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Forbidden);
}

TEST_CASE("a grant for one bundle does not admit another", "[cdn][service]") {
	Deployment origin;
	REQUIRE(origin.Catalogue->Bundles().size() >= 1);
	const auto &bundle = origin.Catalogue->Bundles()[0];

	// A grant naming content that is not what is being asked for.
	const ContentHash elsewhere =
		engine::assets::Hasher::Of(std::as_bytes(std::span<const uint8_t>(bundle.Root.Digest.data(), 8)));

	Request request = origin.Get("/bundle/" + bundle.Root.ToHex());
	request.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(origin.Token(elsewhere))});

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Forbidden);
}

TEST_CASE("a bundle target that is not a hash is refused", "[cdn][service]") {
	// There is no route that takes a name, so there is nothing to walk out of a
	// directory - CDN.md §8.
	Deployment origin;

	for (const std::string &target : {
			 std::string("/bundle/../../etc/passwd"),
			 std::string("/bundle/not-hex"),
			 std::string("/bundle/"),
			 std::string("/bundle/ABCD"),
		 }) {
		INFO("target " << target);
		const auto answer = origin.Ask(origin.Get(target));
		REQUIRE(answer.has_value());
		CHECK(answer->Code == Status::BadRequest);
	}
}

TEST_CASE("a bundle this origin does not have is a miss, not a refusal", "[cdn][service]") {
	// Counted apart: an operator cannot otherwise tell a misconfigured
	// deployment from somebody probing it.
	Deployment origin;
	// A hash of something this store does not contain.
	const std::array<uint8_t, 4> nothing{9, 9, 9, 9};
	const ContentHash absent = engine::assets::Hasher::Of(std::as_bytes(std::span(nothing)));

	Request request = origin.Get("/bundle/" + absent.ToHex());
	request.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(origin.Token(absent))});

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	// Admitted by the grant and absent from the publication.
	CHECK(answer->Code == Status::NotFound);
	CHECK(origin.Listening->Counters().Missing == 1);
}

TEST_CASE("an unknown route is answered rather than dropped", "[cdn][service]") {
	Deployment origin;
	const auto answer = origin.Ask(origin.Get("/admin"));
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::NotFound);
}

TEST_CASE("an origin with no dictionary answers 404 for one", "[cdn][service]") {
	// Ordinary rather than an error: its groups are compressed without one and
	// the client carries on.
	Deployment origin;
	const auto answer = origin.Ask(origin.Get("/dictionary"));
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::NotFound);
}

TEST_CASE("a range makes a large group resumable", "[cdn][service]") {
	// A client holding part of a bundle asks for the rest rather than paying
	// for the first part twice.
	Deployment origin;
	const auto &bundle = origin.Catalogue->Bundles()[0];

	Request whole = origin.Get("/bundle/" + bundle.Root.ToHex());
	whole.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(origin.Token(bundle.Root))});
	const auto full = origin.Ask(whole);
	REQUIRE(full.has_value());
	REQUIRE(full->Code == Status::Ok);
	REQUIRE(full->Body.size() > 64);

	Request partial = whole;
	partial.Range = engine::net::http::ByteRange{.First = 16, .Last = 47, .Suffix = false};
	const auto piece = origin.Ask(partial);
	REQUIRE(piece.has_value());
	CHECK(piece->Code == Status::PartialContent);
	REQUIRE(piece->Body.size() == 32);

	// And it is the same 32 bytes the whole response had at that offset.
	const std::vector<std::byte> expected(full->Body.begin() + 16, full->Body.begin() + 48);
	CHECK(piece->Body == expected);

	REQUIRE(piece->Find("content-range").has_value());
	CHECK(*piece->Find("content-range") == "bytes 16-47/" + std::to_string(full->Body.size()));
}

TEST_CASE("a range past the end is 416 rather than an empty 200", "[cdn][service]") {
	// An empty 200 would be taken as a zero-length asset.
	Deployment origin;

	Request request = origin.Get("/manifest");
	request.Range = engine::net::http::ByteRange{.First = 1'000'000, .Last = 1'000'100, .Suffix = false};

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::RangeNotSatisfiable);
}

TEST_CASE("a head request answers the size without the bytes", "[cdn][service]") {
	// What a client uses to learn a group's size before spending the bandwidth.
	Deployment origin;

	Request request = origin.Get("/manifest");
	request.Verb = Method::Head;

	const auto answer = origin.Ask(request);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Ok);
	CHECK(answer->Body.empty());
	REQUIRE(answer->Find("content-length").has_value());
	CHECK(*answer->Find("content-length") != "0");
}

TEST_CASE("a prepared group is served from the cache the second time", "[cdn][service]") {
	// Preparing per request would make an origin's cost scale with its
	// popularity rather than with its content - exactly backwards.
	Deployment origin;
	const auto &bundle = origin.Catalogue->Bundles()[0];

	Request request = origin.Get("/bundle/" + bundle.Root.ToHex());
	request.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(origin.Token(bundle.Root))});

	const auto first = origin.Ask(request);
	REQUIRE(first.has_value());
	REQUIRE(first->Code == Status::Ok);
	CHECK(origin.Serving->Cache().Count() >= 1);

	const auto second = origin.Ask(request);
	REQUIRE(second.has_value());
	REQUIRE(second->Code == Status::Ok);
	// Byte-identical, because preparation is deterministic and the second
	// request was answered from the cache.
	CHECK(second->Body == first->Body);
	CHECK(origin.Listening->Counters().Bundles == 2);
}
