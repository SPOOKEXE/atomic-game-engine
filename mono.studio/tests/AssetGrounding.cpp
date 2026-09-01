// A universe export can become a self-contained processed content store.

#include "SourceEditor.hpp"

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/LocalStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Project.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
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
#include <server/Server.hpp>
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

TEST_CASE("raw-only grounding uses an explicit authoring root and still skips links", "[studio][grounding]") {
	GroundingTree tree;
	const std::filesystem::path rawPath = tree.Root / "raw-source";
	const std::filesystem::path assetsPath = tree.Root / "staging/assets";
	const std::filesystem::path authoringPath = tree.Root / "staging/authoring";
	std::filesystem::create_directories(rawPath / "nested");
	std::ofstream(rawPath / "nested/source.txt") << "source";
	std::error_code linkError;
	std::filesystem::create_symlink(rawPath / "nested/source.txt", rawPath / "linked.txt", linkError);

	studio::AssetGrounding grounding;
	const std::array sources{rawPath};
	REQUIRE(studio::BeginAssetGrounding(grounding, assetsPath, sources, authoringPath, false));
	for (size_t attempt = 0; attempt < 32 && grounding.State != studio::AssetGroundingState::Complete;
		 attempt++) {
		studio::PumpAssetGrounding(grounding, static_cast<engine::delivery::AssetClient *>(nullptr));
	}
	REQUIRE(grounding.State == studio::AssetGroundingState::Complete);
	CHECK(std::filesystem::is_regular_file(authoringPath / "1-raw-source/nested/source.txt"));
	CHECK_FALSE(std::filesystem::exists(authoringPath / "1-raw-source/linked.txt"));
	CHECK_FALSE(std::filesystem::exists(assetsPath / engine::assets::ChunkStore::MANIFEST_FILE));
}

TEST_CASE(
	"cancelling raw export removes staging and leaves no final document", "[studio][grounding][export]"
) {
	GroundingTree tree;
	const std::filesystem::path rawPath = tree.Root / "raw-source";
	std::filesystem::create_directories(rawPath);
	for (size_t index = 0; index < 24; index++) {
		std::ofstream(rawPath / ("file-" + std::to_string(index) + ".txt")) << index;
	}

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings world;
	world.Name = engine::core::Name("Cancel");
	editor.Active = editor.Universe->Create(world);
	REQUIRE(editor.Active.IsValid());
	editor.Content.RawFolders.push_back(rawPath);

	studio::ExportOptions choices;
	choices.Product = engine::game::ExportProduct::WorldFile;
	choices.IncludeRawAuthoring = true;
	engine::game::ProjectValidationReport report;
	const std::filesystem::path destination = tree.Root / "cancelled.aworld";
	const auto request = studio::BuildExportRequest(destination, choices, report);
	REQUIRE(request.has_value());
	REQUIRE(editor.BeginExport(*request));
	const std::filesystem::path staging = editor.ExportStagingRoot;
	REQUIRE(std::filesystem::is_directory(staging));
	editor.PumpAssetExport();
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::CopyRawFiles);
	editor.CancelExport();
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::Cancelled);
	CHECK_FALSE(std::filesystem::exists(staging));
	CHECK_FALSE(std::filesystem::exists(destination));
}

TEST_CASE(
	"export cancellation is safe while waiting for and fetching catalogue assets",
	"[studio][grounding][export]"
) {
	GroundingTree tree;
	const std::filesystem::path sourcePath = tree.Root / "source";
	auto source = engine::assets::ChunkStore::Open(sourcePath, true);
	REQUIRE(source.has_value());
	const std::vector<std::byte> bytes = Bytes("pending asset");
	const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(bytes);
	REQUIRE(source->Write(chunk, bytes));
	engine::assets::Manifest manifest;
	const engine::assets::ContentHash root = manifest.AddAsset(
		"data/pending.bin",
		engine::assets::AssetKind::Data,
		{{.Hash = chunk, .Bytes = static_cast<uint32_t>(bytes.size())}}
	);
	const std::array roots{root};
	REQUIRE(manifest.AddBundle(roots).has_value());
	REQUIRE(source->WriteManifest(
		manifest, engine::assets::DevelopmentSigningKey().SignManifestRoot(manifest.Root())
	));

	engine::delivery::DeliverySettings delivery;
	delivery.Publisher = engine::assets::DevelopmentPublisher();
	delivery.Sources.push_back(
		engine::delivery::Source{
			.Name = "fixture",
			.Kind = engine::delivery::SourceKind::Directory,
			.Location = sourcePath.string(),
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
		}
	);

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings world;
	world.Name = engine::core::Name("Cancellation");
	editor.Active = editor.Universe->Create(world);
	REQUIRE(editor.Active.IsValid());
	editor.ContentClient = engine::delivery::MakeAssetClient(delivery);
	REQUIRE(editor.ContentClient != nullptr);

	studio::ExportOptions choices;
	choices.Product = engine::game::ExportProduct::WorldFile;
	choices.IncludeProcessedAssets = true;
	engine::game::ProjectValidationReport requestReport;
	const std::filesystem::path waitingPath = tree.Root / "waiting.aworld";
	auto request = studio::BuildExportRequest(waitingPath, choices, requestReport);
	REQUIRE(request.has_value());
	REQUIRE(editor.BeginExport(*request));
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::ValidateCatalogue);
	const std::filesystem::path waitingStaging = editor.ExportStagingRoot;
	editor.CancelExport();
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::Cancelled);
	CHECK_FALSE(std::filesystem::exists(waitingStaging));
	CHECK_FALSE(std::filesystem::exists(waitingPath));

	for (size_t attempt = 0; attempt < 32 && !editor.ContentClient->Ready(); attempt++) {
		editor.ContentClient->Pump();
	}
	REQUIRE(editor.ContentClient->Ready());
	const std::filesystem::path fetchingPath = tree.Root / "fetching.aworld";
	request = studio::BuildExportRequest(fetchingPath, choices, requestReport);
	REQUIRE(request.has_value());
	REQUIRE(editor.BeginExport(*request));
	editor.PumpAssetExport();
	REQUIRE(editor.CurrentExportPhase() == studio::ExportPhase::FetchAssets);
	const std::filesystem::path fetchingStaging = editor.ExportStagingRoot;
	editor.CancelExport();
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::Cancelled);
	CHECK_FALSE(std::filesystem::exists(fetchingStaging));
	CHECK_FALSE(std::filesystem::exists(fetchingPath));
}

TEST_CASE("world export flushes unsaved Program and ShaderScript buffers", "[studio][export]") {
	GroundingTree tree;
	engine::scene::EnsureClassTree();
	engine::script::ScriptClass();
	engine::scene::ShaderScriptClass();

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("Sources");
	editor.Active = editor.Universe->Create(settings);
	REQUIRE(editor.Active.IsValid());

	engine::ecs::Entity program = engine::ecs::NULL_ENTITY;
	engine::ecs::Entity shader = engine::ecs::NULL_ENTITY;
	editor.Universe->Enter(editor.Active, [&](engine::ecs::Store &store) {
		program = store.CreateInstance(engine::script::ScriptClass(), "Program");
		shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shader");
	});
	REQUIRE(program != engine::ecs::NULL_ENTITY);
	REQUIRE(shader != engine::ecs::NULL_ENTITY);
	studio::OpenScript programTab;
	programTab.World = editor.Active;
	programTab.Instance = program;
	programTab.Text = "print('exported program')";
	programTab.Modified = true;
	editor.Scripts.push_back(std::move(programTab));
	studio::OpenScript shaderTab;
	shaderTab.World = editor.Active;
	shaderTab.Instance = shader;
	shaderTab.Shader = true;
	shaderTab.Text = "void main() { discard; }";
	shaderTab.Modified = true;
	editor.Scripts.push_back(std::move(shaderTab));

	studio::ExportOptions choices;
	choices.Product = engine::game::ExportProduct::WorldFile;
	engine::game::ProjectValidationReport requestReport;
	const std::filesystem::path destination = tree.Root / "sources.aworld";
	const auto request = studio::BuildExportRequest(destination, choices, requestReport);
	REQUIRE(request.has_value());
	REQUIRE(editor.BeginExport(*request));
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::Complete);
	CHECK_FALSE(editor.Scripts[0].Modified);
	CHECK_FALSE(editor.Scripts[1].Modified);

	engine::world::Universe loaded;
	std::string error;
	const engine::world::WorldId loadedWorld =
		engine::game::ImportWorld(loaded, destination, engine::core::Name("Loaded"), error);
	REQUIRE(loadedWorld.IsValid());
	loaded.Enter(loadedWorld, [&](engine::ecs::Store &store) {
		const auto *cache = store.Resource<engine::script::SourceCache>();
		REQUIRE(cache != nullptr);
		const std::string *programText = cache->Find(engine::core::Name("Scripts/Program.luau"));
		REQUIRE(programText != nullptr);
		CHECK(*programText == "print('exported program')");
		const engine::scene::ShaderText shaderText =
			engine::scene::ShaderTextOf(store, engine::core::Name("Shader"));
		REQUIRE(shaderText.Found);
		CHECK(shaderText.Code == "void main() { discard; }");
	});
}

TEST_CASE("export replacement is explicit and preserves one recoverable prior document", "[studio][export]") {
	GroundingTree tree;
	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("Replacement");
	editor.Active = editor.Universe->Create(settings);
	REQUIRE(editor.Active.IsValid());

	const std::filesystem::path destination = tree.Root / "replacement.aworld";
	std::filesystem::create_directories(tree.Root);
	std::ofstream(destination) << "prior document";
	studio::ExportOptions choices;
	choices.Product = engine::game::ExportProduct::WorldFile;
	engine::game::ProjectValidationReport report;
	auto request = studio::BuildExportRequest(destination, choices, report);
	REQUIRE(request.has_value());
	CHECK_FALSE(editor.BeginExport(*request));

	choices.ReplaceExisting = true;
	request = studio::BuildExportRequest(destination, choices, report);
	REQUIRE(request.has_value());
	REQUIRE(editor.BeginExport(*request));
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::Complete);
	CHECK(std::filesystem::is_regular_file(destination));
	const std::filesystem::path backup = destination.string() + ".previous";
	REQUIRE(std::filesystem::is_regular_file(backup));
	std::ifstream prior(backup);
	std::string priorText;
	std::getline(prior, priorText);
	CHECK(priorText == "prior document");

	CHECK_FALSE(editor.BeginExport(*request));
	CHECK(std::filesystem::is_regular_file(destination));
	CHECK(std::filesystem::is_regular_file(backup));
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

TEST_CASE(
	"Studio Project ZIP contains every catalogue asset and hosts without source folders",
	"[studio][grounding][export]"
) {
	GroundingTree tree;
	const std::filesystem::path sourcePath = tree.Root / "source";
	const std::filesystem::path rawPath = tree.Root / "raw-source";
	std::filesystem::create_directories(rawPath);
	std::ofstream(rawPath / "model.blend") << "authoring source";

	auto source = engine::assets::ChunkStore::Open(sourcePath, true);
	REQUIRE(source.has_value());
	engine::assets::Manifest manifest;
	std::vector<engine::assets::ContentHash> roots;
	for (const auto &[name, text] : std::vector<std::pair<std::string_view, std::string_view>>{
			 {"meshes/one.amesh", "mesh one"}, {"textures/two.atex", "texture two"}
		 }) {
		const std::vector<std::byte> content = Bytes(text);
		const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(content);
		REQUIRE(source->Write(chunk, content));
		roots.push_back(manifest.AddAsset(
			std::string(name),
			engine::assets::KindOfName(name),
			{{.Hash = chunk, .Bytes = static_cast<uint32_t>(content.size())}}
		));
	}
	REQUIRE(manifest.AddBundle(roots).has_value());
	const auto &key = engine::assets::DevelopmentSigningKey();
	REQUIRE(source->WriteManifest(manifest, key.SignManifestRoot(manifest.Root())));

	engine::delivery::DeliverySettings delivery;
	delivery.Publisher = engine::assets::DevelopmentPublisher();
	delivery.Sources.push_back(
		engine::delivery::Source{
			.Name = "fixture",
			.Kind = engine::delivery::SourceKind::Directory,
			.Location = sourcePath.string(),
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
		}
	);

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings world;
	world.Name = engine::core::Name("Package World");
	editor.Active = editor.Universe->Create(world);
	REQUIRE(editor.Active.IsValid());
	editor.GameName = engine::core::Name("Portable");
	editor.ContentClient = engine::delivery::MakeAssetClient(delivery);
	REQUIRE(editor.ContentClient != nullptr);
	editor.Content.PublisherKey = engine::assets::DevelopmentPublisher().ToHex();
	editor.Content.RawFolders.push_back(rawPath);

	studio::ExportOptions choices;
	choices.Product = engine::game::ExportProduct::ProjectZip;
	choices.IncludeRawAuthoring = true;
	engine::game::ProjectValidationReport requestReport;
	const std::filesystem::path packagePath = tree.Root / "portable.zip";
	const auto request = studio::BuildExportRequest(packagePath, choices, requestReport);
	REQUIRE(request.has_value());
	REQUIRE(editor.BeginExport(*request));
	for (size_t attempt = 0; attempt < 200 && editor.ExportInProgress(); attempt++) {
		editor.ContentClient->Pump();
		editor.PumpAssetExport();
	}
	REQUIRE(std::filesystem::is_regular_file(packagePath));
	CHECK(editor.CurrentExportPhase() == studio::ExportPhase::Complete);

	engine::game::ProjectValidationReport openReport;
	{
		auto opened = engine::game::OpenProject(packagePath, {}, openReport);
		REQUIRE(opened.has_value());
		auto packagedStore = engine::assets::ChunkStore::Open(opened->Assets(), false);
		REQUIRE(packagedStore.has_value());
		engine::assets::SignatureBytes signature;
		const auto packagedManifest = packagedStore->ReadManifest(signature);
		REQUIRE(packagedManifest.has_value());
		CHECK(packagedManifest->Assets().size() == manifest.Assets().size());
		for (const engine::assets::AssetEntry &asset : packagedManifest->Assets()) {
			CHECK(packagedStore->ReadAsset(asset).has_value());
		}
		CHECK(
			std::filesystem::is_regular_file(
				opened->Entrypoint().parent_path() / "authoring/1-raw-source/model.blend"
			)
		);
	}

	std::error_code ignored;
	std::filesystem::remove_all(sourcePath, ignored);
	std::filesystem::remove_all(rawPath, ignored);
	server::Options hosting;
	hosting.GamePath = packagePath.string();
	hosting.MaximumTicks = 1;
	hosting.Unpaced = true;
	hosting.ContentGrantKey = std::string(64, '2');
	server::Server host;
	REQUIRE(host.Initialise(hosting));
	CHECK(host.Worlds().Find(engine::core::Name("Package World")).IsValid());
	host.Shutdown();
}

TEST_CASE(
	"universe export preserves public content and recursive discovery without adopting its path",
	"[studio][export]"
) {
	GroundingTree tree;
	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	engine::world::WorldSettings world;
	world.Name = engine::core::Name("Main");
	editor.Active = editor.Universe->Create(world);
	REQUIRE(editor.Active.IsValid());
	editor.GameName = engine::core::Name("Metadata");
	editor.GamePath = tree.Root / "original.agame";
	editor.UniverseFileSettings.RecursiveWorldDiscovery = true;
	editor.Content.PublisherKey = engine::assets::DevelopmentPublisher().ToHex();
	editor.Content.Sources.push_back(
		engine::delivery::Source{
			.Name = "public",
			.Kind = engine::delivery::SourceKind::Http,
			.Location = "127.0.0.1:9080",
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
		}
	);

	const std::filesystem::path exported = tree.Root / "copy.auniverse";
	editor.BeginUniverseExport(exported, false, false);
	REQUIRE(std::filesystem::is_regular_file(exported));
	CHECK(editor.GamePath == tree.Root / "original.agame");

	engine::world::Universe loaded;
	engine::game::GameInfo info;
	std::string error;
	REQUIRE(engine::game::LoadGame(loaded, exported, info, error));
	CHECK(info.RecursiveWorldDiscovery);
	CHECK(info.HttpEnabled);
	CHECK(info.PublisherKey == engine::assets::DevelopmentPublisher().ToHex());
	REQUIRE(info.Cdns.size() == 1);
	CHECK(info.Cdns.front().Name == "public");
	CHECK(info.Cdns.front().Location == "127.0.0.1:9080");
}
