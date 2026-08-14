#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Settings.hpp>

#include <array>

namespace engine::parallel {
	namespace {
		constexpr std::string_view WORKERS = "engine.jobs.workers";
		constexpr std::string_view SERIAL = "engine.serial-compute";

		constexpr std::array<core::FlagDescription, 2> TABLE{{
			{WORKERS,
			 core::FlagKind::Integer,
			 "0",
			 "Worker threads in the process-wide pool, or 0 to choose from the hardware"},
			{SERIAL,
			 core::FlagKind::Boolean,
			 "false",
			 "Run every parallel dispatch on its caller's thread, so a profile keeps every span"},
		}};
	}

	bool DeclareFlags() {
		return core::Flags::Declare(TABLE);
	}

	void ApplyFlags() {
		// Nothing declared these, so this program does not use them — the same
		// direction `assets::ContentPolicy::FromFlags` takes, and for the same
		// reason: a dead flag reads `false`, which here would happen to be right
		// and for the wrong reason.
		if (!core::Flags::Has(SERIAL)) {
			return;
		}

		if (const core::Flag serial(SERIAL); serial.Boolean()) {
			SetForceSerialCompute(true);
			ENGINE_INFO("serial compute forced: every dispatch runs on its caller's thread");
		}
	}

	unsigned ConfiguredWorkers() {
		if (!core::Flags::Has(WORKERS)) {
			return 0;
		}

		const int64_t workers = core::Flag(WORKERS).Integer();
		if (workers < 0) {
			// **Clamped rather than cast**, because `static_cast<unsigned>(-1)`
			// is four billion workers and the failure is a machine that stops
			// responding rather than a message.
			ENGINE_WARN("{} is negative; working the count out instead", WORKERS);
			return 0;
		}
		return static_cast<unsigned>(workers);
	}
}
