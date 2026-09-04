#include <engine/assets/HashTree.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
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
using engine::assets::AssetKind;
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
	// The same text as bytes, for the rows that verify content rather than
	// describe it.
	std::vector<std::byte> Body(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}

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
			manifest.AddAsset("meshes/rock.mesh", AssetKind::Mesh, {Chunk("rock-one"), Chunk("rock-two")});
		const ContentHash grass =
			manifest.AddAsset("textures/grass.tex", AssetKind::Texture, {Chunk("grass")});
		const ContentHash bark = manifest.AddAsset("audio/bark.wav", AssetKind::Audio, {Chunk("bark")});

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
	const ContentHash root = manifest.AddAsset("a", AssetKind::Data, chunks);

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
	// used at all.
	CHECK(manifest.Find("meshes/rock.mesh")->Name == "meshes/rock.mesh");
}

TEST_CASE("adding a name twice replaces rather than duplicates", "[assets][manifest]") {
	Manifest manifest;
	manifest.AddAsset("a", AssetKind::Data, {Chunk("first")});
	const ContentHash second = manifest.AddAsset("a", AssetKind::Data, {Chunk("second")});

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
	const ContentHash a = manifest.AddAsset("a", AssetKind::Data, {Chunk("a")});
	const ContentHash b = manifest.AddAsset("b", AssetKind::Data, {Chunk("b")});
	const ContentHash c = manifest.AddAsset("c", AssetKind::Data, {Chunk("c")});

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
	const ContentHash known = manifest.AddAsset("a", AssetKind::Data, {Chunk("a")});

	// Unfetchable content in a bundle is a problem discovered at delivery time,
	// which is far too late.
	const ContentHash unknown = Hasher::Of(std::span<const std::byte>{});
	CHECK_FALSE(manifest.AddBundle(std::vector<ContentHash>{known, unknown}).has_value());
	CHECK_FALSE(manifest.AddBundle({}).has_value());
	CHECK(manifest.Bundles().empty());
}

TEST_CASE("a bundle totals its assets' bytes", "[assets][manifest]") {
	Manifest manifest;
	const ContentHash a = manifest.AddAsset("a", AssetKind::Data, {Chunk("12345")});
	const ContentHash b = manifest.AddAsset("b", AssetKind::Data, {Chunk("123"), Chunk("12")});

	REQUIRE(manifest.AddBundle(std::vector<ContentHash>{a, b}).has_value());
	REQUIRE(manifest.Bundles().size() == 1);
	CHECK(manifest.Bundles()[0].TotalBytes == 10);
}

TEST_CASE("the manifest root changes when anything below it changes", "[assets][manifest]") {
	const Manifest original = Sample();
	const ContentHash root = original.Root();

	// One edited chunk changes its asset root, its bundle root and this. That
	// chain is the invalidation set an edge cache is handed.
	Manifest edited;
	const ContentHash rock =
		edited.AddAsset("meshes/rock.mesh", AssetKind::Mesh, {Chunk("rock-one"), Chunk("rock-CHANGED")});
	const ContentHash grass = edited.AddAsset("textures/grass.tex", AssetKind::Texture, {Chunk("grass")});
	const ContentHash bark = edited.AddAsset("audio/bark.wav", AssetKind::Audio, {Chunk("bark")});
	REQUIRE(edited.AddBundle(std::vector<ContentHash>{rock, grass}).has_value());
	REQUIRE(edited.AddBundle(std::vector<ContentHash>{bark}).has_value());

	CHECK(edited.Root() != root);
}

TEST_CASE("the root binds a name to its content, not just the content", "[assets][manifest]") {
	// **The gap this closes.** Before v0.9 the root covered bundle roots alone,
	// so an origin serving a manifest with two names swapped handed a client
	// content that verified perfectly against the signed root and was the wrong
	// asset. Signing the content and not the index is signing the half nobody
	// looks anything up by.
	Manifest straight;
	const ContentHash a1 = straight.AddAsset("meshes/rock.mesh", AssetKind::Mesh, {Chunk("rock")});
	const ContentHash b1 = straight.AddAsset("meshes/tree.mesh", AssetKind::Mesh, {Chunk("tree")});
	REQUIRE(straight.AddBundle(std::vector<ContentHash>{a1, b1}).has_value());

	Manifest swapped;
	const ContentHash a2 = swapped.AddAsset("meshes/rock.mesh", AssetKind::Mesh, {Chunk("tree")});
	const ContentHash b2 = swapped.AddAsset("meshes/tree.mesh", AssetKind::Mesh, {Chunk("rock")});
	REQUIRE(swapped.AddBundle(std::vector<ContentHash>{a2, b2}).has_value());

	// Identical content, identical bundles - and a different root, because the
	// names now point at each other's bytes.
	CHECK(straight.BundleRoot() == swapped.BundleRoot());
	CHECK(straight.DescriptorRoot() != swapped.DescriptorRoot());
	CHECK(straight.Root() != swapped.Root());
}

TEST_CASE("the root binds an asset's kind", "[assets][manifest]") {
	// A client routes on the kind, so an origin that could relabel a script as
	// a texture without breaking the signature would be deciding what a blob is
	// for.
	Manifest asMesh;
	const ContentHash mesh = asMesh.AddAsset("thing", AssetKind::Mesh, {Chunk("bytes")});
	REQUIRE(asMesh.AddBundle(std::vector<ContentHash>{mesh}).has_value());

	Manifest asAudio;
	const ContentHash audio = asAudio.AddAsset("thing", AssetKind::Audio, {Chunk("bytes")});
	REQUIRE(asAudio.AddBundle(std::vector<ContentHash>{audio}).has_value());

	CHECK(asMesh.BundleRoot() == asAudio.BundleRoot());
	CHECK(asMesh.Root() != asAudio.Root());
}

TEST_CASE("a name and a kind cannot be re-cut to produce one descriptor", "[assets][manifest]") {
	// The descriptor is length-prefixed, so `ab` + `c` and `a` + `bc` are not
	// the same hash. Without the length they would be, and the binding above
	// would not hold for names that happen to line up.
	Manifest first;
	const ContentHash left = first.AddAsset("ab", AssetKind::Mesh, {Chunk("x")});
	REQUIRE(first.AddBundle(std::vector<ContentHash>{left}).has_value());

	Manifest second;
	const ContentHash right = second.AddAsset("a", AssetKind::Mesh, {Chunk("x")});
	REQUIRE(second.AddBundle(std::vector<ContentHash>{right}).has_value());

	CHECK(first.DescriptorRoot() != second.DescriptorRoot());
}

TEST_CASE("the asset root still addresses content alone", "[assets][manifest]") {
	// The name is bound a level up, deliberately: folding it into the asset
	// root would give two identical files under two names two different roots
	// and lose dedup across them.
	Manifest manifest;
	const ContentHash here = manifest.AddAsset("one/thing.mesh", AssetKind::Mesh, {Chunk("identical")});
	const ContentHash there = manifest.AddAsset("other/thing.mesh", AssetKind::Mesh, {Chunk("identical")});
	CHECK(here == there);
}

TEST_CASE("an asset's kind survives a round trip", "[assets][manifest]") {
	Manifest manifest;
	manifest.AddAsset("a.mesh", AssetKind::Mesh, {Chunk("a")});
	manifest.AddAsset("b.png", AssetKind::Texture, {Chunk("b")});
	manifest.AddAsset("c.wav", AssetKind::Audio, {Chunk("c")});
	manifest.AddAsset("d.spv", AssetKind::Shader, {Chunk("d")});
	manifest.AddAsset("e.aanim", AssetKind::Animation, {Chunk("e")});

	// The bytes are held in a named local: a ByteReader is a view, so reading
	// from a temporary would be reading freed memory.
	const auto bytes = Serialise(manifest);
	ByteReader reader(bytes);
	const auto parsed = Manifest::Read(reader);
	REQUIRE(parsed.has_value());

	REQUIRE(parsed->Find("a.mesh") != nullptr);
	CHECK(parsed->Find("a.mesh")->Kind == AssetKind::Mesh);
	CHECK(parsed->Find("b.png")->Kind == AssetKind::Texture);
	CHECK(parsed->Find("c.wav")->Kind == AssetKind::Audio);
	CHECK(parsed->Find("d.spv")->Kind == AssetKind::Shader);
	CHECK(parsed->Find("e.aanim")->Kind == AssetKind::Animation);
}

TEST_CASE("assets can be selected by kind", "[assets][manifest]") {
	// What "connect and get assets of types" resolves to at the format layer.
	Manifest manifest;
	manifest.AddAsset("a.mesh", AssetKind::Mesh, {Chunk("a")});
	manifest.AddAsset("b.png", AssetKind::Texture, {Chunk("b")});
	manifest.AddAsset("c.png", AssetKind::Texture, {Chunk("c")});

	const auto textures = manifest.OfKind(AssetKind::Texture);
	REQUIRE(textures.size() == 2);
	// Name order, which is the order the manifest holds them in.
	CHECK(textures[0]->Name == "b.png");
	CHECK(textures[1]->Name == "c.png");

	CHECK(manifest.OfKind(AssetKind::Mesh).size() == 1);
	CHECK(manifest.OfKind(AssetKind::Audio).empty());
}

TEST_CASE("an asset resolves to the bundle that carries it", "[assets][manifest]") {
	// The lookup a fetch needs: a client wants one asset and the unit that
	// travels is the group it is in.
	Manifest manifest;
	const ContentHash rock = manifest.AddAsset("rock.mesh", AssetKind::Mesh, {Chunk("rock")});
	const ContentHash bark = manifest.AddAsset("bark.wav", AssetKind::Audio, {Chunk("bark")});
	const auto first = manifest.AddBundle(std::vector<ContentHash>{rock});
	REQUIRE(first.has_value());

	REQUIRE(manifest.BundleFor(rock) != nullptr);
	CHECK(manifest.BundleFor(rock)->Root == *first);

	// An asset in no bundle is unfetchable, and being able to detect that is
	// worth more than a nullptr nobody checks.
	CHECK(manifest.BundleFor(bark) == nullptr);
}

TEST_CASE("the manifest is byte-stable", "[assets][manifest]") {
	// Two builds of the same content, added in different orders. They must
	// produce identical bytes, or the manifest cannot be diffed or cached and
	// "did the content change?" stops having a cheap answer.
	Manifest forward;
	const ContentHash a1 = forward.AddAsset("a", AssetKind::Data, {Chunk("a")});
	const ContentHash b1 = forward.AddAsset("b", AssetKind::Data, {Chunk("b")});
	REQUIRE(forward.AddBundle(std::vector<ContentHash>{a1, b1}).has_value());

	Manifest backward;
	const ContentHash b2 = backward.AddAsset("b", AssetKind::Data, {Chunk("b")});
	const ContentHash a2 = backward.AddAsset("a", AssetKind::Data, {Chunk("a")});
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
	// throwing - so a truncation that is not checked parses as a manifest of
	// empty things instead of failing.
	for (size_t length = 0; length < bytes.size(); ++length) {
		INFO("truncated to " << length);
		ByteReader reader(std::span<const std::byte>(bytes).first(length));
		CHECK_FALSE(Manifest::Read(reader).has_value());
	}
}

TEST_CASE("short manifest rows are refused before their counts reserve", "[assets][manifest]") {
	// These are each within the parser's explicit numeric ceiling. Their only
	// contradiction is that the buffer cannot contain even the shortest rows,
	// so accepting the count long enough to reserve turns a tiny hostile file
	// into a large allocation.
	constexpr uint32_t COUNT = 1'000'000;

	SECTION("assets") {
		ByteWriter writer;
		writer.WriteUInt32(Manifest::MAGIC);
		writer.WriteUInt16(Manifest::VERSION);
		writer.WriteUInt32(COUNT);

		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Manifest::Read(reader).has_value());
		CHECK(reader.Failed());
	}

	SECTION("chunks") {
		ByteWriter writer;
		writer.WriteUInt32(Manifest::MAGIC);
		writer.WriteUInt16(Manifest::VERSION);
		writer.WriteUInt32(1);
		writer.WriteString("a");
		writer.WriteUInt8(static_cast<uint8_t>(AssetKind::Data));
		writer.WriteRaw(ContentHash{}.Digest.data(), ContentHash::BYTES);
		writer.WriteUInt64(0);
		writer.WriteUInt32(COUNT);

		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Manifest::Read(reader).has_value());
		CHECK(reader.Failed());
	}

	SECTION("bundles") {
		ByteWriter writer;
		writer.WriteUInt32(Manifest::MAGIC);
		writer.WriteUInt16(Manifest::VERSION);
		writer.WriteUInt32(0);
		writer.WriteUInt32(COUNT);

		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Manifest::Read(reader).has_value());
		CHECK(reader.Failed());
	}

	SECTION("bundle members") {
		Manifest source;
		const ContentHash empty = source.AddAsset("a", AssetKind::Data, {});

		ByteWriter writer;
		writer.WriteUInt32(Manifest::MAGIC);
		writer.WriteUInt16(Manifest::VERSION);
		writer.WriteUInt32(1);
		writer.WriteString("a");
		writer.WriteUInt8(static_cast<uint8_t>(AssetKind::Data));
		writer.WriteRaw(empty.Digest.data(), ContentHash::BYTES);
		writer.WriteUInt64(0);
		writer.WriteUInt32(0);
		writer.WriteUInt32(1);
		writer.WriteRaw(ContentHash{}.Digest.data(), ContentHash::BYTES);
		writer.WriteUInt64(0);
		writer.WriteUInt32(COUNT);
		writer.WriteRaw(ContentHash{}.Digest.data(), ContentHash::BYTES);

		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Manifest::Read(reader).has_value());
		CHECK(reader.Failed());
	}
}

TEST_CASE("a manifest whose root does not match its chunks is refused", "[assets][manifest]") {
	Manifest manifest;
	manifest.AddAsset("a", AssetKind::Data, {Chunk("one"), Chunk("two")});
	const ContentHash a = manifest.Assets()[0].Root;
	REQUIRE(manifest.AddBundle(std::vector<ContentHash>{a}).has_value());

	auto bytes = Serialise(manifest);

	// Flip a byte inside the first chunk hash. The written asset root then
	// describes content the chunk list does not, which is exactly the lie the
	// hash tree exists to make impossible - so the reader recomputes rather
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
	const ContentHash only = different.AddAsset("a", AssetKind::Data, {Chunk("a")});
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

TEST_CASE("the root index answers what a scan over the assets would", "[assets][manifest]") {
	// **`FindByRoot` was a scan until v0.19 and is a binary search now.** The
	// index is derived from `AssetsByName` and nothing else, so the check that
	// matters is not that it finds something but that it finds *the same thing*
	// the scan would have, for every asset and for a root that is not there.
	Manifest manifest;
	std::vector<ContentHash> roots;
	for (int index = 0; index < 64; ++index) {
		// A scrambled order, because a sorted one would append every time and
		// never exercise the shift an insert in the middle causes.
		const int scattered = (index * 37) % 64;
		roots.push_back(manifest.AddAsset(
			"asset-" + std::to_string(scattered), AssetKind::Data, {Chunk(std::to_string(scattered))}
		));
	}

	for (const ContentHash &root : roots) {
		const AssetEntry *const found = manifest.FindByRoot(root);
		const auto scanned = std::find_if(
			manifest.Assets().begin(), manifest.Assets().end(), [&root](const AssetEntry &asset) {
				return asset.Root == root;
			}
		);
		REQUIRE(scanned != manifest.Assets().end());
		REQUIRE(found != nullptr);
		CHECK(found == &*scanned);
	}

	CHECK(manifest.FindByRoot(Hasher::Of(std::span<const std::byte>())) == nullptr);
}

TEST_CASE("two names for one blob resolve to the first by name", "[assets][manifest]") {
	// Identical content under two names is one root - that is what dedup means
	// - so the index holds two positions for one key. The scan this replaced
	// returned the earlier of them, and the tiebreak in `RootOrder` is there so
	// that it still does.
	Manifest manifest;
	const ContentHash second = manifest.AddAsset("z-later.bin", AssetKind::Data, {Chunk("identical")});
	const ContentHash first = manifest.AddAsset("a-earlier.bin", AssetKind::Data, {Chunk("identical")});
	REQUIRE(first == second);

	const AssetEntry *const found = manifest.FindByRoot(first);
	REQUIRE(found != nullptr);
	CHECK(found->Name == "a-earlier.bin");
}

TEST_CASE("replacing a name moves its root in the index", "[assets][manifest]") {
	// `AddAsset` replaces rather than duplicates, and the replacement usually
	// has a different root. The old one has to stop resolving, or a manifest
	// would answer for content it no longer describes.
	Manifest manifest;
	manifest.AddAsset("a", AssetKind::Data, {Chunk("before")});
	manifest.AddAsset("b", AssetKind::Data, {Chunk("other")});
	const ContentHash before = manifest.Assets()[0].Root;

	const ContentHash after = manifest.AddAsset("a", AssetKind::Data, {Chunk("after")});
	REQUIRE(before != after);

	CHECK(manifest.FindByRoot(before) == nullptr);
	REQUIRE(manifest.FindByRoot(after) != nullptr);
	CHECK(manifest.FindByRoot(after)->Name == "a");
	// And the asset the replacement shifted past is still findable.
	REQUIRE(manifest.FindByRoot(manifest.Assets()[1].Root) != nullptr);
	CHECK(manifest.FindByRoot(manifest.Assets()[1].Root)->Name == "b");
}

TEST_CASE("a parsed manifest resolves by root as well as a built one", "[assets][manifest]") {
	// The parse path fills the asset list in one go rather than through
	// `AddAsset`, so it builds the index separately - and its bundle loop
	// resolves every member against it while still parsing.
	Manifest manifest;
	std::vector<ContentHash> roots;
	for (int index = 0; index < 32; ++index) {
		roots.push_back(manifest.AddAsset(
			"asset-" + std::to_string(index), AssetKind::Data, {Chunk(std::to_string(index))}
		));
	}
	REQUIRE(manifest.AddBundle(roots).has_value());

	const auto bytes = Serialise(manifest);
	ByteReader reader(bytes);
	const auto parsed = Manifest::Read(reader);
	REQUIRE(parsed.has_value());

	for (const ContentHash &root : roots) {
		const AssetEntry *const found = parsed->FindByRoot(root);
		REQUIRE(found != nullptr);
		CHECK(found->Root == root);
	}
	CHECK(parsed->FindByRoot(Hasher::Of(std::span<const std::byte>())) == nullptr);
}

TEST_CASE("a copied manifest carries a working index", "[assets][manifest]") {
	// The index holds positions rather than pointers, which is what makes a
	// copy of a manifest a copy of a working index instead of one pointing into
	// the original's storage.
	const Manifest original = Sample();
	const Manifest copied = original;
	for (const AssetEntry &asset : original.Assets()) {
		REQUIRE(copied.FindByRoot(asset.Root) != nullptr);
		CHECK(copied.FindByRoot(asset.Root)->Name == asset.Name);
	}
}

TEST_CASE("VerifyAssetShape is the half of VerifyAsset that needs no bytes", "[assets][manifest]") {
	// **The two halves have to stay two halves.** `ChunkStore::ReadAsset` calls
	// the shape half alone because its reads already made the content half, so
	// a change that folded a content check into `VerifyAssetShape` would break
	// it and a change that dropped the tree check from it would let a run of
	// good chunks be passed off as a different asset.
	Manifest manifest;
	manifest.AddAsset("a", AssetKind::Data, {Chunk("one"), Chunk("two")});
	const AssetEntry &asset = manifest.Assets()[0];

	CHECK(engine::assets::VerifyAssetShape(asset));
	CHECK(engine::assets::VerifyAsset(asset, Body("onetwo")));

	// A root that does not cover the chunk list. Refused by the shape half
	// alone, with no content anywhere near it.
	AssetEntry renamed = asset;
	renamed.Root = Hasher::Of(std::span<const std::byte>());
	CHECK_FALSE(engine::assets::VerifyAssetShape(renamed));
	CHECK_FALSE(engine::assets::VerifyAsset(renamed, Body("onetwo")));

	// A total that disagrees with the chunks, which is what would size a buffer
	// one way and fill it another.
	AssetEntry miscounted = asset;
	miscounted.TotalBytes += 1;
	CHECK_FALSE(engine::assets::VerifyAssetShape(miscounted));

	// And the content half is still the content half: the shape is untouched
	// and the bytes are wrong.
	CHECK(engine::assets::VerifyAssetShape(asset));
	CHECK_FALSE(engine::assets::VerifyAsset(asset, Body("onetwX")));
}
