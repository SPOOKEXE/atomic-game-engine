#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentForm.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/Publisher.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

TEST_SUITE_ID("cdn.publisher")
TEST_DEPENDS("engine.assets.chunkstore")
TEST_DEPENDS("engine.assets.manifest")
TEST_DEPENDS("cdn.grouper")
TEST_DEPENDS("engine.assets.contentpolicy")

using engine::assets::AssetKind;
using engine::assets::ChunkStore;
using engine::assets::Manifest;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;
using engine::assets::VerifyManifestRoot;

namespace {
	namespace fs = std::filesystem;

	SigningKey Key(uint8_t fill = 3) {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); ++index) {
			seed[index] = static_cast<std::byte>(fill + index);
		}
		auto key = SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// A content directory and a store beside it.
	struct Workspace {
		fs::path Root;

		Workspace() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-publish-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
			fs::create_directories(Content());
		}

		~Workspace() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		fs::path Content() const {
			return Root / "content";
		}

		fs::path Store() const {
			return Root / "store";
		}

		void Add(const std::string &name, std::string_view body) {
			const fs::path path = Content() / name;
			std::error_code failure;
			fs::create_directories(path.parent_path(), failure);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(body.data(), static_cast<std::streamsize>(body.size()));
		}

		// Something with enough internal repetition to chunk into more than one
		// piece, so the chunker is genuinely exercised.
		void AddLarge(const std::string &name, char fill, size_t bytes = 300 * 1024) {
			Add(name, std::string(bytes, fill));
		}
	};
}

TEST_CASE("a directory of files becomes a signed store", "[cdn][publisher]") {
	Workspace workspace;
	workspace.Add("meshes/rock.mesh", "MESHv1 rock geometry");
	workspace.Add("textures/grass.png", "PNG grass pixels");
	workspace.Add("audio/bark.wav", "RIFF bark samples");

	const SigningKey key = Key();
	const auto report = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(report.has_value());
	CHECK(report->Assets == 3);
	CHECK(report->Bundles >= 1);
	CHECK(report->ContentBytes > 0);

	// And the store reads back as a manifest that verifies against the key.
	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());

	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());
	CHECK(manifest->Root() == report->Root);
	CHECK(VerifyManifestRoot(manifest->Root(), signature, key.Public()));
}

TEST_CASE("a name is relative and uses forward slashes", "[cdn][publisher]") {
	// A manifest keys on the name, so the same files published on Windows and
	// on Linux have to produce one manifest — otherwise the two builds share no
	// cache entries and nothing says why.
	Workspace workspace;
	workspace.Add("meshes/rock.mesh", "content");

	const SigningKey key = Key();
	REQUIRE(cdn::Publish(workspace.Content(), workspace.Store(), key).has_value());

	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());
	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());

	REQUIRE(manifest->Assets().size() == 1);
	CHECK(manifest->Assets()[0].Name == "meshes/rock.mesh");
}

TEST_CASE("the kind is decided once, at publish", "[cdn][publisher]") {
	// Nothing downstream re-derives it — that is the whole argument in
	// AssetKind.hpp for the kind being in the manifest at all.
	Workspace workspace;
	workspace.Add("meshes/rock.mesh", "a");
	workspace.Add("textures/grass.png", "b");
	workspace.Add("audio/bark.wav", "c");
	workspace.Add("scripts/Rings.luau", "d");
	workspace.Add("data/table.xyzzy", "e");

	const SigningKey key = Key();
	REQUIRE(cdn::Publish(workspace.Content(), workspace.Store(), key).has_value());

	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());
	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());

	REQUIRE(manifest->Find("meshes/rock.mesh") != nullptr);
	CHECK(manifest->Find("meshes/rock.mesh")->Kind == AssetKind::Mesh);
	CHECK(manifest->Find("textures/grass.png")->Kind == AssetKind::Texture);
	CHECK(manifest->Find("audio/bark.wav")->Kind == AssetKind::Audio);
	CHECK(manifest->Find("scripts/Rings.luau")->Kind == AssetKind::Script);
	// Unrecognised is Unknown rather than a guess. It still publishes.
	CHECK(manifest->Find("data/table.xyzzy")->Kind == AssetKind::Unknown);

	CHECK(manifest->OfKind(AssetKind::Mesh).size() == 1);
	CHECK(manifest->OfKind(AssetKind::Texture).size() == 1);
}

TEST_CASE("identical content is stored once", "[cdn][publisher]") {
	// The number that says whether content addressing earned its keep. Two
	// files with the same bytes are two assets sharing every chunk.
	Workspace workspace;
	workspace.AddLarge("meshes/one.mesh", 'x');
	workspace.AddLarge("meshes/copy.mesh", 'x');

	const SigningKey key = Key();
	const auto report = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(report.has_value());
	CHECK(report->Assets == 2);
	// Two assets' worth of content in one asset's worth of chunks.
	CHECK(report->StoredBytes < report->ContentBytes);
}

TEST_CASE("publishing is deterministic", "[cdn][publisher]") {
	// Two origins that publish the same content differently would prepare and
	// cache different bundles for it, and nothing anywhere would report that
	// they had stopped sharing.
	Workspace first;
	Workspace second;
	for (Workspace *workspace : {&first, &second}) {
		workspace->Add("meshes/rock.mesh", "MESHv1 rock");
		workspace->Add("textures/grass.png", "PNG grass");
		workspace->Add("audio/bark.wav", "RIFF bark");
	}

	const SigningKey key = Key();
	const auto one = cdn::Publish(first.Content(), first.Store(), key);
	const auto two = cdn::Publish(second.Content(), second.Store(), key);
	REQUIRE(one.has_value());
	REQUIRE(two.has_value());

	CHECK(one->Root == two->Root);
	CHECK(one->Chunks == two->Chunks);
	CHECK(one->Bundles == two->Bundles);
}

TEST_CASE("republishing unchanged content is a no-op on the store", "[cdn][publisher]") {
	// What makes republishing cheap: unchanged content is already there and
	// every write of it is a no-op.
	Workspace workspace;
	workspace.Add("meshes/rock.mesh", "MESHv1 rock");

	const SigningKey key = Key();
	const auto first = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(first.has_value());

	const auto second = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(second.has_value());
	CHECK(second->Chunks == first->Chunks);
	CHECK(second->Root == first->Root);
}

TEST_CASE("changing one file changes the root and reuses the rest", "[cdn][publisher]") {
	// The invalidation set is the chain of changed hashes — CDN.md §2 — so a
	// one-file edit must not rewrite the store.
	Workspace workspace;
	workspace.Add("meshes/rock.mesh", "MESHv1 rock");
	workspace.Add("textures/grass.png", "PNG grass");

	const SigningKey key = Key();
	const auto before = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(before.has_value());

	workspace.Add("meshes/rock.mesh", "MESHv1 rock, edited");
	const auto after = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(after.has_value());

	CHECK(after->Root != before->Root);
	// The old chunk is still there — nothing prunes — and the new one joined it.
	CHECK(after->Chunks > before->Chunks);
}

TEST_CASE("an empty file is skipped rather than published", "[cdn][publisher]") {
	// An empty file has no chunks, so it would be an asset with an empty root:
	// a row in the manifest that fetches nothing.
	Workspace workspace;
	workspace.Add("meshes/rock.mesh", "MESHv1 rock");
	workspace.Add("meshes/empty.mesh", "");

	const SigningKey key = Key();
	const auto report = cdn::Publish(workspace.Content(), workspace.Store(), key);
	REQUIRE(report.has_value());
	CHECK(report->Assets == 1);
}

TEST_CASE("publishing nothing is refused rather than producing an empty store", "[cdn][publisher]") {
	Workspace workspace;
	const SigningKey key = Key();
	CHECK_FALSE(cdn::Publish(workspace.Content(), workspace.Store(), key).has_value());
	// And a content directory that is not there at all.
	CHECK_FALSE(cdn::Publish(workspace.Root / "nowhere", workspace.Store(), key).has_value());
}

TEST_CASE("assets in one directory land in one group", "[cdn][publisher]") {
	// Affinity stands in for a decision the import pipeline will make properly:
	// somebody who put a mesh and its textures in one folder was saying they go
	// together, and a group that lands has to make something appear.
	Workspace workspace;
	cdn::PublishSettings settings;
	// A ceiling small enough that grouping has to make a choice.
	settings.Grouping.TargetBytes = 256 * 1024;
	settings.Grouping.MaximumBytes = 512 * 1024;

	workspace.AddLarge("props/rock/mesh.mesh", 'a', 200 * 1024);
	workspace.AddLarge("props/rock/albedo.png", 'b', 200 * 1024);
	workspace.AddLarge("props/tree/mesh.mesh", 'c', 200 * 1024);
	workspace.AddLarge("props/tree/albedo.png", 'd', 200 * 1024);

	const SigningKey key = Key();
	const auto report = cdn::Publish(workspace.Content(), workspace.Store(), key, settings);
	REQUIRE(report.has_value());

	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());
	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());

	// Each directory's pair stays together: whichever bundle carries a rock
	// file carries the other rock file too.
	const auto *const rockMesh = manifest->Find("props/rock/mesh.mesh");
	const auto *const rockTexture = manifest->Find("props/rock/albedo.png");
	REQUIRE(rockMesh != nullptr);
	REQUIRE(rockTexture != nullptr);

	const auto *const carrier = manifest->BundleFor(rockMesh->Root);
	REQUIRE(carrier != nullptr);
	CHECK(manifest->BundleFor(rockTexture->Root) == carrier);
}

TEST_CASE("every published asset is in exactly one bundle", "[cdn][publisher]") {
	// An asset in no bundle is unfetchable, and finding that out at delivery
	// time is finding out too late.
	Workspace workspace;
	for (int index = 0; index < 12; ++index) {
		workspace.Add("things/" + std::to_string(index) + ".mesh", "content " + std::to_string(index));
	}

	const SigningKey key = Key();
	REQUIRE(cdn::Publish(workspace.Content(), workspace.Store(), key).has_value());

	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());
	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());

	for (const auto &asset : manifest->Assets()) {
		INFO("asset " << asset.Name);
		CHECK(manifest->BundleFor(asset.Root) != nullptr);
	}
}

TEST_CASE("a published bundle reassembles from the store", "[cdn][publisher]") {
	// The publisher writes chunks and the origin reads them back as a group's
	// payload. If those two disagree, every fetch returns the wrong bytes.
	Workspace workspace;
	workspace.AddLarge("meshes/rock.mesh", 'r', 200 * 1024);
	workspace.AddLarge("textures/grass.png", 'g', 100 * 1024);

	const SigningKey key = Key();
	REQUIRE(cdn::Publish(workspace.Content(), workspace.Store(), key).has_value());

	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());
	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());

	for (const auto &bundle : manifest->Bundles()) {
		const auto payload = store->ReadBundle(*manifest, bundle);
		REQUIRE(payload.has_value());
		CHECK(payload->size() == bundle.TotalBytes);
	}
}

TEST_CASE("a refused form never becomes a chunk, a name or a manifest row", "[cdn][publisher][content]") {
	// **The gate is before the chunker, and that is what is being asserted.** A
	// filter applied after — at serve time, say — would be looking at a hash
	// that has already been stored, and `cdn/Publisher.hpp`'s first paragraph
	// says a hash cannot be walked back to a name. So the only honest place is
	// the one moment the name is real, and the evidence is that the store is
	// smaller rather than that the manifest is shorter.
	Workspace workspace;
	workspace.Add("textures/grass.png", "PNG grass pixels");
	workspace.AddLarge("art/logo.svg", 'v');
	workspace.AddLarge("clips/intro.mp4", 'm');

	cdn::PublishSettings settings;
	settings.Content.Allow(engine::assets::ContentForm::Svg, false);
	settings.Content.Allow(engine::assets::ContentForm::Mp4, false);

	const SigningKey key = Key();
	const auto report = cdn::Publish(workspace.Content(), workspace.Store(), key, settings);
	REQUIRE(report.has_value());
	CHECK(report->Assets == 1);
	CHECK(report->Refused == 2);

	auto store = ChunkStore::Open(workspace.Store(), false);
	REQUIRE(store.has_value());
	SignatureBytes signature;
	const auto manifest = store->ReadManifest(signature);
	REQUIRE(manifest.has_value());

	CHECK(manifest->Find("textures/grass.png") != nullptr);
	CHECK(manifest->Find("art/logo.svg") == nullptr);
	CHECK(manifest->Find("clips/intro.mp4") == nullptr);

	// Six hundred kilobytes of refused content, and the store holds a sentence.
	// A gate that ran after the chunker would pass every assertion above this
	// one and fail this.
	CHECK(store->Bytes() < 100u * 1024u);
}

TEST_CASE(
	"refusing the unnamed form closes a publish to what this build knows", "[cdn][publisher][content]"
) {
	Workspace workspace;
	workspace.Add("textures/grass.png", "PNG grass pixels");
	workspace.Add("notes/README", "no extension at all");
	workspace.Add("data/table.xyzzy", "an extension nothing has a row for");

	cdn::PublishSettings settings;
	settings.Content.Allow(engine::assets::ContentForm::Unknown, false);

	const SigningKey key = Key();
	const auto report = cdn::Publish(workspace.Content(), workspace.Store(), key, settings);
	REQUIRE(report.has_value());
	CHECK(report->Assets == 1);
	CHECK(report->Refused == 2);
}
