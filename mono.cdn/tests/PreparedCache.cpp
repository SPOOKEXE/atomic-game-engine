#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cdn/PreparedCache.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("cdn.preparedcache")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.core.metrics")

using cdn::PreparedCache;
using cdn::PreparedFrame;
using cdn::PreparedKey;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::core::Metrics;

namespace {
	ContentHash Hash(std::string_view text) {
		return Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
	}

	PreparedKey Key(std::string_view bundle, std::string_view dictionary = "") {
		PreparedKey key;
		key.Bundle = Hash(bundle);
		if (!dictionary.empty()) {
			key.Dictionary = Hash(dictionary);
		}
		return key;
	}

	std::vector<std::byte> Frame(size_t bytes, std::byte fill = std::byte{0xAB}) {
		return std::vector<std::byte>(bytes, fill);
	}
}

TEST_CASE("a stored group is found again", "[cdn][preparedcache]") {
	PreparedCache cache(1024);

	REQUIRE(cache.Insert(Key("terrain"), Frame(100)) != nullptr);

	const PreparedFrame found = cache.Find(Key("terrain"));
	REQUIRE(found != nullptr);
	CHECK(found->size() == 100);
	CHECK(cache.Count() == 1);
	CHECK(cache.Bytes() == 100);
}

TEST_CASE("a miss answers nothing", "[cdn][preparedcache]") {
	PreparedCache cache(1024);

	CHECK(cache.Find(Key("never stored")) == nullptr);
	CHECK_FALSE(cache.Contains(Key("never stored")));
}

TEST_CASE("the dictionary is part of the key", "[cdn][preparedcache]") {
	PreparedCache cache(1024);

	REQUIRE(cache.Insert(Key("terrain", "dictionary-one"), Frame(100)) != nullptr);

	// A group compressed against one dictionary is a different artefact from the
	// same group compressed against another, and serving the wrong one hands a
	// client bytes it cannot decode. Keying on the bundle alone is the bug that
	// only appears the day a second dictionary exists.
	CHECK(cache.Find(Key("terrain", "dictionary-one")) != nullptr);
	CHECK(cache.Find(Key("terrain", "dictionary-two")) == nullptr);
	CHECK(cache.Find(Key("terrain")) == nullptr);
}

TEST_CASE("the least recently used group is evicted first", "[cdn][preparedcache]") {
	PreparedCache cache(250);

	REQUIRE(cache.Insert(Key("a"), Frame(100)) != nullptr);
	REQUIRE(cache.Insert(Key("b"), Frame(100)) != nullptr);

	// Touch "a", so "b" becomes the oldest.
	REQUIRE(cache.Find(Key("a")) != nullptr);

	REQUIRE(cache.Insert(Key("c"), Frame(100)) != nullptr);

	CHECK(cache.Contains(Key("a")));
	CHECK_FALSE(cache.Contains(Key("b")));
	CHECK(cache.Contains(Key("c")));
	CHECK(cache.Bytes() <= cache.Capacity());
}

TEST_CASE("a frame larger than the capacity is refused", "[cdn][preparedcache]") {
	PreparedCache cache(100);

	REQUIRE(cache.Insert(Key("small"), Frame(50)) != nullptr);

	// One group that evicts everything else on every insert is worse than not
	// caching that group at all.
	CHECK(cache.Insert(Key("enormous"), Frame(200)) == nullptr);
	CHECK(cache.Contains(Key("small")));
	CHECK(cache.Count() == 1);
}

TEST_CASE("an evicted frame stays alive while it is being read", "[cdn][preparedcache]") {
	PreparedCache cache(150);

	const PreparedFrame streaming = cache.Insert(Key("a"), Frame(100, std::byte{0x11}));
	REQUIRE(streaming != nullptr);

	// Evicts "a".
	REQUIRE(cache.Insert(Key("b"), Frame(100)) != nullptr);
	REQUIRE_FALSE(cache.Contains(Key("a")));

	// Eviction happens on whichever thread inserts, and a reader streaming a
	// frame must not have it freed underneath. Shared ownership is what makes a
	// slow client safe without holding a lock for the length of a transfer.
	CHECK(streaming->size() == 100);
	CHECK((*streaming)[0] == std::byte{0x11});
}

TEST_CASE("inserting the same key twice keeps the first", "[cdn][preparedcache]") {
	PreparedCache cache(1024);

	const PreparedFrame first = cache.Insert(Key("terrain"), Frame(100, std::byte{0x01}));
	REQUIRE(first != nullptr);

	// Two threads prepared the same group and raced. Preparation is
	// deterministic so both results are byte-identical - the first wins, and the
	// entry is not churned out from under whatever is streaming it.
	const PreparedFrame second = cache.Insert(Key("terrain"), Frame(100, std::byte{0x02}));
	REQUIRE(second != nullptr);

	CHECK(second == first);
	CHECK(cache.Count() == 1);
	CHECK(cache.Bytes() == 100);
}

TEST_CASE("Contains does not disturb the order", "[cdn][preparedcache]") {
	PreparedCache cache(250);

	REQUIRE(cache.Insert(Key("a"), Frame(100)) != nullptr);
	REQUIRE(cache.Insert(Key("b"), Frame(100)) != nullptr);

	// Contains is for a diagnostic. If it marked "a" used, a caller checking
	// what is cached would change what gets evicted.
	CHECK(cache.Contains(Key("a")));
	REQUIRE(cache.Insert(Key("c"), Frame(100)) != nullptr);

	CHECK_FALSE(cache.Contains(Key("a")));
	CHECK(cache.Contains(Key("b")));
}

TEST_CASE("clearing empties the cache", "[cdn][preparedcache]") {
	PreparedCache cache(1024);
	REQUIRE(cache.Insert(Key("a"), Frame(100)) != nullptr);
	REQUIRE(cache.Insert(Key("b"), Frame(100)) != nullptr);

	cache.Clear();

	CHECK(cache.Count() == 0);
	CHECK(cache.Bytes() == 0);
	CHECK_FALSE(cache.Contains(Key("a")));
}

TEST_CASE("a zero capacity falls back to the default", "[cdn][preparedcache]") {
	// A cache that evicts everything it is given looks like a cache and behaves
	// like a leak of CPU.
	const PreparedCache cache(0);
	CHECK(cache.Capacity() == PreparedCache::DEFAULT_CAPACITY_BYTES);
}

TEST_CASE("the cache never exceeds its capacity", "[cdn][preparedcache]") {
	PreparedCache cache(1000);

	for (int index = 0; index < 50; ++index) {
		cache.Insert(Key("group-" + std::to_string(index)), Frame(100));
		CHECK(cache.Bytes() <= cache.Capacity());
	}

	CHECK(cache.Count() == 10);
}

TEST_CASE("hits, misses and evictions are counted", "[cdn][preparedcache][metrics]") {
	PreparedCache cache(150);

	Metrics::Clear();
	REQUIRE(cache.Insert(Key("a"), Frame(100)) != nullptr);
	CHECK(cache.Find(Key("a")) != nullptr);
	CHECK(cache.Find(Key("missing")) == nullptr);
	REQUIRE(cache.Insert(Key("b"), Frame(100)) != nullptr);
	CHECK(cache.Insert(Key("huge"), Frame(1000)) == nullptr);

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

	// The hit rate is the number that says whether preparing is costing what it
	// should. A miss and a refusal read very differently: one is cold, the other
	// is a group that will never be cached at all.
	CHECK(total("cdn.prepared.hit") == 1.0);
	CHECK(total("cdn.prepared.miss") == 1.0);
	CHECK(total("cdn.prepared.stored") == 2.0);
	CHECK(total("cdn.prepared.evicted") == 1.0);
	CHECK(total("cdn.prepared.refused") == 1.0);
}
