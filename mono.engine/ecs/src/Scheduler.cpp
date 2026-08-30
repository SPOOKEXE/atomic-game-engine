#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <stdexcept>

namespace engine::ecs {

	std::string_view GetPhaseName(Phase phase) {
		switch (phase) {
		case Phase::Input:
			return "input";
		case Phase::Simulation:
			return "simulation";
		case Phase::Physics:
			return "physics";
		case Phase::Animation:
			return "animation";
		case Phase::Replication:
			return "replication";
		case Phase::RenderPreparation:
			return "render preparation";
		case Phase::Render:
			return "render";
		case Phase::Count:
			break;
		}
		return "?";
	}

	void Scheduler::Add(std::string_view name, Phase phase, System system, SystemOrder order) {
		Systems.push_back(
			Registered{
				.Name = std::string(name),
				.RunPhase = phase,
				.Body = std::move(system),
				.ParallelBody = {},
				.Order = std::move(order),
			}
		);
		ScheduleDirty = true;

		// Registered systems are stable for the life of the scheduler, so the
		// timing list is sized once and reused. Reallocating it inside Run
		// would show up in the numbers it is reporting.
		LastTimings.reserve(Systems.size());
	}

	void
	Scheduler::AddParallel(std::string_view name, Phase phase, ParallelSystem system, SystemOrder order) {
		Systems.push_back(
			Registered{
				.Name = std::string(name),
				.RunPhase = phase,
				.Body = {},
				.ParallelBody = std::move(system),
				.Order = std::move(order),
				.Mode = Execution::ParallelReadOnly,
			}
		);
		ScheduleDirty = true;
		LastTimings.reserve(Systems.size());
	}

	bool Scheduler::Replace(std::string_view name, uint64_t revision, System system) {
		Registered *matched = nullptr;
		for (Registered &registered : Systems) {
			if (registered.Name != name) {
				continue;
			}
			if (matched != nullptr) {
				return false;
			}
			matched = &registered;
		}
		if (matched == nullptr || matched->Mode != Execution::Serial || revision <= matched->Revision ||
			!system) {
			return false;
		}
		matched->Body = std::move(system);
		matched->Revision = revision;
		return true;
	}

	bool Scheduler::ReplaceParallel(std::string_view name, uint64_t revision, ParallelSystem system) {
		Registered *matched = nullptr;
		for (Registered &registered : Systems) {
			if (registered.Name != name) {
				continue;
			}
			if (matched != nullptr) {
				return false;
			}
			matched = &registered;
		}
		if (matched == nullptr || matched->Mode != Execution::ParallelReadOnly ||
			revision <= matched->Revision || !system) {
			return false;
		}
		matched->ParallelBody = std::move(system);
		matched->Revision = revision;
		return true;
	}

	uint64_t Scheduler::SystemRevision(std::string_view name) const {
		const Registered *matched = nullptr;
		for (const Registered &registered : Systems) {
			if (registered.Name != name) {
				continue;
			}
			if (matched != nullptr) {
				return 0;
			}
			matched = &registered;
		}
		return matched == nullptr ? 0 : matched->Revision;
	}

	bool Scheduler::HasSystem(std::string_view name, Phase phase) const {
		return std::any_of(Systems.begin(), Systems.end(), [name, phase](const Registered &system) {
			return system.Name == name && system.RunPhase == phase;
		});
	}

	SystemScheduleIssue Scheduler::BuildSchedule(Schedule &schedule) const {
		for (auto &phase : schedule) {
			phase.clear();
		}

		for (size_t index = 0; index < Systems.size(); index++) {
			const Registered &system = Systems[index];
			if (system.Name.empty()) {
				return {SystemScheduleStatus::EmptyName, system.Name, {}};
			}
			for (size_t prior = 0; prior < index; prior++) {
				if (Systems[prior].Name == system.Name && Systems[prior].RunPhase == system.RunPhase) {
					return {SystemScheduleStatus::DuplicateName, system.Name, {}};
				}
			}
		}

		std::vector<std::vector<size_t>> outgoing(Systems.size());
		std::vector<size_t> incoming(Systems.size(), 0);
		auto addDependency = [&](size_t before, size_t after) {
			auto &edges = outgoing[before];
			if (std::find(edges.begin(), edges.end(), after) == edges.end()) {
				edges.push_back(after);
				incoming[after]++;
			}
		};

		for (size_t index = 0; index < Systems.size(); index++) {
			const Registered &system = Systems[index];
			auto resolve = [&](const std::string &dependency,
							   bool systemRunsBefore,
							   bool required) -> SystemScheduleIssue {
				size_t matched = Systems.size();
				bool existsInAnotherPhase = false;
				for (size_t candidate = 0; candidate < Systems.size(); candidate++) {
					if (Systems[candidate].Name != dependency) {
						continue;
					}
					if (Systems[candidate].RunPhase == system.RunPhase) {
						matched = candidate;
						break;
					}
					existsInAnotherPhase = true;
				}
				if (matched == Systems.size() && !required) {
					return {};
				}
				if (matched == Systems.size() && !existsInAnotherPhase) {
					return {SystemScheduleStatus::UnknownDependency, system.Name, dependency};
				}
				if (matched == Systems.size()) {
					return {SystemScheduleStatus::CrossPhaseDependency, system.Name, dependency};
				}
				if (systemRunsBefore) {
					addDependency(index, matched);
				} else {
					addDependency(matched, index);
				}
				return {};
			};

			for (const std::string &dependency : system.Order.Before) {
				const SystemScheduleIssue issue = resolve(dependency, true, true);
				if (issue.Status != SystemScheduleStatus::Ready) {
					return issue;
				}
			}
			for (const std::string &dependency : system.Order.After) {
				const SystemScheduleIssue issue = resolve(dependency, false, true);
				if (issue.Status != SystemScheduleStatus::Ready) {
					return issue;
				}
			}
			for (const std::string &dependency : system.Order.BeforeIfPresent) {
				const SystemScheduleIssue issue = resolve(dependency, true, false);
				if (issue.Status != SystemScheduleStatus::Ready) {
					return issue;
				}
			}
			for (const std::string &dependency : system.Order.AfterIfPresent) {
				const SystemScheduleIssue issue = resolve(dependency, false, false);
				if (issue.Status != SystemScheduleStatus::Ready) {
					return issue;
				}
			}
		}

		for (size_t phaseIndex = 0; phaseIndex < static_cast<size_t>(Phase::Count); phaseIndex++) {
			std::vector<bool> emitted(Systems.size(), false);
			size_t remaining = 0;
			for (const Registered &system : Systems) {
				remaining += static_cast<size_t>(system.RunPhase == static_cast<Phase>(phaseIndex));
			}

			while (remaining > 0) {
				Wave wave;
				wave.Parallel = true;
				for (size_t index = 0; index < Systems.size(); index++) {
					const Registered &system = Systems[index];
					if (!emitted[index] && system.RunPhase == static_cast<Phase>(phaseIndex) &&
						incoming[index] == 0) {
						wave.Systems.push_back(index);
						wave.Parallel = wave.Parallel && system.Mode == Execution::ParallelReadOnly;
					}
				}
				if (wave.Systems.empty()) {
					for (size_t index = 0; index < Systems.size(); index++) {
						if (!emitted[index] && Systems[index].RunPhase == static_cast<Phase>(phaseIndex)) {
							return {SystemScheduleStatus::Cycle, Systems[index].Name, {}};
						}
					}
				}

				wave.Parallel = wave.Parallel && wave.Systems.size() > 1;
				for (const size_t index : wave.Systems) {
					emitted[index] = true;
					remaining--;
					for (const size_t dependent : outgoing[index]) {
						incoming[dependent]--;
					}
				}
				schedule[phaseIndex].push_back(std::move(wave));
			}
		}
		return {};
	}

	SystemScheduleIssue Scheduler::Validate() const {
		Schedule schedule;
		return BuildSchedule(schedule);
	}

	void Scheduler::EnsureSchedule() {
		if (!ScheduleDirty) {
			return;
		}
		Schedule candidate;
		const SystemScheduleIssue issue = BuildSchedule(candidate);
		if (issue.Status != SystemScheduleStatus::Ready) {
			std::string message = "invalid ECS system schedule at '" + issue.System + "'";
			if (!issue.Dependency.empty()) {
				message += " referencing '" + issue.Dependency + "'";
			}
			throw std::logic_error(message);
		}
		Compiled = std::move(candidate);
		ScheduleDirty = false;
	}

	void Scheduler::RecordTiming(const Registered &system, float milliseconds) {
		auto existing = std::find_if(LastTimings.begin(), LastTimings.end(), [&system](const Timing &timing) {
			return timing.Name == system.Name;
		});
		if (existing != LastTimings.end()) {
			existing->Milliseconds += milliseconds;
		} else {
			LastTimings.push_back(Timing{system.Name, system.RunPhase, milliseconds});
		}
	}

	void Scheduler::ClearTimings() {
		LastTimings.clear();
	}

	void Scheduler::Tick(Store &store, float deltaSeconds) {
		// The clock moves first, so every system in the tick - including the
		// ones in PreSimulation - sees the time it is simulating rather than
		// the time it has just left.
		store.AdvanceTick(deltaSeconds);

		ClearTimings();
		RunPhases(store, Phase::Input, Phase::Render);
	}

	void Scheduler::RunPhases(Store &store, Phase first, Phase last) {
		// `ECS`, not `Simulation`. Every engine and game system runs through
		// here, so this is where a game's time goes - and a category that only
		// said "simulation" could not separate the systems from the driver
		// around them.
		ENGINE_PROFILE_CAT("ecs.systems", core::ProfileCategory::ECS);
		EnsureSchedule();

		for (uint8_t index = static_cast<uint8_t>(first);
			 index <= static_cast<uint8_t>(last) && index < static_cast<uint8_t>(Phase::Count);
			 index++) {
			const auto phase = static_cast<Phase>(index);

			// A span per phase, between the scheduler and its systems.
			//
			// **This is the level a bottleneck is obvious at.** One bar for the
			// whole tick says the tick is slow; forty bars, one per system, is
			// a wall of text nobody reads at sixty frames a second. Four bars
			// - one per phase - is the shape that answers "which part" in a
			// glance, and the systems under it answer "which one" when you
			// look closer.
			//
			// STABLE, because `GetPhaseName` returns a literal that outlives
			// the frame; there is nothing to copy.
			const std::string_view phaseName = GetPhaseName(phase);
			ENGINE_PROFILE_DYNAMIC_STABLE("phase", phaseName, core::ProfileCategory::ECS);

			for (const Wave &wave : Compiled[index]) {
				if (wave.Parallel) {
					std::vector<float> milliseconds(wave.Systems.size(), 0.0f);
					parallel::Jobs::For(
						wave.Systems.size(),
						1,
						[&](size_t begin, size_t end) {
							for (size_t task = begin; task < end; task++) {
								const Registered &system = Systems[wave.Systems[task]];
								const uint64_t started = core::Clock::Nanoseconds();
								{
									const std::string_view label = system.Name;
									ENGINE_PROFILE_PRODUCER_DYNAMIC_STABLE("system", label);
									system.ParallelBody(static_cast<const Store &>(store));
								}
								const uint64_t finished = core::Clock::Nanoseconds();
								milliseconds[task] =
									static_cast<float>(static_cast<double>(finished - started) / 1'000'000.0);
							}
						},
						1
					);
					for (size_t task = 0; task < wave.Systems.size(); task++) {
						const Registered &system = Systems[wave.Systems[task]];
						RecordTiming(system, milliseconds[task]);
						core::FrameGraph::ReportNamed(
							"parallel system", system.Name, core::ProfileCategory::ECS, milliseconds[task]
						);
					}
					continue;
				}

				for (const size_t systemIndex : wave.Systems) {
					Registered &system = Systems[systemIndex];
					const uint64_t started = core::Clock::Nanoseconds();
					{
						const std::string_view label = system.Name;
						ENGINE_PROFILE_DYNAMIC_STABLE("system", label, core::ProfileCategory::ECS);
						if (system.Mode == Execution::Serial) {
							system.Body(store);
						} else {
							system.ParallelBody(static_cast<const Store &>(store));
						}
					}
					const uint64_t finished = core::Clock::Nanoseconds();
					const auto milliseconds =
						static_cast<float>(static_cast<double>(finished - started) / 1'000'000.0);
					RecordTiming(system, milliseconds);
				}
			}
		}
	}
}
