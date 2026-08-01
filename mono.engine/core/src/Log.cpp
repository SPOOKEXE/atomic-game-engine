#include <engine/core/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>

namespace engine::core {

	namespace {
		std::shared_ptr<spdlog::logger> &Instance() {
			static std::shared_ptr<spdlog::logger> logger;
			return logger;
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
		auto spdlogLevel = spdlog::level::info;
		switch (level) {
		case LogLevel::Trace:
			spdlogLevel = spdlog::level::trace;
			break;
		case LogLevel::Info:
			spdlogLevel = spdlog::level::info;
			break;
		case LogLevel::Warning:
			spdlogLevel = spdlog::level::warn;
			break;
		case LogLevel::Error:
			spdlogLevel = spdlog::level::err;
			break;
		}
		Logger().set_level(spdlogLevel);
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
