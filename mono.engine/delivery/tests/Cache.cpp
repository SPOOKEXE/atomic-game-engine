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
	// of the whole happens to look right - and a suite built only out of those
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
	// the root refused every asset cut into more than one chunk - silently, as
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
	// Deleted, not merely refused - leaving it would make every later run pay
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

TEST_CASE("the ceiling holds across many stores", "[delivery][cache]") {
	// **The regression for the running total.** `MakeRoom` walks the directory
	// only when the total it keeps says the ceiling is in reach, so a total that
	// stopped being maintained would show up here and nowhere else: every store
	// would succeed, nothing would ever be evicted and the cache would grow
	// without bound. Sixty-four stores against a ceiling four of them fill.
	Tree tree;
	ContentCache cache = tree.Open(256);

	for (int index = 0; index < 64; ++index) {
		// Distinct content, so every store is a new entry rather than a rewrite
		// of one already there - which is a different path and is pinned by the
		// case below.
		const Content content(
			"a" + std::to_string(index), {std::string(60, 'x') + std::to_string(1000 + index)}
		);
		REQUIRE(cache.Store(content.Entry(), content.Whole));
		CHECK(cache.Bytes() <= 256);
	}
}

TEST_CASE("storing the same asset twice does not spend the ceiling twice", "[delivery][cache]") {
	// A content address names one thing, so re-storing an entry writes the
	// bytes that are already there. A total that counted them again would evict
	// on a figure that only ever grows - and a group arriving with a member the
	// cache already holds is ordinary rather than exotic.
	Tree tree;
	ContentCache cache = tree.Open(256);
	const Content content("repeated", {std::string(64, 'r')});

	for (int index = 0; index < 16; ++index) {
		REQUIRE(cache.Store(content.Entry(), content.Whole));
	}

	CHECK(cache.Count() == 1);
	CHECK(cache.Bytes() == 64);
	CHECK(cache.Contains(content.Entry().Root));
}

TEST_CASE("eviction sees content this cache did not write", "[delivery][cache]") {
	// **Why the running total is a trigger and not an answer.** A cache
	// directory is a directory: another process, an older build or a user with a
	// file manager can put bytes in it, and the total this cache keeps will not
	// have counted them. What that costs is stated rather than hidden - the
	// ceiling is crossed a little late, by whatever the other writer wrote.
	//
	// What it must never cost is a file that cannot be evicted. Eviction chooses
	// from a walk of the directory, so the foreign entry is an ordinary
	// candidate and the first store that triggers a walk takes it. A remembered
	// listing could not have done that, which is the reason this holds a total
	// and not one.
	Tree tree;
	ContentCache cache = tree.Open(256);

	const fs::path foreign = tree.Root / "ff";
	std::error_code failure;
	fs::create_directories(foreign, failure);
	{
		std::ofstream file(foreign / std::string(64, 'f'), std::ios::binary | std::ios::trunc);
		file << std::string(200 * 1024, 'x');
	}
	REQUIRE(cache.Bytes() > 256);

	// Enough stores that the total this cache does keep reaches the ceiling and
	// a walk happens. Four sixty-four byte assets is the ceiling exactly, so the
	// fifth is the one that looks.
	for (int index = 0; index < 8; ++index) {
		const Content content(
			"after" + std::to_string(index), {std::string(64, static_cast<char>('a' + index))}
		);
		REQUIRE(cache.Store(content.Entry(), content.Whole));
	}

	CHECK(cache.Bytes() <= 256);
	// And the foreign entry is gone rather than merely uncounted.
	CHECK_FALSE(fs::exists(foreign / std::string(64, 'f')));
}

TEST_CASE("a reopened cache knows what it already holds", "[delivery][cache]") {
	// The total is seeded by one walk when the cache is opened. Without that a
	// second run would believe it held nothing and would fill the directory to
	// twice its ceiling before the first eviction - which is the failure a
	// remembered figure has and a scan per store does not, so it is pinned here.
	Tree tree;
	for (int index = 0; index < 12; ++index) {
		ContentCache cache = tree.Open(256);
		const Content content(
			"run" + std::to_string(index), {std::string(64, static_cast<char>('a' + index))}
		);
		REQUIRE(cache.Store(content.Entry(), content.Whole));
		CHECK(cache.Bytes() <= 256);
	}
}
