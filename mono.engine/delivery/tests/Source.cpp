#include <engine/assets/Signature.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.delivery.source")

using engine::assets::PublicKey;
using engine::delivery::DEFAULT_ORIGIN_PORT;
using engine::delivery::DeliverySettings;
using engine::delivery::Describe;
using engine::delivery::HostPermitted;
using engine::delivery::Source;
using engine::delivery::SourceKind;

namespace {
	PublicKey SomeKey() {
		PublicKey key;
		for (size_t index = 0; index < key.Value.size(); ++index) {
			key.Value[index] = static_cast<uint8_t>(index + 1);
		}
		return key;
	}

	Source Http(std::string name, std::string location) {
		return Source{
			.Name = std::move(name),
			.Kind = SourceKind::Http,
			.Location = std::move(location),
			.Enabled = true,
		};
	}
}

TEST_CASE("the default points at this machine", "[delivery][source]") {
	// A game being developed has its content beside it, and a default pointing
	// anywhere else would make the first run depend on a network.
	const DeliverySettings settings = DeliverySettings::Default();
	REQUIRE(settings.Sources.size() == 1);
	CHECK(settings.Sources[0].Kind == SourceKind::Http);
	CHECK(settings.Sources[0].Location == "127.0.0.1:" + std::to_string(DEFAULT_ORIGIN_PORT));
}

TEST_CASE("a default with no publisher key is not usable", "[delivery][source]") {
	// There is no sensible default for a root of trust, and inventing one would
	// make an unconfigured client look configured. A client that accepts an
	// unsigned manifest has no trust boundary at all.
	DeliverySettings settings = DeliverySettings::Default();
	CHECK_FALSE(settings.IsValid());

	settings.Publisher = SomeKey();
	CHECK(settings.IsValid());
}

TEST_CASE("an http source needs host and port", "[delivery][source]") {
	CHECK(Http("a", "127.0.0.1:9080").IsValid());
	CHECK(Http("a", "[::1]:9080").IsValid());

	CHECK_FALSE(Http("a", "127.0.0.1").IsValid());
	CHECK_FALSE(Http("a", "127.0.0.1:").IsValid());
	CHECK_FALSE(Http("a", ":9080").IsValid());
	CHECK_FALSE(Http("a", "127.0.0.1:0").IsValid());
	CHECK_FALSE(Http("a", "127.0.0.1:70000").IsValid());
	CHECK_FALSE(Http("", "127.0.0.1:9080").IsValid());
	CHECK_FALSE(Http("a", "").IsValid());
}

TEST_CASE("order is priority and a disabled source is skipped but kept", "[delivery][source]") {
	// Kept rather than removed, so the studio can turn an origin off without
	// losing how it was configured - which is what somebody wants while they
	// are working out which one is broken.
	DeliverySettings settings;
	settings.Publisher = SomeKey();
	settings.Sources = {
		Http("near", "127.0.0.1:9080"),
		Http("far", "10.0.0.5:9080"),
	};
	settings.Sources[0].Enabled = false;

	const std::vector<Source> usable = settings.Usable();
	REQUIRE(usable.size() == 1);
	CHECK(usable[0].Name == "far");
	// The disabled row is still in the settings.
	CHECK(settings.Sources.size() == 2);
}

TEST_CASE("an invalid source is passed over rather than failing the list", "[delivery][source]") {
	DeliverySettings settings;
	settings.Publisher = SomeKey();
	settings.Sources = {
		Http("broken", "not-an-address"),
		Http("working", "127.0.0.1:9080"),
	};

	const std::vector<Source> usable = settings.Usable();
	REQUIRE(usable.size() == 1);
	CHECK(usable[0].Name == "working");
	CHECK(settings.IsValid());
}

TEST_CASE("settings naming no usable source are not valid", "[delivery][source]") {
	DeliverySettings settings;
	settings.Publisher = SomeKey();
	settings.Sources = {Http("broken", "not-an-address")};
	CHECK_FALSE(settings.IsValid());
}

// --- the request-forgery check --------------------------------------------

TEST_CASE("an empty allow-list permits anything", "[delivery][source]") {
	// Right for a list a person typed into their own preferences. A list
	// assembled from session data a server sent must fill it in.
	CHECK(HostPermitted("10.0.0.5:9080", {}));
}

TEST_CASE("a host outside the allow-list is refused", "[delivery][source]") {
	// A client told to fetch from an arbitrary host is a request-forgery
	// primitive, so a descriptor is validated against the session's declared
	// origins.
	const std::vector<std::string> allowed{"cdn.example.com", "127.0.0.1"};

	CHECK(HostPermitted("127.0.0.1:9080", allowed));
	CHECK(HostPermitted("cdn.example.com:443", allowed));
	CHECK_FALSE(HostPermitted("169.254.169.254:80", allowed));
	CHECK_FALSE(HostPermitted("evil.example.com:9080", allowed));
}

TEST_CASE("the allow-list matches the host and not the port", "[delivery][source]") {
	// An operator declares which machines are permitted; a port is a deployment
	// detail of a machine that was already permitted.
	const std::vector<std::string> allowed{"127.0.0.1"};
	CHECK(HostPermitted("127.0.0.1:9080", allowed));
	CHECK(HostPermitted("127.0.0.1:1234", allowed));
}

TEST_CASE("a refused host makes that source unusable", "[delivery][source]") {
	// The check lives with the source list rather than at the call sites, so a
	// call site added later cannot forget it.
	DeliverySettings settings;
	settings.Publisher = SomeKey();
	settings.Sources = {Http("elsewhere", "169.254.169.254:80")};
	settings.AllowedHosts = {"127.0.0.1"};

	CHECK(settings.Usable().empty());
	CHECK_FALSE(settings.IsValid());
}

TEST_CASE("every source kind has a name", "[delivery][source]") {
	CHECK(std::string(Describe(SourceKind::Directory)) == "directory");
	CHECK(std::string(Describe(SourceKind::Http)) == "http");
}
