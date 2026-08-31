// A universe export can become a self-contained processed content store.

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/LocalStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/game/Game.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <studio/AssetGrounding.hpp>
#include <studio/Editor.hpp>
#include <system_error>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.assetgrounding")
TEST_DEPENDS("engine.delivery.relay")
TEST_DEPENDS("engine.assets.chunkstore")

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}

	struct GroundingTree {
		std::filesystem::path Root;

		GroundingTree() {
			static int serial = 0;
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-studio-grounding-" + std::to_string(++serial));
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}

		~GroundingTree() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
	};
}

TEST_CASE("grounding copies verified processed assets and their signed manifest", "[studio][grounding]") {
	GroundingTree tree;
	const std::filesystem::path sourcePath = tree.Root / "source";
	const std::filesystem::path destinationPath = tree.Root / "grounded";
	const std::filesystem::path rawPath = tree.Root / "raw-source";
	std::filesystem::create_directories(rawPath / "models");
	{
		std::ofstream(rawPath / "models/rock.obj") << "raw mesh";
		std::ofstream(rawPath / "notes.txt") << "author notes";
	}
	std::error_code linkError;
	std::filesystem::create_symlink(tree.Root / "outside.txt", rawPath / "ignored-link", linkError);
	auto source = engine::assets::ChunkStore::Open(sourcePath, true);
	REQUIRE(source.has_value());

	engine::assets::Manifest manifest;
	std::vector<engine::assets::ContentHash> roots;
	for (const auto &[name, text] : std::vector<std::pair<std::string_view, std::string_view>>{
			 {"meshes/rock.amesh", "processed mesh"}, {"textures/rock.atex", "processed texture"}
		 }) {
		const std::vector<std::byte> bytes = Bytes(text);
		const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(bytes);
		REQUIRE(source->Write(chunk, bytes));
		roots.push_back(manifest.AddAsset(
			std::string(name),
			engine::assets::KindOfName(name),
			{engine::assets::ChunkEntry{.Hash = chunk, .Bytes = static_cast<uint32_t>(bytes.size())}}
		));
	}
	REQUIRE(manifest.AddBundle(roots).has_value());
	const engine::assets::SignatureBytes signature =
		engine::assets::DevelopmentSigningKey().SignManifestRoot(manifest.Root());
	REQUIRE(source->WriteManifest(manifest, signature));

	engine::delivery::DeliverySettings settings;
	settings.Publisher = engine::assets::DevelopmentPublisher();
	settings.Sources.push_back(
		engine::delivery::Source{
			.Name = "fixture",
			.Kind = engine::delivery::SourceKind::Directory,
			.Location = sourcePath.string(),
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
			.IngestKey = {},
		}
	);
	std::unique_ptr<engine::delivery::AssetClient> client = engine::delivery::MakeAssetClient(settings);
	REQUIRE(client != nullptr);

	studio::AssetGrounding grounding;
	const std::vector<std::filesystem::path> rawSources{rawPath};
	REQUIRE(studio::BeginAssetGrounding(grounding, destinationPath, rawSources));
	for (size_t attempt = 0; attempt < 100 && grounding.State != studio::AssetGroundingState::Complete;
		 attempt++) {
		client->Pump();
		studio::PumpAssetGrounding(grounding, *client);
	}
	REQUIRE(grounding.State == studio::AssetGroundingState::Complete);
	CHECK(grounding.Completed == 2);
	CHECK(grounding.Error.empty());

	auto grounded = engine::assets::ChunkStore::Open(destinationPath, false);
	REQUIRE(grounded.has_value());
	engine::assets::SignatureBytes groundedSignature;
	const std::optional<engine::assets::Manifest> groundedManifest =
		grounded->ReadManifest(groundedSignature);
	REQUIRE(groundedManifest.has_value());
	CHECK(groundedSignature == signature);
	CHECK(groundedManifest->Root() == manifest.Root());
	for (const engine::assets::AssetEntry &entry : groundedManifest->Assets()) {
		CHECK(grounded->ReadAsset(entry).has_value());
	}
	const std::filesystem::path groundedRaw = destinationPath / "raw/1-raw-source";
	CHECK(std::filesystem::is_regular_file(groundedRaw / "models/rock.obj"));
	CHECK(std::filesystem::is_regular_file(groundedRaw / "notes.txt"));
	CHECK_FALSE(std::filesystem::exists(groundedRaw / "ignored-link"));
}

TEST_CASE("standalone world export grounds processed and raw assets before writing", "[studio][grounding]") {
	GroundingTree tree;
	const std::filesystem::path sourcePath = tree.Root / "source";
	const std::filesystem::path rawPath = tree.Root / "raw-source";
	std::filesystem::create_directories(rawPath / "models");
	std::ofstream(rawPath / "models/tree.obj") << "raw tree";

	auto source = engine::assets::ChunkStore::Open(sourcePath, true);
	REQUIRE(source.has_value());
	const std::vector<std::byte> bytes = Bytes("processed tree");
	const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(bytes);
	REQUIRE(source->Write(chunk, bytes));
	engine::assets::Manifest manifest;
	const engine::assets::ContentHash root = manifest.AddAsset(
		"meshes/tree.amesh",
		engine::assets::KindOfName("meshes/tree.amesh"),
		{engine::assets::ChunkEntry{.Hash = chunk, .Bytes = static_cast<uint32_t>(bytes.size())}}
	);
	const std::array roots{root};
	REQUIRE(manifest.AddBundle(roots).has_value());
	const engine::assets::SignatureBytes signature =
		engine::assets::DevelopmentSigningKey().SignManifestRoot(manifest.Root());
	REQUIRE(source->WriteManifest(manifest, signature));

	engine::delivery::DeliverySettings delivery;
	delivery.Publisher = engine::assets::DevelopmentPublisher();
	delivery.Sources.push_back(
		engine::delivery::Source{
			.Name = "fixture",
			.Kind = engine::delivery::SourceKind::Directory,
			.Location = sourcePath.string(),
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
			.IngestKey = {},
		}
	);

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings world;
	world.Name = engine::core::Name("Standalone");
	editor.Active = editor.Universe->Create(world);
	REQUIRE(editor.Active.IsValid());
	editor.ContentClient = engine::delivery::MakeAssetClient(delivery);
	REQUIRE(editor.ContentClient != nullptr);
	editor.Content.RawFolders.push_back(rawPath);

	const std::filesystem::path exportPath = tree.Root / "export" / "standalone.aworld";
	editor.BeginWorldExport(exportPath, true, true);
	for (size_t attempt = 0; attempt < 100 && !std::filesystem::is_regular_file(exportPath); attempt++) {
		editor.ContentClient->Pump();
		editor.PumpAssetExport();
	}
	REQUIRE(std::filesystem::is_regular_file(exportPath));

	auto grounded = engine::assets::ChunkStore::Open(exportPath.parent_path() / "assets", false);
	REQUIRE(grounded.has_value());
	CHECK(grounded->ReadAsset(manifest.Assets().front()).has_value());
	CHECK(
		std::filesystem::is_regular_file(exportPath.parent_path() / "assets/raw/1-raw-source/models/tree.obj")
	);

	engine::world::Universe imported;
	std::string error;
	CHECK(engine::game::ImportWorld(imported, exportPath, engine::core::Name("Imported"), error).IsValid());
}
