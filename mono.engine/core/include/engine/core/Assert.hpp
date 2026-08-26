#pragma once

// Invariants, checked or compiled out.
//
// `core/AGENTS.md` listed assertions among the things `core` owns from the
// first version of that file, and until v0.19 there was no such facility.
//
// **What the engine had instead was one `assert()`**, at
// `render/src/ResourcePreview.hpp:34`, and every other module in the tree with
// no invariant check at all. That is the telling part: a facility nobody has is
// a facility nobody reaches for, and `<cassert>` was not what people wanted. It
// says nothing about the value that was wrong, writes to a stream nothing else
// in this engine writes to, and interleaves with a frame's output when it fires
// on a job worker.
//
// ## Three macros, and the difference between them is what `release` does
//
// | Macro | `dev`, `ci`, `server`, `cdn` | `release`, `bench` |
// |---|---|---|
// | `ENGINE_ASSERT` | checks, reports, aborts | not compiled at all |
// | `ENGINE_UNREACHABLE` | reports, aborts | reports, aborts |
// | `ENGINE_ENSURE` | checks, reports, yields `false` | the same |
//
// **`ENGINE_ASSERT` is for a fact that is true or the program is already
// broken**, and it costs a `release` build nothing: the condition is named
// inside a `sizeof`, so it is still compiled, still type-checked and still
// counts as using whatever it mentions, and not one instruction is emitted.
// That is the same trick `ENGINE_TRACE` uses below its compiled floor, for the
// same reason - a check that vanishes entirely takes its variables' "used" with
// it, and the `release` build fails on warnings the `dev` build never saw.
//
// **`ENGINE_ENSURE` is for a fact that should be true and might not be**, on a
// path that can carry on without it. It is an *expression*, it evaluates its
// condition in every build, and it never aborts:
//
//     if (!ENGINE_ENSURE(buffer != nullptr)) {
//         return Status::NoBuffer;
//     }
//
// The report is rate-limited to one line a second per call site, because the
// interesting case is a check failing every frame and sixty identical lines a
// second is how it stops being read.
//
// ## What a failure looks like
//
// It goes through `core::Log` rather than to `stderr`, which is the whole
// reason this is here rather than being `assert()` with a nicer message:
//
//     [14:02:11.123] [critical] [t140234] assertion failed: index < Count · chunk
//     index 7 is past the end of 4  (Store.cpp:912 in Fetch)
//
// (one line, wrapped here to fit)
//
// One `log()` call, so the sink's own lock makes the whole line atomic. An
// invariant that fails on a job worker is a line, not fragments interleaved
// with whatever three other workers were saying. The thread id is in the
// pattern for the same reason.
//
// ## What happens after the report
//
// `Assert::SetHandler` replaces what runs after the line is written. The
// default flushes the log and calls `std::abort`, which is what a test binary
// wants for a real failure and what a `dev` run wants for a fault worth a core
// dump. `core/tests/Assert.cpp` installs its own so that it can check that an
// assert fired without the suite ending; nothing shipped should install one.
//
// @tier L0 · shared
// @since v0.19

#include <engine/core/Log.hpp>

#include <string_view>

namespace engine::core {

	// What ran after an assertion failed, once its line has been written.
	//
	// @param expression The text of the condition, exactly as it was written.
	// @param site       Where the assertion is.
	using AssertHandler = void (*)(std::string_view expression, const LogSite &site);

	// The assertion policy for this process.
	//
	// @threadsafe
	class Assert {
	  public:
		// Whether `ENGINE_ASSERT` is compiled in at all in this build.
		//
		// @return `true` for a build configured with `MONO_ASSERTS`.
		static bool IsCompiledIn();

		// Reports one failed assertion and runs the handler.
		//
		// Called by the macros. The message is written through `core::Log`
		// unfiltered, whatever any log level is set to: an invariant that
		// failed is not something a log setting may hide.
		//
		// @param expression The text of the condition.
		// @param site       Where the assertion is.
		static void Fail(std::string_view expression, const LogSite &site);

		// `Fail`, with an explanation whose arguments are already erased.
		//
		// **Erased, for `Log::Write`'s reason**: it is what keeps `fmt::format`
		// itself out of this header, which would cost every one of the two
		// hundred and fifty translation units below `Log.hpp` the whole
		// formatting library. Prefer `FailWith`, or the macros.
		//
		// @param expression The text of the condition.
		// @param site       Where the assertion is.
		// @param format     A format string, already checked at the call site.
		// @param arguments  The packed arguments.
		static void FailFormatted(
			std::string_view expression,
			const LogSite &site,
			fmt::string_view format,
			fmt::format_args arguments
		);

		// Packs `values` and hands them to `FailFormatted`.
		//
		// @param expression The text of the condition.
		// @param site       Where the assertion is.
		// @param format     The format string, checked against `Ts`.
		// @param values     What it formats.
		template <class... Ts>
		static void FailWith(
			std::string_view expression, const LogSite &site, fmt::format_string<Ts...> format, Ts &&...values
		) {
			FailFormatted(expression, site, format, fmt::make_format_args(values...));
		}

		// Reports one failed `ENGINE_ENSURE`, at most once a second per site.
		//
		// Does **not** run the handler: an ensure is a check the caller intends
		// to survive, so the report is a warning and control returns.
		//
		// @param expression The text of the condition.
		// @param site       Where the check is.
		// @param throttle   The call site's own limiter.
		// @return `false`, always, so a call site can write `return Report(...)`.
		static bool Report(std::string_view expression, const LogSite &site, LogThrottle &throttle);

		// Replaces what runs after a failed assertion's line is written.
		//
		// **For a test, and for a program that wants a crash handler in front
		// of the abort.** A handler that returns lets execution continue past a
		// broken invariant, which is what a test wants and what nothing else
		// does.
		//
		// @param handler The new handler, or `nullptr` to restore the default.
		// @return The handler that was installed before this call.
		static AssertHandler SetHandler(AssertHandler handler);

		// How many assertions and checks have failed in this process.
		//
		// **Because a test that installs a handler needs to know one fired**,
		// and because a program with a handler that returns would otherwise
		// have no record. Counts a suppressed `ENGINE_ENSURE` too, so it is the
		// number of failures rather than the number of lines. Never reset.
		static uint64_t Failures();
	};
}

// Whether the assertion macros check anything.
//
// Set by the build as a public define on `Engine::core`, so a module compiled
// with checks and linked against a core compiled without them cannot happen.
#ifndef ENGINE_ASSERTS_ENABLED
#define ENGINE_ASSERTS_ENABLED 1
#endif

// Names the condition without evaluating it, for a build with checks off.
//
// The condition is still parsed, still type-checked and still counts as using
// every name in it. What it is not is executed.
#define ENGINE_ASSERT_OMITTED(condition) ((void)sizeof(static_cast<bool>(condition)))

#if ENGINE_ASSERTS_ENABLED

// Aborts, saying what was asserted and where, when `condition` is false.
#define ENGINE_ASSERT(condition)                                                                             \
	do {                                                                                                     \
		if (!static_cast<bool>(condition)) [[unlikely]] {                                                    \
			::engine::core::Assert::Fail(#condition, ENGINE_LOG_SITE);                                       \
		}                                                                                                    \
	} while (false)

// `ENGINE_ASSERT`, with a formatted explanation of the values involved.
#define ENGINE_ASSERT_MSG(condition, ...)                                                                    \
	do {                                                                                                     \
		if (!static_cast<bool>(condition)) [[unlikely]] {                                                    \
			::engine::core::Assert::FailWith(#condition, ENGINE_LOG_SITE, __VA_ARGS__);                      \
		}                                                                                                    \
	} while (false)

#else

// Aborts, saying what was asserted and where, when `condition` is false.
#define ENGINE_ASSERT(condition) ENGINE_ASSERT_OMITTED(condition)

// `ENGINE_ASSERT`, with a formatted explanation of the values involved.
#define ENGINE_ASSERT_MSG(condition, ...)                                                                    \
	do {                                                                                                     \
		ENGINE_ASSERT_OMITTED(condition);                                                                    \
		(void)sizeof(::engine::core::Log::Ignore(__VA_ARGS__));                                              \
	} while (false)

#endif

// A branch that cannot be taken. Reports and aborts in every build.
//
// **Kept in `release`, unlike `ENGINE_ASSERT`.** The code after it does not
// exist, so there is nothing to carry on into; falling through would be
// undefined behaviour somewhere else entirely.
#define ENGINE_UNREACHABLE(...) ::engine::core::Assert::FailWith("unreachable", ENGINE_LOG_SITE, __VA_ARGS__)

// Evaluates to `condition`, reporting once a second while it is false.
//
// Checked in every build and never aborts. For a fact that should hold on a
// path that can carry on without it.
#define ENGINE_ENSURE(condition)                                                                             \
	([&]() -> bool {                                                                                         \
		if (static_cast<bool>(condition)) [[likely]] {                                                       \
			return true;                                                                                     \
		}                                                                                                    \
		static ::engine::core::LogThrottle engineEnsureThrottle;                                             \
		return ::engine::core::Assert::Report(#condition, ENGINE_LOG_SITE, engineEnsureThrottle);            \
	}())
