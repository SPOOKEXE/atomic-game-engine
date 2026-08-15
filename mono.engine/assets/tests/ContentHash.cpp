#include <engine/assets/ContentHash.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.assets.contenthash")

using engine::assets::ContentHash;
using engine::assets::Hasher;

namespace {
	std::span<const std::byte> Bytes(std::string_view text) {
		return std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size());
	}
}

// The published BLAKE3 test vectors. Not a round-trip against ourselves - a
// round-trip proves we are self-consistent, which a broken implementation also
// is. These are the numbers that say we are computing the same function
// everybody else calls BLAKE3, which is the only property that matters for an
// address two machines have to agree on.
//
// From the BLAKE3 reference test_vectors.json: the input of length N is the
// repeating byte sequence 0, 1, ..., 250, 0, 1, ... truncated to N.
TEST_CASE("the digest matches the published BLAKE3 vectors", "[assets][contenthash]") {
	const auto pattern = [](size_t length) {
		std::vector<std::byte> data(length);
		for (size_t index = 0; index < length; ++index) {
			data[index] = static_cast<std::byte>(index % 251);
		}
		return data;
	};

	struct Vector {
		size_t Length;
		const char *Digest;
	};

	// Lengths chosen to cross the implementation's own boundaries: empty, one
	// block, exactly one 1024-byte chunk, just over a chunk, and a multi-chunk
	// input that exercises the internal tree rather than the flat path.
	const Vector vectors[] = {
		{0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
		{1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
		{1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
		{1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
		{1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
		{2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
		{3072, "b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2"},
	};

	for (const Vector &entry : vectors) {
		const auto data = pattern(entry.Length);
		const ContentHash hash = Hasher::Of(data);
		INFO("length " << entry.Length);
		CHECK(hash.ToHex() == std::string(entry.Digest));
	}
}

TEST_CASE("how the input was split does not change the digest", "[assets][contenthash]") {
	const std::string text(5000, 'x');

	const ContentHash whole = Hasher::Of(Bytes(text));

	// The property the streaming verifier depends on: a chunk arriving in four
	// socket reads has to hash the same as one that arrived in one.
	Hasher piecemeal;
	size_t offset = 0;
	for (const size_t piece : {1u, 63u, 64u, 1000u, 1u}) {
		piecemeal.Update(Bytes(std::string_view(text).substr(offset, piece)));
		offset += piece;
	}
	piecemeal.Update(Bytes(std::string_view(text).substr(offset)));

	CHECK(piecemeal.Finish() == whole);
}

TEST_CASE("Finish does not end the hash", "[assets][contenthash]") {
	Hasher hasher;
	hasher.Update(Bytes("abc"));
	const ContentHash first = hasher.Finish();

	// Taking a digest of the stream so far and continuing is what lets one pass
	// over a file produce a per-chunk hash and a whole-file hash together.
	CHECK(hasher.Finish() == first);

	hasher.Update(Bytes("def"));
	const ContentHash second = hasher.Finish();

	CHECK(second != first);
	CHECK(second == Hasher::Of(Bytes("abcdef")));
}

TEST_CASE("Reset returns a hasher to its initial state", "[assets][contenthash]") {
	Hasher hasher;
	hasher.Update(Bytes("something else entirely"));
	hasher.Reset();
	hasher.Update(Bytes("abc"));

	CHECK(hasher.Finish() == Hasher::Of(Bytes("abc")));
}

TEST_CASE("the empty input has a digest of its own", "[assets][contenthash]") {
	const ContentHash empty = Hasher::Of({});

	// Which is what makes the all-zero value usable as "not set": no input
	// produces it, so the two can never be confused.
	CHECK_FALSE(empty.IsZero());
	CHECK(ContentHash{}.IsZero());
	CHECK(empty != ContentHash{});
}

TEST_CASE("hex round-trips", "[assets][contenthash]") {
	const ContentHash hash = Hasher::Of(Bytes("round trip"));
	const std::string text = hash.ToHex();

	REQUIRE(text.size() == ContentHash::BYTES * 2);
	const auto parsed = ContentHash::FromHex(text);
	REQUIRE(parsed.has_value());
	CHECK(*parsed == hash);
}

TEST_CASE("hex parsing refuses anything but 64 lowercase digits", "[assets][contenthash]") {
	const std::string valid = Hasher::Of(Bytes("x")).ToHex();

	CHECK_FALSE(ContentHash::FromHex("").has_value());
	CHECK_FALSE(ContentHash::FromHex(valid.substr(0, 63)).has_value());
	CHECK_FALSE(ContentHash::FromHex(valid + "0").has_value());
	CHECK_FALSE(ContentHash::FromHex("0x" + valid.substr(2)).has_value());

	// Uppercase is refused rather than accepted and folded. Two spellings of
	// one address is two cache keys for one piece of content.
	std::string upper = valid;
	for (char &character : upper) {
		if (character >= 'a' && character <= 'f') {
			character = static_cast<char>(character - 'a' + 'A');
		}
	}
	if (upper != valid) {
		CHECK_FALSE(ContentHash::FromHex(upper).has_value());
	}
}

TEST_CASE("hashes order byte-wise, so a list has one canonical arrangement", "[assets][contenthash]") {
	ContentHash low;
	ContentHash high;
	low.Digest[0] = 0x01;
	high.Digest[0] = 0x02;

	CHECK(low < high);
	CHECK_FALSE(high < low);

	// Later bytes only decide when earlier ones tie, which is what makes the
	// order the same as comparing the hex strings.
	ContentHash tie = low;
	tie.Digest[31] = 0xFF;
	CHECK(low < tie);
	CHECK(tie < high);
}

TEST_CASE("a one-bit change changes the digest completely", "[assets][contenthash]") {
	std::vector<std::byte> data(4096, std::byte{0xAB});
	const ContentHash before = Hasher::Of(data);

	data[2048] = std::byte{0xAA};
	const ContentHash after = Hasher::Of(data);

	REQUIRE(before != after);

	// Not a strict property of the function, but a wrong implementation that
	// only mixes part of its input tends to leave most bytes alone. Half the
	// digest differing is the loose floor that catches that.
	size_t differing = 0;
	for (size_t index = 0; index < ContentHash::BYTES; ++index) {
		if (before.Digest[index] != after.Digest[index]) {
			++differing;
		}
	}
	CHECK(differing > ContentHash::BYTES / 2);
}
