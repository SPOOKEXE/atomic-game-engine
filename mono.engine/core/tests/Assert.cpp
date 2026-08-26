#include <engine/core/Assert.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.core.assert")
TEST_DEPENDS("engine.core.log")

using engine::core::Assert;
using engine::core::AssertHandler;
using engine::core::Log;
using engine::core::LogLevel;
using engine::core::LogSite;

namespace {
	// What the last failure said, so a case can check the report rather than
	// only that something happened.
	std::string LastExpression;
	std::string LastFile;
	uint32_t LastLine = 0;
	int Handled = 0;

	// **The reason `SetHandler` exists.** The default aborts, which is right for
	// a program and impossible for a suite that wants to check that an assert
	// fires. A handler that returns lets the case carry on past a deliberately
	// broken invariant.
	void Remember(std::string_view expression, const LogSite &site) {
		LastExpression = std::string(expression);
		LastFile = site.File == nullptr ? "" : site.File;
		LastLine = site.Line;
		Handled++;
	}

	// Installs `Remember` for the length of a case and puts the default back.
	// A suite that left its handler installed would turn every later abort in
	// the binary into a silent return.
	struct Handling {
		Handling() : Previous(Assert::SetHandler(&Remember)) {
			LastExpression.clear();
			LastFile.clear();
			LastLine = 0;
			Handled = 0;
		}
		~Handling() {
			Assert::SetHandler(Previous);
		}
		AssertHandler Previous;
	};

	int Evaluations = 0;

	bool Counted(bool answer) {
		Evaluations++;
		return answer;
	}
}

TEST_CASE("an assertion that holds does nothing", "[assert]") {
	const Handling handling;

	ENGINE_ASSERT(1 + 1 == 2);
	ENGINE_ASSERT_MSG(1 + 1 == 2, "arithmetic is {}", "fine");

	CHECK(Handled == 0);
}

TEST_CASE("a failed assertion says what was asserted and where", "[assert]") {
	const Handling handling;
	if (!Assert::IsCompiledIn()) {
		SUCCEED("this build compiles ENGINE_ASSERT out");
		return;
	}

	const uint64_t before = Assert::Failures();
	const int line = __LINE__ + 1;
	ENGINE_ASSERT(2 + 2 == 5);

	CHECK(Handled == 1);
	CHECK(LastExpression == "2 + 2 == 5");
	CHECK(LastLine == static_cast<uint32_t>(line));
	CHECK(LastFile.find("Assert.cpp") != std::string::npos);
	CHECK(Assert::Failures() == before + 1);
}

TEST_CASE("a failed assertion carries its explanation", "[assert]") {
	const Handling handling;
	if (!Assert::IsCompiledIn()) {
		SUCCEED("this build compiles ENGINE_ASSERT out");
		return;
	}

	const int index = 7;
	const int count = 4;
	ENGINE_ASSERT_MSG(index < count, "chunk index {} is past the end of {}", index, count);

	CHECK(Handled == 1);
	CHECK(LastExpression == "index < count");
}

TEST_CASE("an assertion compiled out evaluates nothing", "[assert]") {
	const Handling handling;

	Evaluations = 0;
	ENGINE_ASSERT(Counted(true));

	// Both halves of the claim, so that the case is meaningful in a `release`
	// build as well as a `dev` one: compiled in, the condition runs exactly
	// once; compiled out, it does not run at all and is still type-checked,
	// which is what the `sizeof` in the macro is for.
	if (Assert::IsCompiledIn()) {
		CHECK(Evaluations == 1);
	} else {
		CHECK(Evaluations == 0);
	}
	CHECK(Handled == 0);
}

TEST_CASE("a check yields its condition and never aborts", "[assert]") {
	const Handling handling;
	Log::SetLevel(LogLevel::Info);

	CHECK(ENGINE_ENSURE(1 + 1 == 2));

	// The false case is the point: it reports and returns rather than ending
	// the process, so a caller can decide what to do about it.
	const uint64_t before = Assert::Failures();
	CHECK_FALSE(ENGINE_ENSURE(2 + 2 == 5));
	CHECK(Assert::Failures() == before + 1);

	// And it does not run the handler, because an ensure is a check the caller
	// intends to survive.
	CHECK(Handled == 0);
}

TEST_CASE("a check is compiled in whatever the assert switch says", "[assert]") {
	const Handling handling;

	Evaluations = 0;
	CHECK(ENGINE_ENSURE(Counted(true)));
	CHECK(Evaluations == 1);
}

TEST_CASE("a check repeating every frame reports once", "[assert]") {
	const Handling handling;

	const uint64_t before = Assert::Failures();
	for (int frame = 0; frame < 200; frame++) {
		CHECK_FALSE(ENGINE_ENSURE(false));
	}

	// Every failure is counted, and the point of the throttle is that they are
	// not every one of them printed: one line a second per call site, because
	// sixty identical lines a second is how a real fault stops being read.
	CHECK(Assert::Failures() == before + 200);
}
