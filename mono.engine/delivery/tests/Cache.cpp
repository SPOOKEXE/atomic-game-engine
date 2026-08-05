#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/delivery/Cache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("engine.delivery.cache")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.assets.manifest")

using engine::assets::AssetEntry;
using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::delivery::ContentCache;

namespace {
	namespace fs = std::filesystem;

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	// A manifest entry over content cut into `pieces`.
	//
	// **Deliberately multi-chunk in most cases below.** An asset root is a tree
	// over chunk hashes, so a single-chunk asset is the one shape where a hash
	// of the whole happens to look right — and a suite built only out of those
	// would pass against a cache that verified the wrong thing.
	struct Content {
		Manifest Catalogue;
		std::vector<std::byte> Whole;

		Content(std::string name, const std::vector<std::string> &pieces) {
			std::vector<ChunkEntry> chunks;
			for (const std::string &piece : pieces) {
				const std::vector<std::byte> bytes = Bytes(piece);
				chunks.push_back(
					ChunkEntry{
						.Hash = Hasher::Of(bytes),
						.Bytes = static_cast<uint32_t>(bytes.size()),
					}
				);
				Whole.insert(Whole.end(), bytes.begin(), bytes.end());
			}
			Catalogue.AddAsset(std::move(name), AssetKind::Data, std::move(chunks));
		}

		const AssetEntry &Entry() const {
			return Catalogue.Assets()[0];
		}
	};

	struct Tree {
		fs::path Root;

		Tree() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-delivery-cache-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		ContentCache Open(uint64_t capacity = 1024 * 1024) {
			auto cache = ContentCache::Open(Root, capacity);
			REQUIRE(cache.has_value());
			return std::move(*cache);
		}
	};
}

TEST_CASE("content round-trips through the cache", "[delivery][cache]") {
	Tree tree;
	ContentCache cache = tree.Open();
	const Content content("a.mesh", {"first-", "second-", "third"});

	CHECK_FALSE(cache.Contains(content.Entry().Root));
	CHECK_FALSE(cache.Find(content.Entry()).has_value());

	REQUIRE(cache.Store(content.Entry(), content.Whole));
	CHECK(cache.Contains(content.Entry().Root));
	CHECK(cache.Count() == 1);

	const auto found = cache.Find(content.Entry());
	REQUIRE(found.has_value());
	CHECK(*found == content.Whole);
}

TEST_CASE("a multi-chunk asset caches, and that is the regression", "[delivery][cache]") {
	// **This is the bug the end-to-end run caught.** An asset root is a tree
	// over chunk hashes, so a cache that compared `Hasher::Of(bytes)` against
	// the root refused every asset cut into more than one chunk — silently, as
	// a cache miss, so every fetch went to the network for ever and nothing
	// said why. A single-chunk asset would have passed that broken check.
	Tree tree;
	ContentCache cache = tree.Open();

	const Content single("one.mesh", {"only-one-chunk"});
	const Content many("many.mesh", {"one", "two", "three", "four", "five"});

	REQUIRE(single.Entry().Chunks.size() == 1);
	REQUIRE(many.Entry().Chunks.size() == 5);
	// The whole asset's digest is not its root once there is more than one
	// chunk, which is exactly what the wrong check assumed.
	CHECK(Hasher::Of(many.Whole) != many.Entry().Root);

	REQUIRE(cache.Store(single.Entry(), single.Whole));
	REQUIRE(cache.Store(many.Entry(), many.Whole));
	CHECK(cache.Find(many.Entry()).has_value());
}

TEST_CASE("bytes that are not the asset are refused", "[delivery][cache]") {
	// A caller bug rather than a cache failure: everything reaching here should
	// already have been verified against the manifest.
	Tree tree;
	ContentCache cache = tree.Open();
	const Content content("a.mesh", {"one", "two"});

	CHECK_FALSE(cache.Store(content.Entry(), Bytes("something else entirely")));
	// The right length and the wrong bytes is the case a length check alone
	// would let through.
	CHECK_FALSE(cache.Store(content.Entry(), Bytes("XXXXXX")));
	CHECK_FALSE(cache.Contains(content.Entry().Root));
}

TEST_CASE("a corrupted entry is dropped rather than returned", "[delivery][cache]") {
	// **The reason reads verify at all.** A cache lives in a directory a user,
	// another program or a failing disk can write to, so trusting it is
	// trusting the least protected thing in the delivery path.
	Tree tree;
	ContentCache cache = tree.Open();
	const Content content("a.mesh", {"original", "content"});

	REQUIRE(cache.Store(content.Entry(), content.Whole));

	const std::string hex = content.Entry().Root.ToHex();
	{
		std::ofstream file(tree.Root / hex.substr(0, 2) / hex, std::ios::binary | std::ios::trunc);
		file << "tamperedcontent";
	}

	CHECK_FALSE(cache.Find(content.Entry()).has_value());
	// Deleted, not merely refused — leaving it would make every later run pay
	// the same failed read.
	CHECK_FALSE(cache.Contains(content.Entry().Root));
}

TEST_CASE("an entry larger than the whole capacity is refused", "[delivery][cache]") {
	// One asset that evicts everything else on every store is worse than not
	// caching that asset at all.
	Tree tree;
	ContentCache cache = tree.Open(16);
	const Content content("big.mesh", {std::string(64, 'x')});

	CHECK_FALSE(cache.Store(content.Entry(), content.Whole));
	CHECK(cache.Count() == 0);
}

TEST_CASE("the cache evicts to stay under its capacity", "[delivery][cache]") {
	Tree tree;
	ContentCache cache = tree.Open(256);

	for (int index = 0; index < 8; ++index) {
		const Content content("a" + std::to_string(index), {std::string(64, static_cast<char>('a' + index))});
		REQUIRE(cache.Store(content.Entry(), content.Whole));
	}

	CHECK(cache.Count() <= 4);
	CHECK(cache.Bytes() <= 256);
}

TEST_CASE("the newest content survives eviction", "[delivery][cache]") {
	Tree tree;
	ContentCache cache = tree.Open(192);

	for (int index = 0; index < 6; ++index) {
		const Content content("a" + std::to_string(index), {std::string(64, static_cast<char>('a' + index))});
		REQUIRE(cache.Store(content.Entry(), content.Whole));
	}

	const Content newest("newest", {std::string(64, 'z')});
	REQUIRE(cache.Store(newest.Entry(), newest.Whole));
	CHECK(cache.Contains(newest.Entry().Root));
}

TEST_CASE("a cache survives being reopened", "[delivery][cache]") {
	// The whole reason it is on a disk: the second run of a game should not
	// re-download its world.
	Tree tree;
	const Content content("persistent.mesh", {"one", "two", "three"});

	{
		ContentCache cache = tree.Open();
		REQUIRE(cache.Store(content.Entry(), content.Whole));
	}

	ContentCache reopened = tree.Open();
	const auto found = reopened.Find(content.Entry());
	REQUIRE(found.has_value());
	CHECK(*found == content.Whole);
}

TEST_CASE("clearing empties the cache", "[delivery][cache]") {
	Tree tree;
	ContentCache cache = tree.Open();
	const Content content("temporary", {"bytes"});

	REQUIRE(cache.Store(content.Entry(), content.Whole));
	REQUIRE(cache.Count() == 1);

	cache.Clear();
	CHECK(cache.Count() == 0);
	CHECK(cache.Bytes() == 0);
}

TEST_CASE("no cache path means no cache rather than a failure", "[delivery][cache]") {
	// An ordinary outcome on a read-only install: a client runs without one and
	// every fetch simply costs the network.
	CHECK_FALSE(ContentCache::Open({}, 1024).has_value());
}
