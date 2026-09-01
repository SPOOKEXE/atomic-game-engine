#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Project.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <server/Server.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("server.project")
TEST_DEPENDS("engine.game.project")
TEST_DEPENDS("server.host")

namespace {
	namespace fs = std::filesystem;

	struct Tree {
		fs::path Root;

		explicit Tree(std::string_view name) {
			Root = fs::temp_directory_path() / ("atomic-server-project-" + std::string(name));
			std::error_code ignored;
			fs::remove_all(Root, ignored);
			fs::create_directories(Root);
		}

		~Tree() {
			std::error_code ignored;
			fs::remove_all(Root, ignored);
		}
	};

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}

	engine::assets::SigningKey Key() {
		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index + 3);
		}
		auto key = engine::assets::SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	void AddWorld(engine::world::Universe &universe) {
		engine::world::WorldSettings world;
		world.Name = engine::core::Name("Hosted");
		world.TickRate = 30.0;
		REQUIRE(universe.Create(world).IsValid());
	}

	server::Options Headless(const fs::path &project) {
		server::Options options;
		options.GamePath = project.string();
		options.MaximumTicks = 1;
		options.Unpaced = true;
		return options;
	}

	std::string PrepareStore(const fs::path &root) {
		auto key = Key();
		auto store = engine::assets::ChunkStore::Open(root / "assets", true);
		REQUIRE(store.has_value());
		const std::vector<std::byte> content = Bytes("hosted content");
		const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(content);
		REQUIRE(store->Write(chunk, content));
		engine::assets::Manifest manifest;
		const engine::assets::ContentHash asset = manifest.AddAsset(
			"data/hosted.bin",
			engine::assets::AssetKind::Data,
			{{.Hash = chunk, .Bytes = static_cast<uint32_t>(content.size())}}
		);
		const std::array roots{asset};
		REQUIRE(manifest.AddBundle(roots).has_value());
		REQUIRE(store->WriteManifest(manifest, key.SignManifestRoot(manifest.Root())));
		return key.Public().ToHex();
	}
}

TEST_CASE("server loads a multi-file universe directly", "[server][project]") {
	Tree tree("universe");
	engine::world::Universe universe;
	AddWorld(universe);
	std::string error;
	const fs::path manifest = tree.Root / "hosted.auniverse";
	REQUIRE(
		engine::game::SaveUniverse(universe, engine::core::Name("Hosted Universe"), {}, manifest, {}, error)
	);

	server::Server host;
	REQUIRE(host.Initialise(Headless(manifest)));
	CHECK(host.Worlds().Find(engine::core::Name("Hosted")).IsValid());
	CHECK(host.Run().Ticks == 1);
	host.Shutdown();
}

TEST_CASE("shared project loading preserves monolithic game support", "[server][project]") {
	Tree tree("game");
	engine::world::Universe universe;
	AddWorld(universe);
	std::string error;
	const fs::path game = tree.Root / "hosted.agame";
	REQUIRE(engine::game::SaveGame(universe, engine::core::Name("Hosted Game"), game, error));

	server::Server host;
	REQUIRE(host.Initialise(Headless(game)));
	CHECK(host.Worlds().Find(engine::core::Name("Hosted")).IsValid());
	host.Shutdown();
}

TEST_CASE("server owns and hosts a self-contained Project ZIP", "[server][project]") {
	Tree tree("zip");
	const fs::path staging = tree.Root / "staging";
	fs::create_directories(staging);
	const std::string publisher = PrepareStore(staging);

	engine::world::Universe universe;
	AddWorld(universe);
	engine::game::UniverseFileOptions universeOptions;
	universeOptions.PublisherKey = publisher;
	std::string error;
	REQUIRE(
		engine::game::SaveUniverse(
			universe,
			engine::core::Name("Hosted Package"),
			{},
			staging / "game.auniverse",
			universeOptions,
			error
		)
	);
	engine::game::ProjectPackageOptions packageOptions;
	packageOptions.PublisherKey = publisher;
	packageOptions.Delivery = engine::game::ProjectDeliveryPreference::Redirect;
	engine::game::ProjectPackageInfo info;
	engine::game::ProjectValidationReport report;
	const fs::path package = tree.Root / "hosted.zip";
	REQUIRE(engine::game::WriteProjectPackage(staging, package, packageOptions, info, report));
	const fs::path operatorPackage = tree.Root / "operator.zip";
	fs::copy_file(package, operatorPackage);
	const fs::path unadvertisedPackage = tree.Root / "unadvertised.zip";
	fs::copy_file(package, unadvertisedPackage);

	auto unadvertised = Headless(unadvertisedPackage);
	unadvertised.ContentGrantKey = std::string(64, '1');
	server::Server refusedRedirect;
	CHECK_FALSE(refusedRedirect.Initialise(unadvertised));
	refusedRedirect.Shutdown();

	auto options = Headless(package);
	options.ContentGrantKey = std::string(64, '1');
	options.ContentPublicHost = "127.0.0.1";
	server::Server host;
	REQUIRE(host.Initialise(options));
	CHECK(host.Worlds().Find(engine::core::Name("Hosted")).IsValid());
	CHECK(host.ContentRelayStats() == nullptr);
	fs::remove(package);
	CHECK(host.Run().Ticks == 1);
	host.Shutdown();

	auto overridden = Headless(operatorPackage);
	overridden.ContentGrantKey = std::string(64, '1');
	overridden.ContentPublicHost = "127.0.0.1";
	overridden.ContentDeliveryConfigured = true;
	overridden.ContentDelivery = server::ContentMode::Relay;
	server::Server operatorHost;
	REQUIRE(operatorHost.Initialise(overridden));
	CHECK(operatorHost.ContentRelayStats() != nullptr);
	operatorHost.Shutdown();
}

TEST_CASE("server policy denies package HTTP without local content", "[server][project]") {
	Tree tree("http-denied");
	engine::world::Universe universe;
	AddWorld(universe);
	auto key = Key();
	engine::game::UniverseFileOptions options;
	options.HttpEnabled = true;
	options.PublisherKey = key.Public().ToHex();
	options.Cdns.push_back({"public", "127.0.0.1:9080"});
	std::string error;
	const fs::path manifest = tree.Root / "remote.auniverse";
	REQUIRE(engine::game::SaveUniverse(universe, engine::core::Name("Remote"), {}, manifest, options, error));

	server::Server host;
	CHECK_FALSE(host.Initialise(Headless(manifest)));
	host.Shutdown();
}

TEST_CASE("operator content source overrides a denied package HTTP hint", "[server][project]") {
	Tree tree("operator-source");
	const std::string publisher = PrepareStore(tree.Root / "operator");
	engine::world::Universe universe;
	AddWorld(universe);
	engine::game::UniverseFileOptions universeOptions;
	universeOptions.HttpEnabled = true;
	universeOptions.PublisherKey = publisher;
	universeOptions.Cdns.push_back({"upstream", "127.0.0.1:9080"});
	std::string error;
	const fs::path manifest = tree.Root / "operator.auniverse";
	REQUIRE(
		engine::game::SaveUniverse(
			universe, engine::core::Name("Operator Source"), {}, manifest, universeOptions, error
		)
	);

	auto options = Headless(manifest);
	options.ContentSources.push_back("UPSTREAM=dir:" + (tree.Root / "operator/assets").string());
	server::Server host;
	REQUIRE(host.Initialise(options));
	CHECK(host.Worlds().Find(engine::core::Name("Hosted")).IsValid());
	CHECK(host.ContentRelayStats() != nullptr);
	host.Shutdown();
}
