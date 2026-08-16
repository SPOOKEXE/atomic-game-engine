#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <chrono>
#include <loadtest/Harness.hpp>
#include <numbers>
#include <optional>
#include <string>
#include <thread>

namespace loadtest {

	namespace {
		// How far apart two clients' headings are.
		//
		// A whole turn spread over the client count, so a hundred clients walk a
		// hundred different ways and the world under them genuinely moves - a
		// world at rest sends nothing, and a load test of a server sending
		// nothing measures the pacing sleep.
		float HeadingOf(uint32_t index, uint32_t clients) {
			const auto turn = static_cast<float>(2.0 * std::numbers::pi);
			return clients == 0 ? 0.0f : turn * static_cast<float>(index) / static_cast<float>(clients);
		}
	}

	Harness::Harness(const Options &options) : Settings(options) {
		Sessions.reserve(Settings.Clients);
	}

	Summary Harness::Run() {
		RegisterReplicaTypes();

		const std::optional<engine::net::Endpoint> server =
			engine::net::Endpoint::Parse(Settings.Address + ":" + std::to_string(Settings.Port));
		if (!server) {
			ENGINE_ERROR("loadtest: '{}:{}' is not an address", Settings.Address, Settings.Port);
			return {};
		}

		if (!Settings.ProfilePath.empty()) {
			engine::core::FrameGraph::SetEnabled(true);
			engine::core::FrameGraph::SetFoldingEnabled(true);
		}

		const double budget = Settings.TickRate > 0.0 ? 1.0 / Settings.TickRate : 1.0 / 30.0;
		const auto budgetNanoseconds = static_cast<uint64_t>(budget * 1e9);

		const uint64_t started = engine::core::Clock::Nanoseconds();
		uint64_t nextTickAt = started;
		uint64_t tick = 0;
		uint32_t opened = 0;

		for (;;) {
			engine::core::FrameGraph::BeginFrame();

			const double nowSeconds = static_cast<double>(engine::core::Clock::Nanoseconds()) / 1e9;

			// Sessions arrive a few per tick rather than all at once. See
			// `Options::ConnectsPerTick` for why that is the honest default.
			{
				ENGINE_PROFILE_CAT("loadtest.dial", engine::core::ProfileCategory::Network);

				for (uint32_t added = 0; added < Settings.ConnectsPerTick && opened < Settings.Clients;
					 added++, opened++) {
					SessionSettings session;
					session.Server = *server;
					session.StallSeconds = Settings.StallSeconds;
					session.InputEveryTicks = Settings.InputEveryTicks;
					session.HeadingRadians = HeadingOf(opened, Settings.Clients);

					auto opening = std::make_unique<Session>(session, nowSeconds);
					if (!opening->Open()) {
						Unopened_++;
						continue;
					}
					Sessions.push_back(std::move(opening));
				}
			}

			{
				ENGINE_PROFILE_CAT("loadtest.sessions", engine::core::ProfileCategory::Network);

				for (const std::unique_ptr<Session> &session : Sessions) {
					session->Tick(nowSeconds, tick);
				}
			}

			engine::core::FrameGraph::EndFrame();

			tick++;

			const uint64_t ended = engine::core::Clock::Nanoseconds();
			const double elapsed = static_cast<double>(ended - started) / 1e9;
			if (Settings.Ticks > 0 && tick >= static_cast<uint64_t>(Settings.Ticks)) {
				break;
			}
			if (Settings.Seconds > 0.0 && elapsed >= Settings.Seconds) {
				break;
			}

			// Paced against an absolute schedule, for the reason
			// `mono.server/AGENTS.md` gives: sleeping for "budget minus spent"
			// accumulates the sleep's own overshoot, and a harness that drifts
			// slow polls less often than it claims to.
			nextTickAt += budgetNanoseconds;
			const uint64_t now = engine::core::Clock::Nanoseconds();
			if (now < nextTickAt) {
				std::this_thread::sleep_for(std::chrono::nanoseconds(nextTickAt - now));
			} else if (now - nextTickAt > budgetNanoseconds * 4) {
				// Far behind. Give up on the missed ticks rather than spiralling,
				// which on this side would mean polling in a tight loop and
				// reporting an apply cost that is really a backlog.
				nextTickAt = now;
			}
		}

		const double seconds = static_cast<double>(engine::core::Clock::Nanoseconds() - started) / 1e9;

		std::vector<SessionReport> reports;
		reports.reserve(Sessions.size());
		for (const std::unique_ptr<Session> &session : Sessions) {
			reports.push_back(session->Report());
		}

		if (!Settings.ProfilePath.empty()) {
			engine::core::FrameGraph::SetFoldingEnabled(false);
			const size_t folded = engine::core::FrameGraph::FoldedFrames();
			if (engine::core::FrameGraph::WriteFolded(Settings.ProfilePath)) {
				ENGINE_INFO("profile: {} frame(s) folded into {}", folded, Settings.ProfilePath.string());
			} else {
				ENGINE_ERROR("profile: nothing to write to '{}'", Settings.ProfilePath.string());
			}
		}

		return Summarise(reports, seconds);
	}
}
