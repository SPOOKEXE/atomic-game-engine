// Project packages are deployment boundaries, so this suite checks both the
// useful round trip and the archive shapes the reader must refuse.

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Project.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.game.project")
TEST_DEPENDS("engine.game.roundtrip")

namespace {
	namespace fs = std::filesystem;

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}

	struct ScratchTree {
		fs::path Root;

		explicit ScratchTree(std::string_view name) {
			static uint64_t serial = 0;
			Root = fs::temp_directory_path() /
				   ("atomic-project-test-" + std::string(name) + "-" + std::to_string(++serial));
			std::error_code ignored;
			fs::remove_all(Root, ignored);
			fs::create_directories(Root);
		}

		~ScratchTree() {
			std::error_code ignored;
			fs::remove_all(Root, ignored);
		}
	};

	engine::assets::SigningKey TestKey() {
		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index + 17);
		}
		auto key = engine::assets::SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	struct PreparedProject {
		fs::path Staging;
		std::string Publisher;
	};

	PreparedProject PrepareProject(const fs::path &root) {
		const fs::path staging = root / "staging";
		fs::create_directories(staging / "authoring");
		{
			std::ofstream(staging / "authoring/a.txt", std::ios::binary) << "alpha";
			std::ofstream(staging / "authoring/b.txt", std::ios::binary) << "bravo";
			std::ofstream(staging / "authoring/p", std::ios::binary) << "project duplicate fixture";
		}

		engine::world::Universe universe;
		engine::world::WorldSettings settings;
		settings.Name = engine::core::Name("Main");
		REQUIRE(universe.Create(settings).IsValid());
		engine::world::WorldSettings remote;
		remote.Name = engine::core::Name("Remote");
		REQUIRE(universe.CreateRemote(remote, engine::core::Name("host")).IsValid());

		engine::game::UniverseFileOptions universeOptions;
		universeOptions.HttpEnabled = true;
		universeOptions.RecursiveWorldDiscovery = true;
		universeOptions.Cdns.push_back({"public", "cdn.example.test:443"});

		auto key = TestKey();
		universeOptions.PublisherKey = key.Public().ToHex();
		std::string error;
		REQUIRE(
			engine::game::SaveUniverse(
				universe,
				engine::core::Name("Package"),
				{},
				staging / "game.auniverse",
				universeOptions,
				error
			)
		);

		auto store = engine::assets::ChunkStore::Open(staging / "assets", true);
		REQUIRE(store.has_value());
		engine::assets::Manifest manifest;
		const std::vector<std::byte> content = Bytes("processed mesh bytes");
		const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(content);
		REQUIRE(store->Write(chunk, content));
		const engine::assets::ContentHash asset = manifest.AddAsset(
			"meshes/cube.mesh",
			engine::assets::AssetKind::Mesh,
			{{.Hash = chunk, .Bytes = static_cast<uint32_t>(content.size())}}
		);
		const std::array roots{asset};
		REQUIRE(manifest.AddBundle(roots).has_value());
		REQUIRE(store->WriteManifest(manifest, key.SignManifestRoot(manifest.Root())));
		return {staging, universeOptions.PublisherKey};
	}

	engine::game::ProjectPackageOptions PackageOptions(const PreparedProject &project) {
		engine::game::ProjectPackageOptions options;
		options.PublisherKey = project.Publisher;
		options.Cdns.push_back({"public", "cdn.example.test:443"});
		return options;
	}

	std::vector<std::byte> ReadFile(const fs::path &path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		REQUIRE(file.good());
		const auto size = file.tellg();
		REQUIRE(size >= 0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		file.seekg(0);
		file.read(reinterpret_cast<char *>(bytes.data()), size);
		REQUIRE(file.good());
		return bytes;
	}

	void WriteFile(const fs::path &path, std::span<const std::byte> bytes) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		REQUIRE(file.good());
	}

	uint16_t Read16(std::span<const std::byte> bytes, size_t offset) {
		return static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[offset])) |
			   (static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[offset + 1])) << 8u);
	}

	uint32_t Read32(std::span<const std::byte> bytes, size_t offset) {
		return static_cast<uint32_t>(Read16(bytes, offset)) |
			   (static_cast<uint32_t>(Read16(bytes, offset + 2)) << 16u);
	}

	void Write16(std::span<std::byte> bytes, size_t offset, uint16_t value) {
		bytes[offset] = static_cast<std::byte>(value & 0xffu);
		bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xffu);
	}

	void Write32(std::span<std::byte> bytes, size_t offset, uint32_t value) {
		Write16(bytes, offset, static_cast<uint16_t>(value));
		Write16(bytes, offset + 2, static_cast<uint16_t>(value >> 16u));
	}

	bool PatchEntryName(std::vector<std::byte> &bytes, std::string_view from, std::string_view to) {
		REQUIRE(from.size() == to.size());
		bool patched = false;
		for (size_t offset = 0; offset + 46 <= bytes.size(); offset++) {
			const uint32_t signature = Read32(bytes, offset);
			size_t nameOffset = 0;
			uint16_t nameBytes = 0;
			if (signature == 0x04034b50u && offset + 30 <= bytes.size()) {
				nameBytes = Read16(bytes, offset + 26);
				nameOffset = offset + 30;
			} else if (signature == 0x02014b50u) {
				nameBytes = Read16(bytes, offset + 28);
				nameOffset = offset + 46;
			} else {
				continue;
			}
			if (nameOffset + nameBytes > bytes.size() || nameBytes != from.size()) {
				continue;
			}
			const std::string_view name(reinterpret_cast<const char *>(bytes.data() + nameOffset), nameBytes);
			if (name == from) {
				std::memcpy(bytes.data() + nameOffset, to.data(), to.size());
				patched = true;
			}
		}
		return patched;
	}

	bool PatchCentralEntry(
		std::vector<std::byte> &bytes,
		std::string_view name,
		const std::function<void(std::span<std::byte>, size_t)> &patch
	) {
		for (size_t offset = 0; offset + 46 <= bytes.size(); offset++) {
			if (Read32(bytes, offset) != 0x02014b50u) {
				continue;
			}
			const uint16_t nameBytes = Read16(bytes, offset + 28);
			if (offset + 46 + nameBytes > bytes.size()) {
				continue;
			}
			const std::string_view found(
				reinterpret_cast<const char *>(bytes.data() + offset + 46), nameBytes
			);
			if (found == name) {
				patch(bytes, offset);
				return true;
			}
		}
		return false;
	}

	bool PatchEntryHeaders(
		std::vector<std::byte> &bytes,
		std::string_view name,
		const std::function<void(std::span<std::byte>, size_t, bool)> &patch
	) {
		bool local = false;
		bool central = false;
		for (size_t offset = 0; offset + 46 <= bytes.size(); offset++) {
			const uint32_t signature = Read32(bytes, offset);
			size_t nameOffset = 0;
			uint16_t nameBytes = 0;
			bool isCentral = false;
			if (signature == 0x04034b50u && offset + 30 <= bytes.size()) {
				nameBytes = Read16(bytes, offset + 26);
				nameOffset = offset + 30;
			} else if (signature == 0x02014b50u) {
				nameBytes = Read16(bytes, offset + 28);
				nameOffset = offset + 46;
				isCentral = true;
			} else {
				continue;
			}
			if (nameOffset + nameBytes > bytes.size()) {
				continue;
			}
			const std::string_view found(
				reinterpret_cast<const char *>(bytes.data() + nameOffset), nameBytes
			);
			if (found == name) {
				patch(bytes, offset, isCentral);
				isCentral ? central = true : local = true;
			}
		}
		return local && central;
	}

	bool HasFinding(const engine::game::ProjectValidationReport &report, std::string_view code) {
		return std::any_of(report.Findings.begin(), report.Findings.end(), [code](const auto &finding) {
			return finding.Code == code;
		});
	}

	fs::path WriteValidPackage(ScratchTree &tree, PreparedProject &project) {
		const fs::path destination = tree.Root / "project.zip";
		engine::game::ProjectPackageInfo info;
		engine::game::ProjectValidationReport report;
		REQUIRE(
			engine::game::WriteProjectPackage(
				project.Staging, destination, PackageOptions(project), info, report
			)
		);
		CHECK(report.Passed());
		return destination;
	}
}

TEST_CASE("project paths have one shared classification", "[game][project]") {
	using engine::game::ProjectKind;
	CHECK(engine::game::ClassifyProject("scene.lua") == ProjectKind::SceneScript);
	CHECK(engine::game::ClassifyProject("game.agame") == ProjectKind::GameFile);
	CHECK(engine::game::ClassifyProject("game.auniverse") == ProjectKind::UniverseFolder);
	CHECK(engine::game::ClassifyProject("game.zip") == ProjectKind::ProjectZip);
	CHECK(engine::game::ClassifyProject("game.ZIP") == ProjectKind::SceneScript);
	CHECK(std::string(engine::game::ExtensionOf(engine::game::ExportProduct::WorldFile)) == ".aworld");
	CHECK(
		std::string(engine::game::ExtensionOf(engine::game::ExportProduct::UniverseFolder)) == ".auniverse"
	);
	CHECK(std::string(engine::game::ExtensionOf(engine::game::ExportProduct::ProjectZip)) == ".zip");
}

TEST_CASE("project ZIP is deterministic and owns extraction lifetime", "[game][project]") {
	ScratchTree tree("roundtrip");
	PreparedProject project = PrepareProject(tree.Root);
	auto options = PackageOptions(project);
	engine::game::ProjectPackageInfo firstInfo;
	engine::game::ProjectPackageInfo secondInfo;
	engine::game::ProjectValidationReport report;
	const fs::path first = tree.Root / "first.zip";
	const fs::path second = tree.Root / "second.zip";
	REQUIRE(engine::game::WriteProjectPackage(project.Staging, first, options, firstInfo, report));
	REQUIRE(engine::game::WriteProjectPackage(project.Staging, second, options, secondInfo, report));
	CHECK(ReadFile(first) == ReadFile(second));
	CHECK(firstInfo.ContentDigest == secondInfo.ContentDigest);

	fs::path extractedRoot;
	{
		auto opened = engine::game::OpenProject(first, {}, report);
		REQUIRE(opened.has_value());
		CHECK(opened->Temporary());
		CHECK(fs::is_regular_file(opened->Entrypoint()));
		CHECK(fs::is_regular_file(opened->Assets() / engine::assets::ChunkStore::MANIFEST_FILE));
		extractedRoot = opened->Entrypoint().parent_path();

		engine::world::Universe loaded;
		engine::game::GameInfo info;
		std::string error;
		REQUIRE(engine::game::LoadGame(loaded, opened->Entrypoint(), info, error));
		CHECK(info.RecursiveWorldDiscovery);
		CHECK(info.PublisherKey == project.Publisher);
		REQUIRE(info.Cdns.size() == 1);
		CHECK(info.Cdns.front().Name == "public");
	}
	CHECK_FALSE(fs::exists(extractedRoot));
}

TEST_CASE("project writer requires a complete trusted processed store", "[game][project]") {
	ScratchTree tree("content");
	PreparedProject project = PrepareProject(tree.Root);
	fs::remove(project.Staging / "assets" / engine::assets::ChunkStore::MANIFEST_FILE);

	engine::game::ProjectPackageInfo info;
	engine::game::ProjectValidationReport report;
	const fs::path destination = tree.Root / "missing.zip";
	CHECK_FALSE(
		engine::game::WriteProjectPackage(project.Staging, destination, PackageOptions(project), info, report)
	);
	CHECK(HasFinding(report, "content.manifest.missing"));
	CHECK_FALSE(fs::exists(destination));
}

TEST_CASE(
	"project writer rejects invalid signatures unbundled assets and missing chunks", "[game][project]"
) {
	ScratchTree tree("content-integrity");
	engine::game::ProjectPackageInfo info;
	engine::game::ProjectValidationReport report;

	PreparedProject badSignature = PrepareProject(tree.Root / "signature");
	const fs::path manifestPath = badSignature.Staging / "assets" / engine::assets::ChunkStore::MANIFEST_FILE;
	std::vector<std::byte> manifestBytes = ReadFile(manifestPath);
	REQUIRE_FALSE(manifestBytes.empty());
	manifestBytes.front() ^= std::byte{1};
	WriteFile(manifestPath, manifestBytes);
	CHECK_FALSE(
		engine::game::WriteProjectPackage(
			badSignature.Staging, tree.Root / "signature.zip", PackageOptions(badSignature), info, report
		)
	);
	CHECK(HasFinding(report, "content.manifest.signature"));

	PreparedProject unbundled = PrepareProject(tree.Root / "unbundled");
	auto store = engine::assets::ChunkStore::Open(unbundled.Staging / "assets", false);
	REQUIRE(store.has_value());
	const std::vector<std::byte> content = Bytes("processed mesh bytes");
	const engine::assets::ContentHash chunk = engine::assets::Hasher::Of(content);
	engine::assets::Manifest manifest;
	manifest.AddAsset(
		"meshes/cube.mesh",
		engine::assets::AssetKind::Mesh,
		{{.Hash = chunk, .Bytes = static_cast<uint32_t>(content.size())}}
	);
	auto key = TestKey();
	REQUIRE(store->WriteManifest(manifest, key.SignManifestRoot(manifest.Root())));
	CHECK_FALSE(
		engine::game::WriteProjectPackage(
			unbundled.Staging, tree.Root / "unbundled.zip", PackageOptions(unbundled), info, report
		)
	);
	CHECK(HasFinding(report, "content.asset.unbundled"));

	PreparedProject missingChunk = PrepareProject(tree.Root / "missing-chunk");
	std::error_code failure;
	fs::remove_all(missingChunk.Staging / "assets/chunks", failure);
	REQUIRE_FALSE(failure);
	CHECK_FALSE(
		engine::game::WriteProjectPackage(
			missingChunk.Staging, tree.Root / "missing-chunk.zip", PackageOptions(missingChunk), info, report
		)
	);
	CHECK(HasFinding(report, "content.asset.missing"));
}

TEST_CASE(
	"project writer rejects a missing authored world and sensitive authoring files", "[game][project]"
) {
	ScratchTree tree("staging-policy");
	PreparedProject project = PrepareProject(tree.Root);
	const fs::path world = project.Staging / "game.worlds/1-Main.aworld";
	REQUIRE(fs::is_regular_file(world));
	fs::remove(world);

	engine::game::ProjectPackageInfo info;
	engine::game::ProjectValidationReport report;
	CHECK_FALSE(
		engine::game::WriteProjectPackage(
			project.Staging, tree.Root / "missing-world.zip", PackageOptions(project), info, report
		)
	);
	CHECK(HasFinding(report, "package.universe.invalid"));

	project = PrepareProject(tree.Root / "second");
	std::ofstream(project.Staging / "authoring/.env") << "PRIVATE_TOKEN=value";
	CHECK_FALSE(
		engine::game::WriteProjectPackage(
			project.Staging, tree.Root / "secret.zip", PackageOptions(project), info, report
		)
	);
	CHECK(HasFinding(report, "package.authoring.secret"));

	project = PrepareProject(tree.Root / "third");
	std::error_code linkFailure;
	fs::create_symlink(
		project.Staging / "authoring/a.txt", project.Staging / "authoring/linked.txt", linkFailure
	);
	if (!linkFailure) {
		CHECK_FALSE(
			engine::game::WriteProjectPackage(
				project.Staging, tree.Root / "linked-staging.zip", PackageOptions(project), info, report
			)
		);
		CHECK(HasFinding(report, "package.staging.special"));
	}
}

TEST_CASE("project reader requires exactly one project entrypoint", "[game][project]") {
	ScratchTree tree("entrypoint");
	PreparedProject project = PrepareProject(tree.Root);
	const fs::path valid = WriteValidPackage(tree, project);
	const std::vector<std::byte> original = ReadFile(valid);

	std::vector<std::byte> missing = original;
	REQUIRE(PatchEntryName(missing, "project.xml", "project.xmo"));
	const fs::path missingPath = tree.Root / "missing-project.zip";
	WriteFile(missingPath, missing);
	engine::game::ProjectValidationReport report;
	CHECK_FALSE(engine::game::OpenProject(missingPath, {}, report).has_value());
	CHECK(HasFinding(report, "package.project.missing"));

	std::vector<std::byte> duplicate = original;
	REQUIRE(PatchEntryName(duplicate, "authoring/p", "project.xml"));
	const fs::path duplicatePath = tree.Root / "duplicate-project.zip";
	WriteFile(duplicatePath, duplicate);
	CHECK_FALSE(engine::game::OpenProject(duplicatePath, {}, report).has_value());
	CHECK(HasFinding(report, "archive.path.duplicate"));
}

TEST_CASE("project reader refuses hostile paths and ambiguous archives", "[game][project]") {
	ScratchTree tree("hostile");
	PreparedProject project = PrepareProject(tree.Root);
	const fs::path valid = WriteValidPackage(tree, project);
	const std::vector<std::byte> original = ReadFile(valid);

	auto refuseRenamed = [&](std::string_view from, std::string_view to, std::string_view code) {
		std::vector<std::byte> hostile = original;
		REQUIRE(PatchEntryName(hostile, from, to));
		const fs::path path = tree.Root / (std::string(code) + ".zip");
		WriteFile(path, hostile);
		engine::game::ProjectValidationReport report;
		CHECK_FALSE(engine::game::OpenProject(path, {}, report).has_value());
		std::string codes;
		for (const auto &finding : report.Findings) {
			codes += finding.Code + " ";
		}
		INFO("findings: " << codes);
		CHECK(HasFinding(report, code));
	};

	refuseRenamed("authoring/b.txt", "/uthoring/b.txt", "archive.path.invalid");
	refuseRenamed("authoring/b.txt", "authoring/a.txt", "archive.path.duplicate");
	refuseRenamed("authoring/b.txt", "Authoring/a.txt", "archive.path.case-collision");
	refuseRenamed("game.auniverse", "../x.auniverse", "archive.path.invalid");

	std::vector<std::byte> trailing = original;
	trailing.push_back(std::byte{0});
	const fs::path trailingPath = tree.Root / "trailing.zip";
	WriteFile(trailingPath, trailing);
	engine::game::ProjectValidationReport report;
	CHECK_FALSE(engine::game::OpenProject(trailingPath, {}, report).has_value());
	CHECK(HasFinding(report, "archive.trailing"));
}

TEST_CASE("project reader enforces archive resource limits", "[game][project]") {
	ScratchTree tree("limits");
	PreparedProject project = PrepareProject(tree.Root);
	const fs::path path = WriteValidPackage(tree, project);

	auto refuses = [&](engine::game::ProjectPackageLimits limits, std::string_view code) {
		engine::game::ProjectValidationReport report;
		CHECK_FALSE(engine::game::OpenProject(path, limits, report).has_value());
		CHECK(HasFinding(report, code));
	};

	engine::game::ProjectPackageLimits entries;
	entries.MaximumEntries = 1;
	refuses(entries, "archive.entry.count");
	engine::game::ProjectPackageLimits paths;
	paths.MaximumPathBytes = 4;
	refuses(paths, "archive.path.length");
	engine::game::ProjectPackageLimits nesting;
	nesting.MaximumNesting = 1;
	refuses(nesting, "archive.path.invalid");
	engine::game::ProjectPackageLimits fileBytes;
	fileBytes.MaximumFileBytes = 1;
	refuses(fileBytes, "archive.entry.size");
	engine::game::ProjectPackageLimits totalBytes;
	totalBytes.MaximumTotalBytes = 1;
	refuses(totalBytes, "archive.entry.size");
	engine::game::ProjectPackageLimits ratio;
	ratio.MaximumCompressionRatio = 0;
	refuses(ratio, "archive.entry.ratio");
}

TEST_CASE("project reader refuses encrypted unsupported and linked entries", "[game][project]") {
	ScratchTree tree("headers");
	PreparedProject project = PrepareProject(tree.Root);
	const fs::path valid = WriteValidPackage(tree, project);
	const std::vector<std::byte> original = ReadFile(valid);

	auto refusePatched = [&](std::string_view label, auto patch, std::string_view code) {
		std::vector<std::byte> hostile = original;
		REQUIRE(PatchEntryHeaders(hostile, "authoring/a.txt", patch));
		const fs::path path = tree.Root / (std::string(label) + ".zip");
		WriteFile(path, hostile);
		engine::game::ProjectValidationReport report;
		CHECK_FALSE(engine::game::OpenProject(path, {}, report).has_value());
		std::string codes;
		for (const auto &finding : report.Findings) {
			codes += finding.Code + " ";
		}
		INFO("findings: " << codes);
		CHECK(HasFinding(report, code));
	};

	refusePatched(
		"encrypted",
		[](std::span<std::byte> bytes, size_t offset, bool central) {
			const size_t flags = central ? offset + 8 : offset + 6;
			Write16(bytes, flags, static_cast<uint16_t>(Read16(bytes, flags) | 1u));
		},
		"archive.entry.encrypted"
	);
	refusePatched(
		"unsupported",
		[](std::span<std::byte> bytes, size_t offset, bool central) {
			Write16(bytes, central ? offset + 10 : offset + 8, 99);
		},
		"archive.entry.unsupported"
	);

	std::vector<std::byte> linked = original;
	REQUIRE(PatchCentralEntry(linked, "authoring/a.txt", [](std::span<std::byte> bytes, size_t offset) {
		Write16(bytes, offset + 4, static_cast<uint16_t>((3u << 8u) | 20u));
		Write32(bytes, offset + 38, static_cast<uint32_t>(0120777u << 16u));
	}));
	const fs::path linkedPath = tree.Root / "linked.zip";
	WriteFile(linkedPath, linked);
	engine::game::ProjectValidationReport linkedReport;
	CHECK_FALSE(engine::game::OpenProject(linkedPath, {}, linkedReport).has_value());
	CHECK(HasFinding(linkedReport, "archive.entry.special"));
}

TEST_CASE("failed package publication preserves existing output", "[game][project]") {
	ScratchTree tree("publish");
	PreparedProject project = PrepareProject(tree.Root);
	const fs::path destination = tree.Root / "project.zip";
	std::ofstream(destination, std::ios::binary) << "existing";

	engine::game::ProjectPackageInfo info;
	engine::game::ProjectValidationReport report;
	CHECK_FALSE(
		engine::game::WriteProjectPackage(project.Staging, destination, PackageOptions(project), info, report)
	);
	CHECK(HasFinding(report, "package.destination.exists"));
	const auto bytes = ReadFile(destination);
	CHECK(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()) == "existing");
}
