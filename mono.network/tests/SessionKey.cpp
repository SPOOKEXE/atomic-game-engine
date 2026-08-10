// The pre-shared key: where one comes from, what it tags, and what it refuses.
//
// The passphrase path is the one with a standing obligation attached — the salt
// and the round count are part of the key, so a change to either silently
// invalidates every key anybody derived from words. The vector below is what
// makes that a failing test rather than a session nobody can join.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <network/SessionKey.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("network.sessionkey")

using network::SessionKey;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes;
		bytes.reserve(text.size());
		for (const char character : text) {
			bytes.push_back(static_cast<std::byte>(character));
		}
		return bytes;
	}

	SessionKey FromWords(std::string_view words) {
		auto key = SessionKey::FromPassphrase(words);
		REQUIRE(key.has_value());
		return std::move(*key);
	}
}

TEST_CASE("a key round-trips through its own text", "[network][sessionkey]") {
	auto drawn = SessionKey::Draw();
	REQUIRE(drawn.has_value());

	const std::string text = drawn->Text();
	CHECK(text.size() == SessionKey::BYTES * 2);

	auto read = SessionKey::FromText(text);
	REQUIRE(read.has_value());
	CHECK(read->Text() == text);

	// Uppercase reads the same, because a person retyping a key off a screen
	// will not preserve the case and refusing them would be refusing for no
	// reason.
	std::string shouted = text;
	for (char &character : shouted) {
		if (character >= 'a' && character <= 'f') {
			character = static_cast<char>(character - 'a' + 'A');
		}
	}
	auto loud = SessionKey::FromText(shouted);
	REQUIRE(loud.has_value());
	CHECK(loud->Text() == text);
}

TEST_CASE("hostile text is refused rather than half-read", "[network][sessionkey]") {
	CHECK_FALSE(SessionKey::FromText("").has_value());
	CHECK_FALSE(SessionKey::FromText(std::string(63, 'a')).has_value());
	CHECK_FALSE(SessionKey::FromText(std::string(65, 'a')).has_value());

	// Right length, wrong alphabet — and the wrong character is at the end, so
	// a reader that filled the key as it went has already written 31 bytes by
	// the time it finds out.
	std::string nearly(63, 'a');
	nearly.push_back('z');
	CHECK_FALSE(SessionKey::FromText(nearly).has_value());
}

TEST_CASE("secret material has to be exactly the right length", "[network][sessionkey]") {
	const std::vector<std::byte> exact(SessionKey::BYTES, std::byte{0x11});
	CHECK(SessionKey::FromSecret(exact).has_value());

	CHECK_FALSE(
		SessionKey::FromSecret(std::vector<std::byte>(SessionKey::BYTES - 1, std::byte{1})).has_value()
	);
	CHECK_FALSE(
		SessionKey::FromSecret(std::vector<std::byte>(SessionKey::BYTES + 1, std::byte{1})).has_value()
	);
	CHECK_FALSE(SessionKey::FromSecret({}).has_value());
}

TEST_CASE("the same words make the same key on both machines", "[network][sessionkey]") {
	const SessionKey first = FromWords("the quick brown fox");
	const SessionKey second = FromWords("the quick brown fox");
	CHECK(first.Text() == second.Text());

	// Not trimmed and not case-folded. Somebody who typed a trailing space
	// should be told the key is wrong rather than quietly handed one that is
	// not the one their friend derived.
	CHECK(FromWords("the quick brown fox ").Text() != first.Text());
	CHECK(FromWords("The Quick Brown Fox").Text() != first.Text());

	CHECK_FALSE(SessionKey::FromPassphrase("").has_value());
}

TEST_CASE(
	"the passphrase derivation is a commitment, not an implementation detail", "[network][sessionkey]"
) {
	// **This vector is the point of the suite.** The salt and the round count
	// are part of the key: changing either changes every key ever derived from
	// a passphrase, and the symptom is a private session nobody can join with
	// the words that worked yesterday. If this fails, the question is not "what
	// is the new expected value" — it is whether the change was meant, and
	// whether the advert version moved with it.
	CHECK(FromWords("atomic").Text() == "b78d1fb97427b4c754db1988a231734a4fa4982ef157c970da451482f60fac9a");
	CHECK(SessionKey::PASSPHRASE_ROUNDS == 200000);
}

TEST_CASE("a tag commits to the bytes it was made over", "[network][sessionkey]") {
	const SessionKey key = FromWords("shared secret");
	const std::vector<std::byte> message = Bytes("come and join me");

	const std::array<std::byte, SessionKey::TAG_BYTES> tag = key.Tag(message);
	CHECK(key.Admits(message, tag));

	// One byte different in the message.
	std::vector<std::byte> altered = message;
	altered.back() = static_cast<std::byte>(0xFF);
	CHECK_FALSE(key.Admits(altered, tag));

	// One byte different in the tag.
	std::array<std::byte, SessionKey::TAG_BYTES> forged = tag;
	forged[0] = static_cast<std::byte>(static_cast<uint8_t>(forged[0]) ^ 0x01u);
	CHECK_FALSE(key.Admits(message, forged));

	// A different key over the same message.
	CHECK_FALSE(FromWords("another secret").Admits(message, tag));

	// A truncated tag is a refusal rather than a comparison over whichever
	// bytes happened to be there — which is the shape of a forgery that
	// succeeds by presenting nothing.
	CHECK_FALSE(key.Admits(message, std::span(tag).first(16)));
	CHECK_FALSE(key.Admits(message, {}));

	// An empty message is a legitimate thing to tag, and its tag is not the
	// tag of anything else.
	const std::array<std::byte, SessionKey::TAG_BYTES> nothing = key.Tag({});
	CHECK(key.Admits({}, nothing));
	CHECK_FALSE(key.Admits(message, nothing));
}

TEST_CASE("moving a key leaves nothing behind", "[network][sessionkey]") {
	SessionKey source = FromWords("moved from");
	const std::string before = source.Text();

	SessionKey moved = std::move(source);
	CHECK(moved.Text() == before);

	// The source's storage is zeroed rather than left holding a copy nobody
	// believes exists. Reading a moved-from object is legal and this is what it
	// says.
	CHECK(source.Text() == std::string(SessionKey::BYTES * 2, '0'));

	SessionKey other = FromWords("assigned over");
	other = std::move(moved);
	CHECK(other.Text() == before);
	CHECK(moved.Text() == std::string(SessionKey::BYTES * 2, '0'));
}
