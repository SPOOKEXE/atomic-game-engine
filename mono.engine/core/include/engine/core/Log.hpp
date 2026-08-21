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
	enum class LogLevel : uint8_t {
		Trace,	 // Diagnostic details, including all higher severities.
		Info,	 // Normal runtime events and higher severities.
		Warning, // Recoverable problems and errors.
		Error,	 // Failures only.
	};

	// Owns the process-wide spdlog logger used by engine programs and scripts.
	class Log {
	  public:
		// Idempotent. Called by every program's main before anything else, and
		// by the test main, so that a log line during static initialisation
		// cannot reach an unconfigured sink.
		static void Initialise(std::string_view program);

		// Sets the minimum severity emitted by the process-wide logger.
		static void SetLevel(LogLevel level);

		// Whether a message at this level would be emitted at all.
		//
		// **Offered so that a caller with something expensive to say can ask
		// first.** The macros below do not use it: they evaluate their
		// arguments whether the level is on or not, exactly as they did when
		// they called spdlog directly, because changing that would silently
		// stop running any argument that has a side effect. Guarding them is a
		// one-line change and a deliberate one, not a side effect of this.
		//
		// @param level The severity to test.
		// @return `true` when a message at `level` would reach a sink.
		static bool Enabled(LogLevel level);

		// Formats `arguments` into `format` and emits the result at `level`.
		//
		// Takes the arguments already erased, which is what keeps the formatter
		// itself out of this header. Prefer the macros, or `Emit`.
		//
		// @param level     The severity.
		// @param format    A format string, already checked at the call site.
		// @param arguments The packed arguments.
		static void Write(LogLevel level, fmt::string_view format, fmt::format_args arguments);

		// Packs `values` and hands them to `Write`.
		//
		// `values` are named parameters and therefore lvalues, which is what
		// `fmt::make_format_args` binds to - forwarding them instead would hand
		// it rvalues and fmt refuses those, because the pack it builds holds
		// references and a temporary would dangle before `Write` ran.
		//
		// @param level  The severity.
		// @param format The format string, checked against `Ts` at compile time.
		// @param values What it formats.
		template <class... Ts>
		static void Emit(LogLevel level, fmt::format_string<Ts...> format, Ts &&...values) {
			Write(level, format, fmt::make_format_args(values...));
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

// Writes a trace-level message through the engine logger.
#define ENGINE_TRACE(...) ::engine::core::Log::Emit(::engine::core::LogLevel::Trace, __VA_ARGS__)
// Writes an informational message through the engine logger.
#define ENGINE_INFO(...) ::engine::core::Log::Emit(::engine::core::LogLevel::Info, __VA_ARGS__)
// Writes a warning through the engine logger.
#define ENGINE_WARN(...) ::engine::core::Log::Emit(::engine::core::LogLevel::Warning, __VA_ARGS__)
// Writes an error through the engine logger.
#define ENGINE_ERROR(...) ::engine::core::Log::Emit(::engine::core::LogLevel::Error, __VA_ARGS__)
