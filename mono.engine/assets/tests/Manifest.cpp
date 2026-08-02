#include <engine/assets/HashTree.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.assets.manifest")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.assets.hashtree")
TEST_DEPENDS("engine.core.bytes")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::assets::AssetEntry;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::HashTree;
using engine::assets::Manifest;
using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	ChunkEntry Chunk(std::string_view text) {
		ChunkEntry entry;
		entry.Hash = Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
		entry.Bytes = static_cast<uint32_t>(text.size());
		return entry;
	}

	// A manifest with three assets in two bundles. Built in a deliberately
	// unsorted order, because the canonical arrangement is the class's job and
	// not the caller's.
	Manifest Sample() {
		Manifest manifest;
		const ContentHash rock =
			manifest.AddAsset("meshes/rock.mesh", {Chunk("rock-one"), Chunk("rock-two")});
		const ContentHash grass = manifest.AddAsset("textures/grass.tex", {Chunk("grass")});
		const ContentHash bark = manifest.AddAsset("audio/bark.wav", {Chunk("bark")});

		REQUIRE(manifest.AddBundle(std::vector<ContentHash>{rock, grass}).has_value());
		REQUIRE(manifest.AddBundle(std::vector<ContentHash>{bark}).has_value());
		return manifest;
	}

	std::vector<std::byte> Serialise(const Manifest &manifest) {
		ByteWriter writer;
		manifest.Write(writer);
		const auto bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}
}

TEST_CASE("an asset's root is the tree over its chunks", "[assets][manifest]") {
	Manifest manifest;
	const std::vector<ChunkEntry> chunks{Chunk("one"), Chunk("two"), Chunk("three")};
	const ContentHash root = manifest.AddAsset("a", chunks);

	std::vector<ContentHash> hashes;
	for (const ChunkEntry &chunk : chunks) {
		hashes.push_back(chunk.Hash);
	}

	CHECK(root == HashTree::RootOf(hashes));

	const AssetEntry *entry = manifest.Find("a");
	REQUIRE(entry != nullptr);
	CHECK(entry->Root == root);
	CHECK(entry->TotalBytes == 3 + 3 + 5);
	CHECK(entry->Chunks.size() == 3);
}

TEST_CASE("a name resolves to exactly one asset", "[assets][manifest]") {
	const Manifest manifest = Sample();

	REQUIRE(manifest.Find("meshes/rock.mesh") != nullptr);
	REQUIRE(manifest.Find("audio/bark.wav") != nullptr);
	CHECK(manifest.Find("nothing/here") == nullptr);
	CHECK(manifest.Find("") == nullptr);

	// The manifest is where a name becomes a hash, and the last place a name is
	// used at all — CDN.md §1.
	CHECK(manifest.Find("meshes/rock.mesh")->Name == "meshes/rock.mesh");
}

TEST_CASE("adding a name twice replaces rather than duplicates", "[assets][manifest]") {
	Manifest manifest;
	manifest.AddAsset("a", {Chunk("first")});
	const ContentHash second = manifest.AddAsset("a", {Chunk("second")});

	// Publishing twice from one build must not leave two rows for one name, or
	// Find stops being total and which row wins becomes an accident of order.
	REQUIRE(manifest.Assets().size() == 1);
	CHECK(manifest.Find("a")->Root == second);
}

TEST_CASE("assets are held in canonical name order", "[assets][manifest]") {
	const Manifest manifest = Sample();

	const auto &assets = manifest.Assets();
	REQUIRE(assets.size() == 3);
	CHECK(std::is_sorted(assets.begin(), assets.end(), [](const AssetEntry &left, const AssetEntry &right) {
		return left.Name < right.Name;
	}));
	CHECK(assets[0].Name == "audio/bark.wav");
}

TEST_CASE("a bundle is a set, so member order does not change it", "[assets][manifest]") {
	Manifest manifest;
	const ContentHash a = manifest.AddAsset("a", {Chunk("a")});
	const ContentHash b = manifest.AddAsset("b", {Chunk("b")});
	const ContentHash c = manifest.AddAsset("c", {Chunk("c")});

	const auto forward = manifest.AddBundle(std::vector<ContentHash>{a, b, c});
	const auto backward = manifest.AddBundle(std::vector<ContentHash>{c, b, a});

	REQUIRE(forward.has_value());
	REQUIRE(backward.has_value());
	CHECK(*forward == *backward);

	// And adding the same set twice is one bundle, not two.
	CHECK(manifest.Bundles().size() == 1);
}

TEST_CASE("a bundle naming an unknown asset is refused", "[assets][manifest]") {
	Manifest manifest;
	const ContentHash known = manifest.AddAsset("a", {Chunk("a")});

	// Unfetchable content in a bundle is a problem discovered at delivery time,
	// which is far too late.
	const ContentHash unknown = Hasher::Of(std::span<const std::byte>{});
	CHECK_FALSE(manifest.AddBundle(std::vector<ContentHash>{known, unknown}).has_value());
	CHECK_FALSE(manifest.AddBundle({}).has_value());
	CHECK(manifest.Bundles().empty());
}

TEST_CASE("a bundle totals its assets' bytes", "[assets][manifest]") {
	Manifest manifest;
	const ContentHash a = manifest.AddAsset("a", {Chunk("12345")});
	const ContentHash b = manifest.AddAsset("b", {Chunk("123"), Chunk("12")});

	REQUIRE(manifest.AddBundle(std::vector<ContentHash>{a, b}).has_value());
	REQUIRE(manifest.Bundles().size() == 1);
	CHECK(manifest.Bundles()[0].TotalBytes == 10);
}

TEST_CASE("the manifest root changes when anything below it changes", "[assets][manifest]") {
	const Manifest original = Sample();
	const ContentHash root = original.Root();

	// One edited chunk changes its asset root, its bundle root and this. That
	// chain is the invalidation set an edge cache is handed — CDN.md §2.
	Manifest edited;
	const ContentHash rock = edited.AddAsset("meshes/rock.mesh", {Chunk("rock-one"), Chunk("rock-CHANGED")});
	const ContentHash grass = edited.AddAsset("textures/grass.tex", {Chunk("grass")});
	const ContentHash bark = edited.AddAsset("audio/bark.wav", {Chunk("bark")});
	REQUIRE(edited.AddBundle(std::vector<ContentHash>{rock, grass}).has_value());
	REQUIRE(edited.AddBundle(std::vector<ContentHash>{bark}).has_value());

	CHECK(edited.Root() != root);
}

TEST_CASE("the manifest is byte-stable", "[assets][manifest]") {
	// Two builds of the same content, added in different orders. They must
	// produce identical bytes, or the manifest cannot be diffed or cached and
	// "did the content change?" stops having a cheap answer.
	Manifest forward;
	const ContentHash a1 = forward.AddAsset("a", {Chunk("a")});
	const ContentHash b1 = forward.AddAsset("b", {Chunk("b")});
	REQUIRE(forward.AddBundle(std::vector<ContentHash>{a1, b1}).has_value());

	Manifest backward;
	const ContentHash b2 = backward.AddAsset("b", {Chunk("b")});
	const ContentHash a2 = backward.AddAsset("a", {Chunk("a")});
	REQUIRE(backward.AddBundle(std::vector<ContentHash>{b2, a2}).has_value());

	CHECK(Serialise(forward) == Serialise(backward));
	CHECK(forward.Root() == backward.Root());
}

TEST_CASE("a manifest round-trips through bytes", "[assets][manifest]") {
	const Manifest original = Sample();
	const auto bytes = Serialise(original);

	ByteReader reader(bytes);
	const auto parsed = Manifest::Read(reader);
	REQUIRE(parsed.has_value());

	CHECK(parsed->Root() == original.Root());
	REQUIRE(parsed->Assets().size() == original.Assets().size());
	REQUIRE(parsed->Bundles().size() == original.Bundles().size());

	for (size_t index = 0; index < original.Assets().size(); ++index) {
		const AssetEntry &left = original.Assets()[index];
		const AssetEntry &right = parsed->Assets()[index];
		CHECK(left.Name == right.Name);
		CHECK(left.Root == right.Root);
		CHECK(left.TotalBytes == right.TotalBytes);
		REQUIRE(left.Chunks.size() == right.Chunks.size());
		for (size_t chunk = 0; chunk < left.Chunks.size(); ++chunk) {
			CHECK(left.Chunks[chunk].Hash == right.Chunks[chunk].Hash);
			CHECK(left.Chunks[chunk].Bytes == right.Chunks[chunk].Bytes);
		}
	}

	// And re-serialising gives the same bytes, which is what makes the format
	// a fixed point rather than merely reversible.
	CHECK(Serialise(*parsed) == bytes);
}

TEST_CASE("a wrong magic or version is refused", "[assets][manifest]") {
	auto bytes = Serialise(Sample());

	auto corrupted = bytes;
	corrupted[0] = static_cast<std::byte>(0xFF);
	ByteReader wrongMagic(corrupted);
	CHECK_FALSE(Manifest::Read(wrongMagic).has_value());

	// Refused rather than guessed at. A reader carrying on into a version it
	// does not know is a reader mis-parsing hostile bytes.
	corrupted = bytes;
	corrupted[4] = static_cast<std::byte>(0x99);
	ByteReader wrongVersion(corrupted);
	CHECK_FALSE(Manifest::Read(wrongVersion).has_value());
}

TEST_CASE("a truncated manifest is refused", "[assets][manifest]") {
	const auto bytes = Serialise(Sample());

	// Every length, because a reader answers zero past the end rather than
	// throwing — so a truncation that is not checked parses as a manifest of
	// empty things instead of failing.
	for (size_t length = 0; length < bytes.size(); ++length) {
		INFO("truncated to " << length);
		ByteReader reader(std::span<const std::byte>(bytes).first(length));
		CHECK_FALSE(Manifest::Read(reader).has_value());
	}
}

TEST_CASE("a manifest whose root does not match its chunks is refused", "[assets][manifest]") {
	Manifest manifest;
	manifest.AddAsset("a", {Chunk("one"), Chunk("two")});
	const ContentHash a = manifest.Assets()[0].Root;
	REQUIRE(manifest.AddBundle(std::vector<ContentHash>{a}).has_value());

	auto bytes = Serialise(manifest);

	// Flip a byte inside the first chunk hash. The written asset root then
	// describes content the chunk list does not, which is exactly the lie the
	// hash tree exists to make impossible — so the reader recomputes rather
	// than believing what it was handed.
	const size_t firstChunkHash = bytes.size() - ContentHash::BYTES;
	bytes[firstChunkHash] = static_cast<std::byte>(static_cast<uint8_t>(bytes[firstChunkHash]) ^ 0x01);

	ByteReader reader(bytes);
	CHECK_FALSE(Manifest::Read(reader).has_value());
}

TEST_CASE("a refusal marks the reader failed", "[assets][manifest]") {
	auto bytes = Serialise(Sample());
	bytes[0] = static_cast<std::byte>(0xFF);

	ByteReader reader(bytes);
	CHECK_FALSE(Manifest::Read(reader).has_value());

	// One flag carries the verdict, so a caller reading further from the same
	// buffer cannot miss it.
	CHECK(reader.Failed());
}

TEST_CASE("an empty manifest round-trips and has a root", "[assets][manifest]") {
	const Manifest empty;

	CHECK(empty.Assets().empty());
	CHECK(empty.Bundles().empty());
	CHECK_FALSE(empty.Root().IsZero());

	const auto bytes = Serialise(empty);
	ByteReader reader(bytes);
	const auto parsed = Manifest::Read(reader);
	REQUIRE(parsed.has_value());
	CHECK(parsed->Root() == empty.Root());

	// And it is not the same root as a manifest with content in it.
	CHECK(empty.Root() != Sample().Root());
}

TEST_CASE("a manifest root signs and verifies", "[assets][manifest][signature]") {
	using engine::assets::SigningKey;
	using engine::assets::VerifyManifestRoot;

	std::array<std::byte, SigningKey::SEED_BYTES> seed{};
	for (size_t index = 0; index < seed.size(); ++index) {
		seed[index] = static_cast<std::byte>(index + 3);
	}
	auto key = SigningKey::FromSeed(seed);
	REQUIRE(key.has_value());

	const Manifest manifest = Sample();
	const auto signature = key->SignManifestRoot(manifest.Root());

	// The whole chain in one case: content to chunks to assets to bundles to a
	// root, and one signature over the root. Everything below is bound by hash.
	CHECK(VerifyManifestRoot(manifest.Root(), signature, key->Public()));

	Manifest different;
	const ContentHash only = different.AddAsset("a", {Chunk("a")});
	REQUIRE(different.AddBundle(std::vector<ContentHash>{only}).has_value());
	CHECK_FALSE(VerifyManifestRoot(different.Root(), signature, key->Public()));
}

TEST_CASE(
	"reading and writing report themselves to the frame graph and the metrics sink",
	"[assets][manifest][framegraph]"
) {
	const Manifest manifest = Sample();
	const auto bytes = Serialise(manifest);
	auto broken = bytes;
	broken[0] = static_cast<std::byte>(0xFF);

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	ByteWriter writer;
	manifest.Write(writer);
	ByteReader good(bytes);
	CHECK(Manifest::Read(good).has_value());
	ByteReader bad(broken);
	CHECK_FALSE(Manifest::Read(bad).has_value());
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	CHECK(named("Manifest::Write"));
	CHECK(named("Manifest::Read"));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	CHECK(total("assets.manifest.parsed") == 1.0);
	CHECK(total("assets.manifest.refused") == 1.0);
}
