// Zeroing secret bytes so that they stay zeroed.
//
// **What cannot be tested here is the thing that matters.** Whether the
// optimiser removed the write is a property of the generated code, not of the
// program's behaviour - a test that observes the buffer afterwards keeps the
// write alive by observing it, which is exactly the condition under which
// `memset` would also have survived. So these check the contract that *can* be
// checked, and the guarantee itself rests on the implementation going through a
// volatile pointer.

#include <engine/core/SecureWipe.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.core.securewipe")

using engine::core::SecureWipe;

TEST_CASE("a secret is zeroed", "[core]") {
	std::array<uint8_t, 32> secret{};
	for (size_t index = 0; index < secret.size(); index++) {
		secret[index] = static_cast<uint8_t>(index + 1);
	}

	SecureWipe(secret);

	REQUIRE(std::all_of(secret.begin(), secret.end(), [](uint8_t value) { return value == 0; }));
}

TEST_CASE("the byte spelling zeroes too", "[core]") {
	// Two overloads rather than one and a cast at every call site: a
	// `reinterpret_cast` in front of a security primitive is a place for a
	// wrong length to hide.
	std::array<std::byte, 16> secret{};
	secret.fill(std::byte{0xAB});

	SecureWipe(secret);

	REQUIRE(std::all_of(secret.begin(), secret.end(), [](std::byte value) { return value == std::byte{0}; }));
}

TEST_CASE("only the span given is touched", "[core]") {
	// A wipe that ran past its span would erase whatever a key happened to sit
	// next to, which is a bug that looks like memory corruption a long way from
	// its cause.
	std::vector<uint8_t> buffer(64, 0xFF);

	SecureWipe(std::span<uint8_t>(buffer.data() + 16, 16));

	for (size_t index = 0; index < buffer.size(); index++) {
		const bool inside = index >= 16 && index < 32;
		REQUIRE(buffer[index] == (inside ? 0x00 : 0xFF));
	}
}

TEST_CASE("an empty span is nothing, not a null dereference", "[core]") {
	// A moved-from secret has an empty span, and a destructor is exactly where
	// that turns up.
	SecureWipe(std::span<uint8_t>{});
	SecureWipe(std::span<std::byte>{});
	SUCCEED();
}
