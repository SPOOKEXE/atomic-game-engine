// `core::FromChars`, and the portable path behind it.
//
// **Both are checked here, and on every platform.** `FromChars` is
// `std::from_chars` wherever the library has a floating-point one, so on the
// machines this engine is developed on the fallback would otherwise never run -
// and the only computer that takes it is a macOS runner with no toolchain to
// debug on. So `portable::Read` is exercised directly, against the same cases,
// and the two are compared where the platform can answer both.

#include "PortableChars.hpp"

#include <engine/core/Chars.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

TEST_SUITE_ID("engine.core.chars")

using Catch::Approx;
using engine::core::FromChars;
namespace portable = engine::core::portable;

namespace {

	// The portable path, whatever this platform's `FromChars` chose.
	double Portable(std::string_view text, std::errc &code, size_t &consumed) {
		double value = -1.0;
		const auto result = portable::Read(text.data(), text.data() + text.size(), value);
		code = result.ec;
		consumed = static_cast<size_t>(result.ptr - text.data());
		return value;
	}

	template <class T> void CheckRangeError(std::string_view text, size_t consumed) {
		T portableValue = 42;
		const auto portableResult = portable::Read(text.data(), text.data() + text.size(), portableValue);
		T value = 42;
		const auto result = FromChars(text.data(), text.data() + text.size(), value);

		INFO("text was \"" << text << "\"");
		CHECK(portableResult.ec == std::errc::result_out_of_range);
		CHECK(portableResult.ptr == text.data() + consumed);
		CHECK(portableValue == 42);
		CHECK(result.ec == std::errc::result_out_of_range);
		CHECK(result.ptr == text.data() + consumed);
		CHECK(value == 42);
	}

	template <class T> void CheckBoundaryValue(std::string_view text, T expected) {
		T portableValue = 42;
		const auto portableResult = portable::Read(text.data(), text.data() + text.size(), portableValue);
		T value = 42;
		const auto result = FromChars(text.data(), text.data() + text.size(), value);

		INFO("text was \"" << text << "\"");
		CHECK(portableResult.ec == std::errc{});
		CHECK(portableResult.ptr == text.data() + text.size());
		CHECK(portableValue == expected);
		CHECK(std::signbit(portableValue) == std::signbit(expected));
		CHECK(result.ec == std::errc{});
		CHECK(result.ptr == text.data() + text.size());
		CHECK(value == expected);
		CHECK(std::signbit(value) == std::signbit(expected));
	}
}

TEST_CASE("a number is read and the rest is left alone", "[chars]") {
	const std::string_view text = "3.5kg";
	double value = 0.0;
	const auto result = FromChars(text.data(), text.data() + text.size(), value);

	CHECK(result.ec == std::errc{});
	CHECK(value == Approx(3.5));

	// **`ptr` is the contract every caller in this engine tests.** They compare
	// it against the end to mean "there was trailing rubbish", so a parse that
	// consumed the number and lied about where it stopped would accept `3.5kg`
	// as a valid attribute.
	CHECK(result.ptr == text.data() + 3);
}

TEST_CASE("what is not a number is refused rather than guessed at", "[chars]") {
	for (const std::string_view text : {"", "kg", "-", ".", "-.", "+1.5", " 1.5"}) {
		float value = -1.0f;
		const auto result = FromChars(text.data(), text.data() + text.size(), value);
		INFO("text was \"" << text << "\"");
		CHECK(result.ec == std::errc::invalid_argument);

		// Untouched on failure, so a caller that ignores the code still has the
		// value it started with rather than a half-parsed one.
		CHECK(value == -1.0f);
	}
}

TEST_CASE("a leading sign, a fraction and an exponent all read", "[chars]") {
	struct Case {
		std::string_view Text;
		double Value;
	};
	for (const Case one : {
			 Case{"0", 0.0},
			 Case{"-2", -2.0},
			 Case{"1.25", 1.25},
			 Case{"-0.5", -0.5},
			 Case{".5", 0.5},
			 Case{"2.", 2.0},
			 Case{"1e3", 1000.0},
			 Case{"1E3", 1000.0},
			 Case{"1.5e-2", 0.015},
			 Case{"-1.5E+2", -150.0},
		 }) {
		double value = 0.0;
		const auto result = FromChars(one.Text.data(), one.Text.data() + one.Text.size(), value);
		INFO("text was \"" << one.Text << "\"");
		CHECK(result.ec == std::errc{});
		CHECK(result.ptr == one.Text.data() + one.Text.size());
		CHECK(value == Approx(one.Value));
	}
}

TEST_CASE("an exponent with no digits is not part of the number", "[chars]") {
	// `1e` is the number one followed by a letter. A scan that swallowed the
	// `e` would report a `ptr` past it and the caller would accept `1e` as one.
	const std::string_view text = "1e";
	double value = 0.0;
	const auto result = FromChars(text.data(), text.data() + text.size(), value);
	CHECK(result.ec == std::errc{});
	CHECK(value == Approx(1.0));
	CHECK(result.ptr == text.data() + 1);
}

TEST_CASE("the portable path agrees with the platform's own", "[chars]") {
	// **The case that matters, because one of these two is what macOS runs.**
	// Where the platform has both, they are checked against each other; where it
	// has only the portable one, `FromChars` *is* the portable one and this
	// still checks it.
	for (const std::string_view text :
		 {"0",
		  "-2",
		  "1.25",
		  "-0.5",
		  ".5",
		  "2.",
		  "1e3",
		  "1.5e-2",
		  "-1.5E+2",
		  "3.5kg",
		  "1e",
		  "",
		  "kg",
		  "+1",
		  " 1"}) {
		double mine = 0.0;
		std::errc code{};
		size_t consumed = 0;
		mine = Portable(text, code, consumed);

		double theirs = 0.0;
		const auto result = FromChars(text.data(), text.data() + text.size(), theirs);

		INFO("text was \"" << text << "\"");
		CHECK(code == result.ec);
		CHECK(consumed == static_cast<size_t>(result.ptr - text.data()));
		if (code == std::errc{}) {
			CHECK(mine == Approx(theirs));
		}
	}
}

TEST_CASE("a float and a double read the same text", "[chars]") {
	const std::string_view text = "0.125";

	float single = 0.0f;
	CHECK(FromChars(text.data(), text.data() + text.size(), single).ec == std::errc{});
	CHECK(single == Approx(0.125f));

	double twice = 0.0;
	CHECK(FromChars(text.data(), text.data() + text.size(), twice).ec == std::errc{});
	CHECK(twice == Approx(0.125));
}

TEST_CASE("floating-point range errors leave the value untouched", "[chars]") {
	for (const std::string_view text : {"1e9999", "-1e9999", "1e-9999", "-1e-9999"}) {
		CheckRangeError<float>(text, text.size());
		CheckRangeError<double>(text, text.size());
	}

	CheckRangeError<float>("1e9999kg", 6);
	CheckRangeError<double>("1e9999kg", 6);
}

TEST_CASE("zero and representable subnormals remain valid", "[chars]") {
	CheckBoundaryValue<float>("0e-9999", 0.0f);
	CheckBoundaryValue<double>("0e-9999", 0.0);
	CheckBoundaryValue<float>("-0e-9999", -0.0f);
	CheckBoundaryValue<double>("-0e-9999", -0.0);
	CheckBoundaryValue<float>("1.40129846e-45", std::numeric_limits<float>::denorm_min());
	CheckBoundaryValue<double>("4.9406564584124654e-324", std::numeric_limits<double>::denorm_min());
}
