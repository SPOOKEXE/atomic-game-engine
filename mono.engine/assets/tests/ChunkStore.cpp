#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Chunker.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.assets.chunkstore")
TEST_DEPENDS("engine.assets.manifest")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.assets.signature")

using engine::assets::AssetEntry;
using engine::assets::AssetKind;
using engine::assets::BundleEntry;
using engine::assets::ChunkEntry;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;

namespace {
	namespace fs = std::filesystem;

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	struct Tree {
		fs::path Root;

		Tree() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-chunkstore-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		ChunkStore Store(bool create = true) {
			auto store = ChunkStore::Open(Root, create);
			REQUIRE(store.has_value());
			return std::move(*store);
		}
	};

	// Writes one asset's chunks into a store and returns its manifest entry.
	ContentHash Publish(
		ChunkStore &store,
		Manifest &manifest,
		std::string name,
		AssetKind kind,
		const std::vector<std::string> &pieces
	) {
		std::vector<ChunkEntry> chunks;
		for (const std::string &piece : pieces) {
			const std::vector<std::byte> bytes = Bytes(piece);
			const ContentHash hash = Hasher::Of(bytes);
			REQUIRE(store.Write(hash, bytes));
			chunks.push_back(ChunkEntry{.Hash = hash, .Bytes = static_cast<uint32_t>(bytes.size())});
		}
		return manifest.AddAsset(std::move(name), kind, std::move(chunks));
	}
}

TEST_CASE("a chunk round-trips through the store", "[assets][chunkstore]") {
	Tree tree;
	ChunkStore store = tree.Store();

	const std::vector<std::byte> bytes = Bytes("some cooked mesh data");
	const ContentHash hash = Hasher::Of(bytes);

	CHECK_FALSE(store.Contains(hash));
	REQUIRE(store.Write(hash, bytes));
	CHECK(store.Contains(hash));

	const auto read = store.Read(hash);
	REQUIRE(read.has_value());
	CHECK(*read == bytes);
}

TEST_CASE("a chunk whose bytes are not its name is refused", "[assets][chunkstore]") {
	// A caller bug rather than an IO failure: a store that accepted this would
	// be a store whose filenames mean nothing.
	Tree tree;
	ChunkStore store = tree.Store();

	const ContentHash wrong = Hasher::Of(Bytes("something else"));
	CHECK_FALSE(store.Write(wrong, Bytes("these bytes")));
	CHECK_FALSE(store.Contains(wrong));
}

TEST_CASE("writing a chunk twice is a no-op that succeeds", "[assets][chunkstore]") {
	// Two assets sharing a chunk is the point of content addressing, so a
	// publisher should not have to check first.
	Tree tree;
	ChunkStore store = tree.Store();

	const std::vector<std::byte> bytes = Bytes("shared");
	const ContentHash hash = Hasher::Of(bytes);
	REQUIRE(store.Write(hash, bytes));
	REQUIRE(store.Write(hash, bytes));
	CHECK(store.Count() == 1);
}

TEST_CASE("a corrupted chunk fails to read rather than being handed over", "[assets][chunkstore]") {
	// The name *is* the hash, so this catches a corrupt disk, a partial write
	// and a tampered store with one check - and says which chunk, where the
	// asset root would only say that the asset was wrong.
	Tree tree;
	ChunkStore store = tree.Store();

	const std::vector<std::byte> bytes = Bytes("original content");
	const ContentHash hash = Hasher::Of(bytes);
	REQUIRE(store.Write(hash, bytes));

	const std::string hex = hash.ToHex();
	const fs::path path = tree.Root / "chunks" / hex.substr(0, 2) / hex;
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << "tampered content";
	}

	CHECK_FALSE(store.Read(hash).has_value());
	// Still "present" - Contains asks about a file and Read asks about bytes.
	CHECK(store.Contains(hash));
}

TEST_CASE("an asset is reassembled from its chunks", "[assets][chunkstore]") {
	Tree tree;
	ChunkStore store = tree.Store();
	Manifest manifest;

	const ContentHash root =
		Publish(store, manifest, "meshes/rock.mesh", AssetKind::Mesh, {"first-", "second-", "third"});

	const AssetEntry *const asset = manifest.FindByRoot(root);
	REQUIRE(asset != nullptr);

	const auto whole = store.ReadAsset(*asset);
	REQUIRE(whole.has_value());
	CHECK(*whole == Bytes("first-second-third"));
}

TEST_CASE("a complete asset is split into its signed chunk layout", "[assets][chunkstore]") {
	Tree sourceTree;
	ChunkStore source = sourceTree.Store();
	Manifest manifest;
	const ContentHash root =
		Publish(source, manifest, "grounded.mesh", AssetKind::Mesh, {"first-", "second"});
	const AssetEntry *const asset = manifest.FindByRoot(root);
	REQUIRE(asset != nullptr);

	Tree destinationTree;
	ChunkStore destination = destinationTree.Store();
	REQUIRE(destination.WriteAsset(*asset, Bytes("first-second")));
	CHECK(destination.Count() == 2);
	CHECK(destination.ReadAsset(*asset) == std::optional(Bytes("first-second")));

	CHECK_FALSE(destination.WriteAsset(*asset, Bytes("wrong bytes!")));
	AssetEntry wrongShape = *asset;
	wrongShape.TotalBytes += 1;
	CHECK_FALSE(destination.WriteAsset(wrongShape, Bytes("first-second!")));
}

TEST_CASE("an asset with a missing chunk does not half-read", "[assets][chunkstore]") {
	Tree tree;
	ChunkStore store = tree.Store();
	Manifest manifest;

	const ContentHash root = Publish(store, manifest, "a", AssetKind::Data, {"one", "two"});
	const AssetEntry *const asset = manifest.FindByRoot(root);
	REQUIRE(asset != nullptr);

	// Remove the second chunk.
	const std::string hex = asset->Chunks[1].Hash.ToHex();
	std::error_code failure;
	fs::remove(tree.Root / "chunks" / hex.substr(0, 2) / hex, failure);

	// Nothing rather than the first chunk alone: a partial asset is the shape a
	// caller uses by accident.
	CHECK_FALSE(store.ReadAsset(*asset).has_value());
}

TEST_CASE("a bundle payload is its members concatenated in member order", "[assets][chunkstore]") {
	// **The group layout**, and the one implementation of it. The origin
	// compresses what this produces and the client splits what it decompresses,
	// so if these two disagree every fetch returns the wrong bytes.
	Tree tree;
	ChunkStore store = tree.Store();
	Manifest manifest;

	const ContentHash a = Publish(store, manifest, "a", AssetKind::Data, {"AAAA"});
	const ContentHash b = Publish(store, manifest, "b", AssetKind::Data, {"BBBBBB"});
	const auto bundle = manifest.AddBundle(std::vector<ContentHash>{a, b});
	REQUIRE(bundle.has_value());

	const BundleEntry &entry = manifest.Bundles()[0];
	const auto payload = store.ReadBundle(manifest, entry);
	REQUIRE(payload.has_value());
	CHECK(payload->size() == entry.TotalBytes);

	// And SliceOf cuts it back up exactly.
	for (const ContentHash &member : entry.Assets) {
		const auto slice = manifest.SliceOf(entry, member);
		REQUIRE(slice.has_value());

		const AssetEntry *const asset = manifest.FindByRoot(member);
		REQUIRE(asset != nullptr);
		CHECK(slice->Bytes == asset->TotalBytes);

		const std::vector<std::byte> cut(
			payload->begin() + static_cast<ptrdiff_t>(slice->Offset),
			payload->begin() + static_cast<ptrdiff_t>(slice->Offset + slice->Bytes)
		);
		const auto direct = store.ReadAsset(*asset);
		REQUIRE(direct.has_value());
		CHECK(cut == *direct);
	}
}

TEST_CASE("the manifest is published with its signature in front", "[assets][chunkstore]") {
	// Signature first, so a reader knows what to verify before it has parsed
	// anything - the arrangement Grant::Open exists to enforce at its own layer.
	Tree tree;
	ChunkStore store = tree.Store();
	Manifest manifest;

	const ContentHash a = Publish(store, manifest, "a.mesh", AssetKind::Mesh, {"content"});
	REQUIRE(manifest.AddBundle(std::vector<ContentHash>{a}).has_value());

	std::array<std::byte, 32> seed{};
	for (size_t index = 0; index < seed.size(); ++index) {
		seed[index] = static_cast<std::byte>(index + 1);
	}
	const auto key = SigningKey::FromSeed(seed);
	REQUIRE(key.has_value());
	const SignatureBytes signature = key->SignManifestRoot(manifest.Root());

	REQUIRE(store.WriteManifest(manifest, signature));

	SignatureBytes read;
	const auto parsed = store.ReadManifest(read);
	REQUIRE(parsed.has_value());
	CHECK(read == signature);
	CHECK(parsed->Root() == manifest.Root());
	CHECK(engine::assets::VerifyManifestRoot(parsed->Root(), read, key->Public()));
	REQUIRE(parsed->Find("a.mesh") != nullptr);
	CHECK(parsed->Find("a.mesh")->Kind == AssetKind::Mesh);
}

TEST_CASE("a dictionary round-trips", "[assets][chunkstore]") {
	Tree tree;
	ChunkStore store = tree.Store();

	CHECK_FALSE(store.ReadDictionary().has_value());

	const std::vector<std::byte> dictionary = Bytes("not really a dictionary, but bytes");
	REQUIRE(store.WriteDictionary(dictionary));

	const auto read = store.ReadDictionary();
	REQUIRE(read.has_value());
	CHECK(*read == dictionary);
}

TEST_CASE("opening a store that is not there fails rather than creating one", "[assets][chunkstore]") {
	// A reader pointed at the wrong path learns that, instead of getting an
	// empty store that then reports every asset as missing.
	Tree tree;
	CHECK_FALSE(ChunkStore::Open(tree.Root / "nowhere", false).has_value());
	CHECK_FALSE(ChunkStore::Open({}, true).has_value());

	// A publisher asks for it to be made.
	CHECK(ChunkStore::Open(tree.Root / "made", true).has_value());
}

TEST_CASE("a chunk that is not what it is named refuses the whole asset", "[assets][chunkstore]") {
	// **The content half of the check, and where it now happens.** `ReadAsset`
	// used to hash every chunk on the way in and then hash the concatenation
	// again; it hashes once now, in `Read`, so this is the case that says the
	// remaining pass is still doing the work. A chunk file rewritten under its
	// own name is a corrupt disk, a partial write or a tampered store.
	Tree tree;
	ChunkStore store = tree.Store();
	Manifest manifest;

	const ContentHash root = Publish(store, manifest, "a", AssetKind::Data, {"one", "two"});
	const AssetEntry *const asset = manifest.FindByRoot(root);
	REQUIRE(asset != nullptr);
	REQUIRE(store.ReadAsset(*asset).has_value());

	const std::string hex = asset->Chunks[1].Hash.ToHex();
	{
		std::ofstream file(tree.Root / "chunks" / hex.substr(0, 2) / hex, std::ios::binary | std::ios::trunc);
		file << "TWO";
	}

	CHECK_FALSE(store.ReadAsset(*asset).has_value());
}

TEST_CASE("an entry whose chunks do not root to it refuses", "[assets][chunkstore]") {
	// **The shape half.** Every chunk on the disk is exactly what it is named,
	// so the per-chunk pass is satisfied and the asset is still not the asset -
	// which is the case a reader that trusted its chunk reads alone would let
	// through. It is `VerifyAssetShape` that catches it.
	Tree tree;
	ChunkStore store = tree.Store();
	Manifest manifest;

	const ContentHash root = Publish(store, manifest, "a", AssetKind::Data, {"one", "two"});
	const AssetEntry *const asset = manifest.FindByRoot(root);
	REQUIRE(asset != nullptr);

	AssetEntry claimed = *asset;
	claimed.Root = Hasher::Of(Bytes("something else"));
	CHECK_FALSE(store.ReadAsset(claimed).has_value());

	// Same chunks, same root, a total that disagrees with them.
	AssetEntry miscounted = *asset;
	miscounted.TotalBytes += 1;
	CHECK_FALSE(store.ReadAsset(miscounted).has_value());

	// And the chunks in the wrong order, which each verify and together are a
	// different asset.
	AssetEntry reordered = *asset;
	std::swap(reordered.Chunks[0], reordered.Chunks[1]);
	CHECK_FALSE(store.ReadAsset(reordered).has_value());
}
