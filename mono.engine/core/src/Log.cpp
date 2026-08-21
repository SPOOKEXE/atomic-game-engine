#include <engine/core/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <iterator>
#include <memory>

namespace engine::core {

	namespace {
		std::shared_ptr<spdlog::logger> &Instance() {
			static std::shared_ptr<spdlog::logger> logger;
			return logger;
		}

		// One mapping, used by `SetLevel`, `Enabled` and `Write` alike. It was
		// a switch inside `SetLevel` when that was the only caller; three
		// copies of a four-way mapping is how two of them end up disagreeing.
		spdlog::level::level_enum Severity(LogLevel level) {
			switch (level) {
			case LogLevel::Trace:
				return spdlog::level::trace;
			case LogLevel::Info:
				return spdlog::level::info;
			case LogLevel::Warning:
				return spdlog::level::warn;
			case LogLevel::Error:
				return spdlog::level::err;
			}
			return spdlog::level::info;
		}
	}

	void Log::Initialise(std::string_view program) {
		auto &logger = Instance();
		if (logger) {
			return;
		}

		logger = spdlog::stdout_color_mt(std::string(program));
		logger->set_pattern("[%T.%e] [%^%l%$] %v");
		logger->set_level(spdlog::level::info);
	}

	void Log::SetLevel(LogLevel level) {
		Logger().set_level(Severity(level));
	}

	bool Log::Enabled(LogLevel level) {
		return Logger().should_log(Severity(level));
	}

	void Log::Write(LogLevel level, fmt::string_view format, fmt::format_args arguments) {
		const spdlog::level::level_enum severity = Severity(level);
		spdlog::logger &logger = Logger();
		if (!logger.should_log(severity)) {
			return;
		}

		// **Into a stack buffer rather than a `std::string`.** `fmt::memory_buffer`
		// carries 500 bytes inline, so the common line costs no allocation at
		// all, and spdlog is handed a view of it rather than a second copy.
		fmt::memory_buffer message;
		fmt::vformat_to(std::back_inserter(message), format, arguments);
		logger.log(severity, spdlog::string_view_t(message.data(), message.size()));
	}

	spdlog::logger &Log::Logger() {
		auto &logger = Instance();
		if (!logger) {
			// A log line before Initialise is a bug, but losing it is worse
			// than the bug. Fall back rather than crash.
			Log::Initialise("engine");
		}
		return *Instance();
	}
}
