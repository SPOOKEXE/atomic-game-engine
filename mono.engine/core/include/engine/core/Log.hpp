#pragma once

// The engine log. Also the destination for the userland globals `print`,
// `warn`, `error` and `assert`, which is why the levels below are named after
// what a script author sees rather than after spdlog's set.
//
// **No spdlog here, and that is the point of the shape below.** This header is
// included by about two hundred and fifty translation units, and
// `<spdlog/spdlog.h>` preprocesses to 118,000 lines - so every one of them paid
// for the whole logging library whether it logged or not, and twenty-nine of
// them contain no log call at all. What a caller actually needs is a way to
// hand a format string and some arguments to something that will format them;
// `<spdlog/fmt/bundled/base.h>` is 7,373 lines and declares exactly that. fmt's
// own comment on `core.h` says the same thing: use `base.h` if you do not need
// `fmt::format` itself.
//
// The formatting happens in `Log.cpp`, behind `Write`, which takes the
// arguments already type-erased into `fmt::format_args`. That is fmt's own
// recommended shape for this and it keeps the compile-time format checking:
// `fmt::format_string<Ts...>` still rejects a mismatched `{}` at the call site.
//
// **The budget is 22,842 preprocessed lines and it is a real constraint on
// what may be added here.** Everything below was written to fit inside it:
// `<atomic>` is 7,452 lines on its own and `Name.hpp` is 51,820, so neither is
// included even though a category *is* a `core::Name` and a category's level
// *is* read atomically. `LogCategory` holds the interned id as a plain integer
// and a pointer to a byte the registry in `Log.cpp` owns, and reads that byte
// with the compiler's own atomic builtin. Measured either side of the change,
// with the flags `compile_commands.json` gives for `Log.cpp`: **22,842 before,
// 23,016 after** - and all 174 of those are declarations in this file rather
// than a header that arrived with them. `Clock.hpp` is 422 and is the bar this
// module is measured against; `Assert.hpp`, which includes this, is 23,076.
//
// ## Three questions a log statement asks, in this order
//
//     ENGINE_WARN("shader '{}': {}", name.Text(), SDL_GetError());
//
// 1. **Is this level compiled in at all?** `ENGINE_LOG_COMPILED_LEVEL` is the
//    floor the build was configured with. Below it the macro expands to a
//    `sizeof` of an unevaluated call, so the arguments are still type-checked
//    and the format string is still verified, and not one instruction is
//    emitted. `release` compiles at `debug`, so the eight `ENGINE_TRACE` sites
//    in the tree cost a release build nothing whatsoever.
// 2. **Is this level on for this category right now?** One relaxed byte load
//    and a compare, inline. A category is one per module by convention and
//    arrives from `ENGINE_LOG_CATEGORY`, which `mono_add_library` defines as
//    the module's own name - so a call site carries a category without a call
//    site ever having been edited to say so.
// 3. Only then are the arguments evaluated and the line formatted.
//
// **Step 2 is why `Enabled` used to exist and go unused.** The macros used to
// evaluate their arguments whether the level was on or not, because guarding
// them would silently stop running any argument that had a side effect. All 711
// call sites were swept before the guard went in - `++`, compound assignment,
// `fetch_add`, `Pop`, `Drain`, `release`, `Metrics::`, and every distinct
// function named inside an argument list - and **none of them had one**. So the
// guard is on, and this paragraph is the record of the check rather than a
// promise to do it later.
//
// ## Configuring it
//
// `engine.log-level` takes a bare level for everything, or a comma-separated
// list of `category=level` overrides, or both:
//
//     --log trace                     everything at trace
//     --log warning,net=trace         warnings, except net
//     ATOMIC_ENGINE_LOG_LEVEL=render=debug,physics=off
//
// which is `core::Flags` doing the work: the same setting reaches a config
// file, the environment and the command line with the precedence they always
// have. `Log::SetLevel` is the runtime form and takes effect on the next
// statement, on every thread, with no lock.
//
// @tier L0 · shared

#include <spdlog/fmt/bundled/base.h>

#include <cstdint>
#include <string_view>

// **Forward declared rather than included**, so that `Logger()` can be offered
// to the one caller that installs a sink without the other two hundred and
// fifty acquiring spdlog's vocabulary. A reference to an incomplete type is
// legal in a declaration; a caller that wants to *use* it includes spdlog
// itself, which `mono.studio/src/Editor.cpp` already does for its own sink.
//
// This assumes `SPDLOG_API` is empty, which it is because spdlog is built
// static here. A shared spdlog would make it `__declspec(dllimport)` on Windows
// and this declaration would disagree with the real one. The build does not
// check that, so it is written down here instead.
namespace spdlog {
	class logger;
}

namespace engine::core {

	// The severity threshold understood by the engine logging facade.
	//
	// **Ordered, and the order is the comparison.** A statement is emitted when
	// its level is at or above its category's floor, which is one unsigned
	// compare rather than a table.
	enum class LogLevel : uint8_t {
		Trace,	 // Diagnostic details, including all higher severities.
		Debug,	 // Development detail a person asked for, and higher.
		Info,	 // Normal runtime events and higher severities.
		Warning, // Recoverable problems and errors.
		Error,	 // Failures only.

		// Nothing at all. A floor only - no statement is written at this level,
		// which is what makes `physics=off` mean what it says.
		Off,
	};

	// A stable, lowercase name for a level, as the settings spell it.
	//
	// @param level The severity.
	// @return A view valid for the lifetime of the process.
	const char *Describe(LogLevel level);

	// Reads a level from the name `Describe` gives it.
	//
	// **Refused rather than defaulted**, because a misspelled level that
	// silently means `info` is a deployment that thinks it turned tracing on
	// and did not. `warn` is accepted beside `warning`; nothing else is.
	//
	// @param      text  The name, in any case.
	// @param[out] level Written only when the name is one.
	// @return `true` when `text` named a level.
	bool LevelFromName(std::string_view text, LogLevel &level);

	// Where a log statement is written, for the line it produces.
	//
	// **`__FILE__` and `__LINE__` rather than `std::source_location`**, which
	// would be correct and costs 3,000 preprocessed lines in a header with a
	// budget. The macros fill this in; nothing else should need to.
	struct LogSite {
		// The full path the compiler was given. Trimmed to its last two
		// components when a line is written, not here - a suppressed statement
		// must not pay for a string operation.
		const char *File = "";

		// `__func__` at the call site.
		const char *Function = "";

		// The line within `File`.
		uint32_t Line = 0;
	};

	// A named area of the engine, and the severity floor currently set on it.
	//
	// **A category is a `core::Name`**, which is rule 4 at the bottom of the
	// stack: the string is what crosses a config file, an environment variable
	// and a command line, and the id is an optimisation that never leaves the
	// process. `Name.hpp` is not included here for the reason the file comment
	// gives, so this holds the id as a plain integer and asks `Log.cpp` for the
	// text.
	//
	// Registration is what a construction from text does, and the registry
	// never removes an entry - so the pointer below stays valid for the life of
	// the process and `Enabled` is a load rather than a lookup. Construct one
	// in a `static` beside the call site, which is what the macros do.
	//
	// @threadsafe
	class LogCategory {
	  public:
		// Creates a category that names nothing and enables nothing.
		constexpr LogCategory() = default;

		// Registers `name`, or finds the registration it already has.
		//
		// @param name The category, lowercase and one word by convention.
		explicit LogCategory(std::string_view name);

		// Whether a statement at `level` in this category would be written.
		//
		// One relaxed byte load and a compare. This is the guard on every log
		// statement in the engine, so it is inline and it does not lock.
		//
		// @param level The severity of the statement.
		// @return `true` when the statement should be evaluated and emitted.
		bool Enabled(LogLevel level) const {
			return Threshold != nullptr && static_cast<uint8_t>(level) >= Load(Threshold);
		}

		// The registered name, or empty for a default-constructed category.
		std::string_view Text() const;

		// The `core::Name` id the text interned to, or `0xFFFFFFFF`.
		//
		// @return The process-local id. Do not serialize it; serialize `Text`.
		constexpr uint32_t Id() const {
			return Identifier;
		}

		// Whether this object names a registered category.
		constexpr bool IsValid() const {
			return Threshold != nullptr;
		}

	  private:
		// Reads the floor without a data race and without `<atomic>` here.
		//
		// **The builtin rather than `std::atomic_ref`**, because `<atomic>` is
		// 7,452 preprocessed lines and this header has a budget the file
		// comment states. `Log.cpp` writes the same byte with the matching
		// store, so the pair is a relaxed atomic access on both sides even
		// though neither side names the type. Every compiler this repository
		// builds with - GCC, Clang and the mingw-w64 GCC the Windows cross
		// preset uses - has it; the fallback is there so an unknown compiler
		// produces a working log rather than a compile error.
		static uint8_t Load(const unsigned char *slot) {
#if defined(__GNUC__) || defined(__clang__)
			return __atomic_load_n(slot, __ATOMIC_RELAXED);
#else
			return *static_cast<const volatile unsigned char *>(slot);
#endif
		}

		// Into the registry `Log.cpp` owns, which never moves and never removes.
		const unsigned char *Threshold = nullptr;

		uint32_t Identifier = 0xFFFFFFFFu;
	};

	// A rate limiter for one log statement.
	//
	// **Because a fault that reproduces every frame produces sixty lines a
	// second, and the sixty-first tells nobody anything the first did not.**
	// The suppressed lines are counted rather than discarded silently, and the
	// next line that is written says how many it stands for.
	//
	// Every member has a constant initialiser, so a `static` one costs no guard
	// variable and no thread-safe-initialisation check at the call site. Two
	// threads racing on one of these may let a second line through; that is
	// cheaper than a lock on a path whose whole job is to do nothing.
	//
	// @threadsafe
	class LogThrottle {
	  public:
		// Creates a throttle that will let the next statement through.
		constexpr LogThrottle() = default;

		// Whether to write now, and what the line would stand for.
		//
		// @param seconds The minimum gap between two written lines.
		// @return Zero to stay quiet. Otherwise one plus the number of
		//         statements suppressed since the last written line.
		uint64_t Due(double seconds);

	  private:
		// `Clock::Nanoseconds()` of the earliest next line. Zero until the
		// first, which is what makes the first line always pass.
		uint64_t NextNanoseconds = 0;

		// Statements refused since the last one that was written.
		uint64_t Suppressed = 0;
	};

	// Owns the process-wide spdlog logger used by engine programs and scripts.
	class Log {
	  public:
		// Idempotent. Called by every program's main before anything else, and
		// by the test main, so that a log line during static initialisation
		// cannot reach an unconfigured sink.
		static void Initialise(std::string_view program);

		// Sets the floor for every category, and for categories not yet seen.
		//
		// **Every category, including ones already overridden.** A person
		// typing `--log trace` after a config file said `net=warning` means
		// trace; anything else is a setting that appears to do nothing.
		//
		// @param level The lowest severity that will be written.
		static void SetLevel(LogLevel level);

		// The floor a category with no override of its own uses.
		static LogLevel Level();

		// Sets the floor for one category, registering it if it is new.
		//
		// @param category The category name.
		// @param level    The lowest severity that category will write.
		static void SetLevel(std::string_view category, LogLevel level);

		// The floor currently in force for one category.
		//
		// @param category The category name.
		// @return Its floor, or the default for a category nothing has set.
		static LogLevel LevelOf(std::string_view category);

		// Applies a settings string: a bare level, `category=level` pairs, or both.
		//
		//     trace
		//     warning,net=trace,physics=off
		//
		// Empty is a no-op that succeeds, so an unset flag changes nothing.
		//
		// @param      specification The setting, as somebody typed it.
		// @param[out] unknown       The first term that named no level, as a
		//                           view into `specification`. Written only
		//                           when the return is `false`, so that the
		//                           caller composes the message it wants
		//                           without this header acquiring `<string>`.
		// @return `false` when a term named no level, **having applied
		//         nothing**. A misspelling is a deployment that thinks it
		//         turned tracing on and did not, and a half-applied setting is
		//         harder to notice than one that plainly did not take.
		static bool Configure(std::string_view specification, std::string_view *unknown);

		// The floor this build was compiled with.
		//
		// Statements below it were removed by the preprocessor and cannot be
		// switched on at runtime, which is what makes them free.
		static LogLevel CompiledFloor();

		// How many categories have been registered.
		//
		// **A count and an index rather than a container**, so that listing
		// them needs neither `<vector>` nor `<string>` in this header. The
		// registry never removes an entry, so an index stays meaningful.
		static uint32_t CategoryCount();

		// The category at `index`, or an invalid one past the end.
		//
		// @param index Below `CategoryCount()`.
		// @return The category.
		static LogCategory CategoryAt(uint32_t index);

		// Whether a message at this level in this category would be emitted.
		//
		// The by-name form, for a caller holding a string rather than a
		// registered category. It registers the category if it is new, so
		// prefer `LogCategory::Enabled` on any path that repeats.
		//
		// @param level    The severity to test.
		// @param category The category name.
		// @return `true` when a message would reach a sink.
		static bool Enabled(LogLevel level, std::string_view category);

		// Whether source location is appended to the lines that carry it.
		//
		// **Off for `Info` whatever this says, and that is not an
		// inconsistency.** `Info` is the level a person reads as prose and the
		// level `just stress` scrapes, so a suffix on it would break a reader
		// and a script at once. Everything else is a diagnostic and says where
		// it came from.
		//
		// @param shown `false` to drop `file:line` from every line.
		static void SetSourceLocationShown(bool shown);

		// Formats `arguments` into `format` and emits the result.
		//
		// Takes the arguments already erased, which is what keeps the formatter
		// itself out of this header. Prefer the macros, or `Emit`.
		//
		// @param level      The severity.
		// @param category   The area, which the line names.
		// @param site       Where the statement is written.
		// @param suppressed Statements this line stands for, beyond itself.
		// @param format     A format string, already checked at the call site.
		// @param arguments  The packed arguments.
		static void Write(
			LogLevel level,
			const LogCategory &category,
			const LogSite &site,
			uint64_t suppressed,
			fmt::string_view format,
			fmt::format_args arguments
		);

		// Writes at `Error` whatever any level is set to.
		//
		// **For the assert path and nothing else.** An invariant that failed is
		// not something a log setting may hide, and `Assert.hpp` is the only
		// caller. It goes through the same sink as everything else, which is
		// what stops an assert on a job worker interleaving with a frame's
		// ordinary output.
		//
		// @param site      Where the assert is written.
		// @param format    A format string, already checked at the call site.
		// @param arguments The packed arguments.
		static void WriteUnfiltered(const LogSite &site, fmt::string_view format, fmt::format_args arguments);

		// Empties the sink's buffers. Called before the process aborts.
		static void Flush();

		// Packs `values` and hands them to `Write`.
		//
		// `values` are named parameters and therefore lvalues, which is what
		// `fmt::make_format_args` binds to - forwarding them instead would hand
		// it rvalues and fmt refuses those, because the pack it builds holds
		// references and a temporary would dangle before `Write` ran.
		//
		// @param level    The severity.
		// @param category The area, which the line names.
		// @param site     Where the statement is written.
		// @param format   The format string, checked against `Ts` at compile time.
		// @param values   What it formats.
		template <class... Ts>
		static void Emit(
			LogLevel level,
			const LogCategory &category,
			const LogSite &site,
			fmt::format_string<Ts...> format,
			Ts &&...values
		) {
			Write(level, category, site, 0, format, fmt::make_format_args(values...));
		}

		// `Emit`, for a line that stands for others a throttle suppressed.
		//
		// @param level      The severity.
		// @param category   The area, which the line names.
		// @param site       Where the statement is written.
		// @param suppressed Statements this line stands for, beyond itself.
		// @param format     The format string, checked against `Ts`.
		// @param values     What it formats.
		template <class... Ts>
		static void EmitThrottled(
			LogLevel level,
			const LogCategory &category,
			const LogSite &site,
			uint64_t suppressed,
			fmt::format_string<Ts...> format,
			Ts &&...values
		) {
			Write(level, category, site, suppressed, format, fmt::make_format_args(values...));
		}

		// Type-checks a statement the compiled floor removed, and emits nothing.
		//
		// **Named inside `sizeof`, so it is never called and never generates an
		// instruction** - and the arguments are still odr-checked, the format
		// string is still verified against them by fmt's `consteval`
		// constructor, and a variable used only by a compiled-out statement is
		// still used. A macro that expanded to nothing at all would lose all
		// three, and the loss would surface as a `release` build failing on
		// warnings that `dev` never saw.
		//
		// @param format The format string.
		// @param values What it would have formatted.
		// @return Zero, to nothing, ever.
		template <class... Ts> static constexpr int Ignore(fmt::format_string<Ts...> format, Ts &&...values) {
			(void)format;
			((void)values, ...);
			return 0;
		}

		// The process-wide logger, lazily initialising it as `engine` if needed.
		//
		// **For installing a sink, and for nothing else.** Every other use is a
		// macro below. A caller has to include spdlog itself to do anything
		// with the result, which is deliberate: it keeps the cost with the one
		// translation unit that wants it.
		//
		// @return The logger.
		static spdlog::logger &Logger();
	};
}

// The compiled floor, as a number the preprocessor can compare.
//
// Spelled as macros rather than as the enum, because `#if` cannot see a C++
// enumerator. The two orders are the same and `Log.cpp` static_asserts that
// they stay that way.
//@{
#define ENGINE_LOG_LEVEL_TRACE 0
#define ENGINE_LOG_LEVEL_DEBUG 1
#define ENGINE_LOG_LEVEL_INFO 2
#define ENGINE_LOG_LEVEL_WARNING 3
#define ENGINE_LOG_LEVEL_ERROR 4
#define ENGINE_LOG_LEVEL_OFF 5
//@}

// The lowest severity this build compiles in at all.
//
// Set by the build as a public define on `Engine::core`, so every translation
// unit in a program agrees. `trace` everywhere except `release` and `bench`,
// which compile at `debug`.
#ifndef ENGINE_LOG_COMPILED_LEVEL
#define ENGINE_LOG_COMPILED_LEVEL ENGINE_LOG_LEVEL_TRACE
#endif

// The category a translation unit logs under.
//
// **Defined by `mono_add_library` as the module's own name**, which is how 711
// call sites gained a category without one of them being edited. A file that
// logs on behalf of another area passes one explicitly to `ENGINE_LOG`.
//
// The fallback matters for a translation unit the engine build does not compile
// - a game's own code, a scratch program - which gets `engine` rather than a
// compile error.
#ifndef ENGINE_LOG_CATEGORY
#define ENGINE_LOG_CATEGORY "engine"
#endif

// Where the statement is, for the line it writes.
#define ENGINE_LOG_SITE                                                                                      \
	::engine::core::LogSite {                                                                                \
		__FILE__, __func__, static_cast<::std::uint32_t>(__LINE__)                                           \
	}

// Writes one line at `level` in `category`, evaluating nothing when it is off.
//
// The general form. `ENGINE_INFO` and its siblings are this with the
// translation unit's own category, and are what a call site normally writes.
// Reach for this one when a file logs on behalf of another area.
//
// The category is registered once per call site into a function-local `static`,
// so the steady-state cost of a suppressed statement is a guard check, a byte
// load and a compare.
//
// **Not compile-time filtered, and it cannot be.** `level` is an expression
// here, and the preprocessor cannot see one - so a statement written this way
// is present in every build and filtered at runtime. That is the cost of the
// general form, and it is why the named macros are the ones to reach for.
#define ENGINE_LOG(level, category, ...)                                                                     \
	do {                                                                                                     \
		static const ::engine::core::LogCategory engineLogCategory(category);                                \
		if (engineLogCategory.Enabled(level)) {                                                              \
			::engine::core::Log::Emit(level, engineLogCategory, ENGINE_LOG_SITE, __VA_ARGS__);               \
		}                                                                                                    \
	} while (false)

// `ENGINE_LOG`, at most once every `seconds`, counting what it suppressed.
#define ENGINE_LOG_EVERY(seconds, level, category, ...)                                                      \
	do {                                                                                                     \
		static const ::engine::core::LogCategory engineLogCategory(category);                                \
		if (engineLogCategory.Enabled(level)) {                                                              \
			static ::engine::core::LogThrottle engineLogThrottle;                                            \
			if (const auto engineLogDue = engineLogThrottle.Due(seconds); engineLogDue != 0) {               \
				::engine::core::Log::EmitThrottled(                                                          \
					level, engineLogCategory, ENGINE_LOG_SITE, engineLogDue - 1, __VA_ARGS__                 \
				);                                                                                           \
			}                                                                                                \
		}                                                                                                    \
	} while (false)

// A statement below the compiled floor: type-checked, and not emitted.
#define ENGINE_LOG_OMITTED(...) ((void)sizeof(::engine::core::Log::Ignore(__VA_ARGS__)))

// Writes a trace-level message under this translation unit's category.
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_TRACE
#define ENGINE_TRACE(...) ENGINE_LOG(::engine::core::LogLevel::Trace, ENGINE_LOG_CATEGORY, __VA_ARGS__)
// Writes a trace-level message at most once every `seconds`.
#define ENGINE_TRACE_EVERY(seconds, ...)                                                                     \
	ENGINE_LOG_EVERY(seconds, ::engine::core::LogLevel::Trace, ENGINE_LOG_CATEGORY, __VA_ARGS__)
#else
#define ENGINE_TRACE(...) ENGINE_LOG_OMITTED(__VA_ARGS__)
// Writes a trace-level message at most once every `seconds`.
#define ENGINE_TRACE_EVERY(seconds, ...) ((void)(seconds), ENGINE_LOG_OMITTED(__VA_ARGS__))
#endif

// Writes a debug-level message under this translation unit's category.
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_DEBUG
#define ENGINE_DEBUG(...) ENGINE_LOG(::engine::core::LogLevel::Debug, ENGINE_LOG_CATEGORY, __VA_ARGS__)
// Writes a debug-level message at most once every `seconds`.
#define ENGINE_DEBUG_EVERY(seconds, ...)                                                                     \
	ENGINE_LOG_EVERY(seconds, ::engine::core::LogLevel::Debug, ENGINE_LOG_CATEGORY, __VA_ARGS__)
#else
#define ENGINE_DEBUG(...) ENGINE_LOG_OMITTED(__VA_ARGS__)
// Writes a debug-level message at most once every `seconds`.
#define ENGINE_DEBUG_EVERY(seconds, ...) ((void)(seconds), ENGINE_LOG_OMITTED(__VA_ARGS__))
#endif

// Writes an informational message under this translation unit's category.
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_INFO
#define ENGINE_INFO(...) ENGINE_LOG(::engine::core::LogLevel::Info, ENGINE_LOG_CATEGORY, __VA_ARGS__)
// Writes an informational message at most once every `seconds`.
#define ENGINE_INFO_EVERY(seconds, ...)                                                                      \
	ENGINE_LOG_EVERY(seconds, ::engine::core::LogLevel::Info, ENGINE_LOG_CATEGORY, __VA_ARGS__)
#else
#define ENGINE_INFO(...) ENGINE_LOG_OMITTED(__VA_ARGS__)
// Writes an informational message at most once every `seconds`.
#define ENGINE_INFO_EVERY(seconds, ...) ((void)(seconds), ENGINE_LOG_OMITTED(__VA_ARGS__))
#endif

// Writes a warning under this translation unit's category.
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_WARNING
#define ENGINE_WARN(...) ENGINE_LOG(::engine::core::LogLevel::Warning, ENGINE_LOG_CATEGORY, __VA_ARGS__)
// Writes a warning at most once every `seconds`.
#define ENGINE_WARN_EVERY(seconds, ...)                                                                      \
	ENGINE_LOG_EVERY(seconds, ::engine::core::LogLevel::Warning, ENGINE_LOG_CATEGORY, __VA_ARGS__)
#else
#define ENGINE_WARN(...) ENGINE_LOG_OMITTED(__VA_ARGS__)
// Writes a warning at most once every `seconds`.
#define ENGINE_WARN_EVERY(seconds, ...) ((void)(seconds), ENGINE_LOG_OMITTED(__VA_ARGS__))
#endif

// Writes an error under this translation unit's category.
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_ERROR
#define ENGINE_ERROR(...) ENGINE_LOG(::engine::core::LogLevel::Error, ENGINE_LOG_CATEGORY, __VA_ARGS__)
// Writes an error at most once every `seconds`.
#define ENGINE_ERROR_EVERY(seconds, ...)                                                                     \
	ENGINE_LOG_EVERY(seconds, ::engine::core::LogLevel::Error, ENGINE_LOG_CATEGORY, __VA_ARGS__)
#else
#define ENGINE_ERROR(...) ENGINE_LOG_OMITTED(__VA_ARGS__)
// Writes an error at most once every `seconds`.
#define ENGINE_ERROR_EVERY(seconds, ...) ((void)(seconds), ENGINE_LOG_OMITTED(__VA_ARGS__))
#endif
