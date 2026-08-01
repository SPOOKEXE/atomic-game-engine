#pragma once

// The engine log. Also the destination for the userland globals `print`,
// `warn`, `error` and `assert`, which is why the levels below are named after
// what a script author sees rather than after spdlog's set.
//
// @tier L0 · shared

#include <spdlog/spdlog.h>

#include <string_view>

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

		// Returns the process-wide logger, lazily initialising it as `engine` if needed.
		static spdlog::logger &Logger();
	};
}

// Writes a trace-level message through the engine logger.
#define ENGINE_TRACE(...) ::engine::core::Log::Logger().trace(__VA_ARGS__)
// Writes an informational message through the engine logger.
#define ENGINE_INFO(...) ::engine::core::Log::Logger().info(__VA_ARGS__)
// Writes a warning through the engine logger.
#define ENGINE_WARN(...) ::engine::core::Log::Logger().warn(__VA_ARGS__)
// Writes an error through the engine logger.
#define ENGINE_ERROR(...) ::engine::core::Log::Logger().error(__VA_ARGS__)
