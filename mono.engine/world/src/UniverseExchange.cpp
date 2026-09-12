#include "BusRouter.hpp"
#include "UniverseProfiling.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace engine::world {
	namespace {
		struct TimingSnapshot {
			std::vector<ecs::Scheduler::Timing> Rows;
			float WorldMilliseconds = 0.0f;
		};

		std::vector<TimingSnapshot> SnapshotTimings(std::span<World *const> worlds) {
			std::vector<TimingSnapshot> snapshots;
			snapshots.reserve(worlds.size());
			for (World *world : worlds) {
				snapshots.push_back(
					world == nullptr
						? TimingSnapshot{}
						: TimingSnapshot{world->Systems().Timings(), world->Statistics().LastTickMilliseconds}
				);
			}
			return snapshots;
		}

		std::vector<ecs::Scheduler::Timing> TimingDelta(
			std::span<const ecs::Scheduler::Timing> current,
			std::span<const ecs::Scheduler::Timing> before,
			bool reset
		) {
			std::vector<ecs::Scheduler::Timing> delta;
			delta.reserve(current.size());
			for (const ecs::Scheduler::Timing &timing : current) {
				float previous = 0.0f;
				if (!reset) {
					const auto found = std::find_if(
						before.begin(), before.end(), [&timing](const ecs::Scheduler::Timing &candidate) {
							return candidate.Name == timing.Name && candidate.RunPhase == timing.RunPhase;
						}
					);
					if (found != before.end()) {
						previous = found->Milliseconds;
					}
				}
				if (reset || timing.Milliseconds > previous) {
					delta.push_back({timing.Name, timing.RunPhase, timing.Milliseconds - previous});
				}
			}
			return delta;
		}

		void ReportExchangeWorkerTimings(
			std::span<World *const> worlds,
			std::span<const uint8_t> participants,
			const std::vector<TimingSnapshot> &before,
			bool reset,
			float workerMilliseconds
		) {
			core::FrameGraph::ReportedScope workers(
				"exchange worlds (pinned workers)", core::ProfileCategory::ECS, workerMilliseconds
			);
			for (size_t at = 0; at < worlds.size(); ++at) {
				World *world = worlds[at];
				if (world == nullptr || at >= participants.size() || !participants[at]) {
					continue;
				}

				const std::span<const ecs::Scheduler::Timing> prior =
					at < before.size() ? std::span<const ecs::Scheduler::Timing>(before[at].Rows)
									   : std::span<const ecs::Scheduler::Timing>{};
				const std::vector<ecs::Scheduler::Timing> delta =
					TimingDelta(world->Systems().Timings(), prior, reset);
				if (delta.empty()) {
					continue;
				}
				const float beforeWorld = reset || at >= before.size() ? 0.0f : before[at].WorldMilliseconds;
				const float worldMilliseconds =
					std::max(world->Statistics().LastTickMilliseconds - beforeWorld, 0.0f);
				ReportWorkerSchedulerTimings(*world, delta, worldMilliseconds);
			}
		}

		void ReportExchangeWorkerBatch(float milliseconds) {
			core::FrameGraph::Report(
				"exchange worlds (pinned workers)", core::ProfileCategory::Simulation, milliseconds
			);
		}

		auto Key(const TickExchangeStamp &stamp) {
			return std::tie(stamp.SourceWorld, stamp.Channel, stamp.SourceTick, stamp.Sequence);
		}
		template <class Message> bool Bounded(std::span<const Message> messages) {
			if (messages.size() > MAXIMUM_TICK_EXCHANGE_MESSAGES) return false;
			size_t bytes = 0;
			for (const auto &message : messages) {
				core::ByteWriter encoded;
				if (!WriteTickExchange(encoded, message) ||
					encoded.Size() > MAXIMUM_TICK_EXCHANGE_BYTES - bytes)
					return false;
				bytes += encoded.Size();
			}
			return true;
		}
	}

	bool Universe::HasTickExchangeEndpoints() const {
		RequireDriverThread("HasTickExchangeEndpoints");
		for (size_t at = 0; at < Registry.size(); ++at) {
			if (Registry[at] && !IsRemote(WorldId(static_cast<uint32_t>(at))) &&
				HasTickExchanges(Registry[at]->Storage()))
				return true;
		}
		return false;
	}
	bool Universe::TickExchangeFrameOpen() const {
		return ExchangeStage != ExchangePhase::Closed;
	}
	int Universe::BeginTickExchangeFrame(float frameSeconds) {
		RequireDriverThread("BeginTickExchangeFrame");
		ENGINE_PROFILE("begin tick exchange frame");
		if (Ticking || !std::isfinite(frameSeconds) || frameSeconds < 0) return -1;
		ExchangeStarted = core::Clock::Nanoseconds();
		DrainControls();
		const auto barrier = Router->Route(WorldDirectory{Registry, Hosts}, Settings_);
		Stats.BusOperations = barrier.BusOperations;
		Stats.Deliveries = barrier.Deliveries;
		RefreshLanes(parallel::Jobs::PinnedWorkerCount());
		ActiveList.clear();
		ActiveLanes.clear();
		OwedList.clear();
		ExchangeRound = 0;
		ExchangeRounds = 0;
		// Passive local worlds remain serveable at their last committed tick.
		// They incur no Input, integration or clock advance merely for answering.
		for (size_t at = 0; at < Registry.size(); ++at) {
			if (!Registry[at] || IsRemote(WorldId(static_cast<uint32_t>(at)))) continue;
			const int owed = Registry[at]->Owed(frameSeconds, Settings_.MaximumCatchUpTicks);
			ActiveList.push_back(Registry[at].get());
			ActiveLanes.push_back(LaneByWorld[at]);
			OwedList.push_back(owed);
			ExchangeRounds = std::max(ExchangeRounds, static_cast<unsigned>(owed));
		}
		ExchangeParticipants.assign(ActiveList.size(), 0);
		ExchangeRequests.clear();
		Ticking = true;
		ExchangeStage = ExchangePhase::BetweenRounds;
		return static_cast<int>(ExchangeRounds);
	}
	bool Universe::DispatchExchangeWorlds(const std::function<void(size_t)> &body) {
		float estimated = 0;
		for (const auto *world : ActiveList)
			estimated += world->Statistics().LastTickMilliseconds;
		const bool workers = Settings_.Mode == ExecutionMode::WorldParallel &&
							 !parallel::ForceSerialCompute() && LaneCount && ActiveList.size() > 1 &&
							 estimated >= Settings_.WorldParallelFloorMilliseconds;
		const auto run = [&](size_t begin, size_t end) {
			for (size_t at = begin; at < end; ++at) {
				ActiveList[at]->Storage().BindToCallingThread();
				body(at);
			}
		};
		if (workers) {
			parallel::Jobs::ForWorkers(ActiveLanes, run);
			return true;
		} else {
			run(0, ActiveList.size());
			return false;
		}
	}
	bool Universe::BeginTickExchangeRound() {
		RequireDriverThread("BeginTickExchangeRound");
		ENGINE_PROFILE("tick exchange input");
		if (ExchangeStage != ExchangePhase::BetweenRounds) return false;
		ExchangeRequests.clear();
		std::vector<uint8_t> success(ActiveList.size(), 1);
		const bool profileWorkers = core::FrameGraph::IsEnabled();
		const std::vector<TimingSnapshot> before =
			profileWorkers ? SnapshotTimings(ActiveList) : std::vector<TimingSnapshot>{};
		const bool workers = DispatchExchangeWorlds([&](size_t at) {
			const bool participant =
				static_cast<unsigned>(OwedList[at]) > ExchangeRound && Ticks(ActiveList[at]->State());
			ExchangeParticipants[at] = participant;
			if (participant) success[at] = ActiveList[at]->BeginExchangeRound(ExchangeRound == 0);
		});
		ExchangeStage = ExchangePhase::Input;
		if (std::find(success.begin(), success.end(), 0) != success.end()) {
			CancelTickExchangeFrame();
			return false;
		}
		if (workers && profileWorkers) {
			ReportExchangeWorkerTimings(
				ActiveList,
				ExchangeParticipants,
				before,
				ExchangeRound == 0,
				parallel::Jobs::LastBatch().BusyMilliseconds
			);
		}
		return true;
	}
	bool Universe::CollectTickExchangeRequests(std::vector<TickExchangeRequest> &out) {
		RequireDriverThread("CollectTickExchangeRequests");
		if (ExchangeStage != ExchangePhase::Input) return false;
		std::vector<std::vector<TickExchangeRequest>> collected(ActiveList.size());
		std::vector<uint8_t> success(ActiveList.size(), 1);
		const bool workers = DispatchExchangeWorlds([&](size_t at) {
			if (!ExchangeParticipants[at]) return;
			try {
				success[at] = CollectTickExchanges(
					ActiveList[at]->Storage(), ActiveList[at]->Name().Text(), collected[at]
				);
			} catch (...) {
				success[at] = 0;
			}
		});
		if (workers) {
			ReportExchangeWorkerBatch(parallel::Jobs::LastBatch().BusyMilliseconds);
		}
		if (std::find(success.begin(), success.end(), 0) != success.end()) return false;
		std::vector<TickExchangeRequest> requests;
		for (auto &batch : collected) {
			if (batch.size() > MAXIMUM_TICK_EXCHANGE_MESSAGES - requests.size()) return false;
			for (auto &request : batch)
				requests.push_back(std::move(request));
		}
		std::sort(requests.begin(), requests.end(), [](const auto &a, const auto &b) {
			return Key(a.Stamp) < Key(b.Stamp);
		});
		if (!Bounded<TickExchangeRequest>(requests)) return false;
		ExchangeRequests = requests;
		out = std::move(requests);
		ExchangeStage = ExchangePhase::Collected;
		return true;
	}
	bool Universe::ServeTickExchangeRequests(
		std::span<const TickExchangeRequest> requests, std::vector<TickExchangeReply> &out
	) {
		RequireDriverThread("ServeTickExchangeRequests");
		ENGINE_PROFILE("tick exchange destinations");
		if (ExchangeStage != ExchangePhase::Collected || !Bounded(requests)) return false;
		std::vector<TickExchangeReply> replies(requests.size());
		for (size_t at = 0; at < requests.size(); ++at)
			replies[at].Stamp = requests[at].Stamp;
		const bool workers = DispatchExchangeWorlds([&](size_t worldIndex) {
			auto &world = *ActiveList[worldIndex];
			if (world.State() == WorldState::Faulted) return;
			for (size_t at = 0; at < requests.size(); ++at) {
				if (requests[at].Stamp.DestinationWorld != world.Name().Text()) continue;
				try {
					replies[at] = ServeTickExchange(world.Storage(), requests[at]);
				} catch (...) {
					replies[at].Status = TickExchangeStatus::Refused;
				}
			}
		});
		if (workers) {
			ReportExchangeWorkerBatch(parallel::Jobs::LastBatch().BusyMilliseconds);
		}
		if (!Bounded<TickExchangeReply>(replies)) return false;
		out = std::move(replies);
		return true;
	}
	bool Universe::ApplyTickExchangeReplies(std::span<const TickExchangeReply> replies) {
		RequireDriverThread("ApplyTickExchangeReplies");
		ENGINE_PROFILE("tick exchange sources");
		if (ExchangeStage != ExchangePhase::Collected || replies.size() != ExchangeRequests.size() ||
			!Bounded(replies))
			return false;
		std::vector<TickExchangeReply> ordered(replies.begin(), replies.end());
		std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
			return Key(a.Stamp) < Key(b.Stamp);
		});
		for (size_t at = 0; at < ordered.size(); ++at) {
			if (!(ordered[at].Stamp == ExchangeRequests[at].Stamp) ||
				(ordered[at].Status == TickExchangeStatus::Complete && !ordered[at].DestinationIncarnation))
				return false;
		}
		std::vector<uint8_t> success(ActiveList.size(), 1);
		const bool workers = DispatchExchangeWorlds([&](size_t at) {
			if (!ExchangeParticipants[at]) return;
			std::vector<TickExchangeReply> selected;
			for (const auto &reply : ordered)
				if (reply.Stamp.SourceWorld == ActiveList[at]->Name().Text()) selected.push_back(reply);
			try {
				success[at] = ApplyTickExchanges(ActiveList[at]->Storage(), selected);
			} catch (...) {
				success[at] = 0;
			}
		});
		if (workers) {
			ReportExchangeWorkerBatch(parallel::Jobs::LastBatch().BusyMilliseconds);
		}
		if (std::find(success.begin(), success.end(), 0) != success.end()) return false;
		ExchangeStage = ExchangePhase::Applied;
		return true;
	}
	bool Universe::FinishTickExchangeRound() {
		RequireDriverThread("FinishTickExchangeRound");
		ENGINE_PROFILE("tick exchange integration");
		if (ExchangeStage != ExchangePhase::Applied) return false;
		const bool profileWorkers = core::FrameGraph::IsEnabled();
		const std::vector<TimingSnapshot> before =
			profileWorkers ? SnapshotTimings(ActiveList) : std::vector<TimingSnapshot>{};
		const bool workers = DispatchExchangeWorlds([&](size_t at) {
			if (ExchangeParticipants[at]) (void)ActiveList[at]->FinishExchangeRound();
		});
		if (workers && profileWorkers) {
			ReportExchangeWorkerTimings(
				ActiveList, ExchangeParticipants, before, false, parallel::Jobs::LastBatch().BusyMilliseconds
			);
		}
		std::fill(ExchangeParticipants.begin(), ExchangeParticipants.end(), 0);
		ExchangeRequests.clear();
		++ExchangeRound;
		ExchangeStage = ExchangePhase::BetweenRounds;
		return true;
	}
	void Universe::CompleteExchangeFrame() {
		Ticking = false;
		ExchangeStage = ExchangePhase::Closed;
		ExchangeRequests.clear();
		Stats.ActiveWorlds = static_cast<size_t>(
			std::count_if(OwedList.begin(), OwedList.end(), [](int owed) { return owed > 0; })
		);
		Stats.Suspended = Stats.Faulted = Stats.Remote = Stats.SimulationTicks = 0;
		DrainControls();
		for (const auto &world : Registry) {
			if (!world) continue;
			Stats.Suspended += world->State() == WorldState::Suspended;
			Stats.Faulted += world->State() == WorldState::Faulted;
			Stats.Remote += world->State() == WorldState::Remote;
			Stats.SimulationTicks += world->Statistics().Ticks;
		}
		Stats.LastTickMilliseconds =
			static_cast<float>(core::Clock::Nanoseconds() - ExchangeStarted) / 1'000'000;
	}
	bool Universe::EndTickExchangeFrame() {
		RequireDriverThread("EndTickExchangeFrame");
		if (ExchangeStage != ExchangePhase::BetweenRounds || ExchangeRound < ExchangeRounds) return false;
		CompleteExchangeFrame();
		return true;
	}
	void Universe::CancelTickExchangeFrame() {
		RequireDriverThread("CancelTickExchangeFrame");
		if (ExchangeStage == ExchangePhase::Closed) return;
		ENGINE_ERROR("universe: cancelling incomplete phase exchange before physics integration");
		const bool workers = DispatchExchangeWorlds([&](size_t at) {
			if (ExchangeParticipants[at] && ActiveList[at]->State() != WorldState::Faulted)
				ActiveList[at]->CancelExchangeRound();
		});
		if (workers) {
			ReportExchangeWorkerBatch(parallel::Jobs::LastBatch().BusyMilliseconds);
		}
		CompleteExchangeFrame();
	}
}
