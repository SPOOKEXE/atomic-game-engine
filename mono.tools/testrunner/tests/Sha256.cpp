#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <testrunner/Sha256.hpp>

TEST_SUITE_ID("tools.testrunner.sha256")

using testrunner::Sha256;

// The FIPS 180-4 vectors. These guarded a hand-written implementation; they now
// guard the Crypto++ one, and they did not change when it was swapped, which is
// the point of them.
//
// A wrong digest does not look wrong from the outside. Every signature would be
// wrong in the same way, the cascade would still appear to work, and the only
// symptom would be a shared cache that silently never hits. These vectors are
// what make that a failing test instead.
TEST_CASE("the NIST vectors match", "[sha256]") {
	REQUIRE(Sha256::Of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	REQUIRE(Sha256::Of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	REQUIRE(
		Sha256::Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
	);
}

TEST_CASE("a message spanning several blocks matches", "[sha256]") {
	// One million 'a'. Exercises buffering across block boundaries - the path
	// Runner::HashFile takes when it reads a file in 64 KiB chunks, and the one
	// that would break if Update() ever stopped being properly streaming.
	Sha256 hash;
	const std::string chunk(1000, 'a');
	for (int index = 0; index < 1000; index++) {
		hash.Update(chunk);
	}
	REQUIRE(hash.Hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("the split between updates does not change the digest", "[sha256]") {
	const std::string message = "the quick brown fox jumps over the lazy dog, repeatedly";

	Sha256 whole;
	whole.Update(message);

	Sha256 pieces;
	for (const char character : message) {
		pieces.Update(&character, 1);
	}

	REQUIRE(whole.Hex() == pieces.Hex());
}

TEST_CASE("a message that lands exactly on a block boundary matches", "[sha256]") {
	// 55, 56 and 64 bytes are the padding edge cases: 56 is where the length
	// field no longer fits and the padding runs into a second block.
	REQUIRE(
		Sha256::Of(std::string(55, 'x')) == "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"
	);
	REQUIRE(
		Sha256::Of(std::string(56, 'x')) == "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"
	);
	REQUIRE(
		Sha256::Of(std::string(64, 'x')) == "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"
	);
}

TEST_CASE("Hex is idempotent", "[sha256]") {
	Sha256 hash;
	hash.Update("stable");

	const std::string first = hash.Hex();
	REQUIRE(hash.Hex() == first);
}
