#include <engine/core/Clock.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace engine::ecs {

	enum class Scheduler::Execution : uint8_t { Serial, ParallelReadOnly };

	struct Scheduler::Registered {
		std::string Name;
		Phase RunPhase = Phase::Simulation;
		std::shared_ptr<System> Body;
		std::shared_ptr<ParallelSystem> ParallelBody;
		SystemOrder Order;
		uint64_t Revision = 1;
		Execution Mode = Execution::Serial;
	};

	struct Scheduler::Wave {
		std::vector<size_t> Systems;
		bool Parallel = false;
	};

	struct Scheduler::TimingGroup {
		size_t RepresentativeSystem = 0;
		size_t ActiveRow = NO_TIMING_ROW;
	};

	struct Scheduler::TimingRegistration {
		size_t Group = 0;
		bool NewGroup = false;
	};

	struct Scheduler::State {
		std::mutex Guard;
		size_t ActiveCallbacks = 0;
		bool Copying = false;
		Invocation *Active = nullptr;
		std::deque<Registered> Systems;
		std::vector<Timing> LastTimings;
		std::vector<size_t> SystemTimingGroups;
		std::vector<TimingGroup> TimingGroups;
		std::shared_ptr<const Schedule> Compiled;
		bool ScheduleDirty = true;
	};

	struct Scheduler::Invocation {
		State *Owner;
		Invocation *Previous;
		Invocation *PreviousInState;
		const void *Callable;
		std::thread::id Thread;
		explicit Invocation(State *owner, const void *callable);
		~Invocation();
		void Finish(size_t systemIndex, float milliseconds);
		void ReleaseLocked();
	};

	thread_local Scheduler::Invocation *Scheduler::ActiveInvocation = nullptr;
	Scheduler::Invocation::Invocation(State *owner, const void *callable)
		: Owner(owner), Previous(ActiveInvocation), PreviousInState(nullptr), Callable(callable),
		  Thread(std::this_thread::get_id()) {
		if (Owner->Copying)
			throw std::logic_error("cannot execute a scheduler while its callable is being copied");
		for (auto *active = Owner->Active; active; active = active->PreviousInState)
			if (active->Callable == Callable && active->Thread != Thread)
				throw std::logic_error("cannot execute a callable concurrently");
		++Owner->ActiveCallbacks;
		PreviousInState = Owner->Active;
		Owner->Active = this;
		ActiveInvocation = this;
	}
	void Scheduler::Invocation::ReleaseLocked() {
		ActiveInvocation = Previous;
		auto **link = &Owner->Active;
		while (*link != this)
			link = &(*link)->PreviousInState;
		*link = PreviousInState;
		--Owner->ActiveCallbacks;
	}
	Scheduler::Invocation::~Invocation() {
		if (!Owner) return;
		const std::lock_guard lock(Owner->Guard);
		ReleaseLocked();
	}
	void Scheduler::Invocation::Finish(size_t systemIndex, float milliseconds) {
		const std::lock_guard lock(Owner->Guard);
		RecordTimingLocked(Owner, systemIndex, milliseconds);
		ReleaseLocked();
		Owner = nullptr;
	}

	std::shared_ptr<Scheduler::State> Scheduler::GetState() const {
		auto state = std::atomic_load(&State_);
		if (state) return state;
		auto candidate = std::make_shared<State>();
		if (std::atomic_compare_exchange_strong(&State_, &state, candidate)) return candidate;
		return state;
	}

	Scheduler::Scheduler(const Scheduler &other) {
		auto source = other.GetState();
		auto state = std::make_shared<State>();
		struct CopyGuard {
			State *Source;
			bool Armed = false;
			~CopyGuard() {
				if (!Armed) return;
				const std::lock_guard lock(Source->Guard);
				Source->Copying = false;
			}
		} copying{source.get()};
		{
			const std::lock_guard lock(source->Guard);
			size_t localCallbacks = 0;
			for (auto *active = ActiveInvocation; active; active = active->Previous)
				if (active->Owner == source.get()) ++localCallbacks;
			if (source->Copying || source->ActiveCallbacks != localCallbacks)
				throw std::logic_error("cannot copy a scheduler while its callbacks run on another thread");
			source->Copying = true;
			copying.Armed = true;
			state->Systems = source->Systems;
			state->LastTimings = source->LastTimings;
			state->SystemTimingGroups = source->SystemTimingGroups;
			state->TimingGroups = source->TimingGroups;
			state->Compiled = source->Compiled;
			state->ScheduleDirty = source->ScheduleDirty;
		}
		for (Registered &system : state->Systems) {
			if (system.Body) system.Body = std::make_shared<System>(*system.Body);
			if (system.ParallelBody)
				system.ParallelBody = std::make_shared<ParallelSystem>(*system.ParallelBody);
		}
		RebindTimingNames(state.get());
		std::atomic_store(&State_, std::move(state));
	}

	Scheduler &Scheduler::operator=(const Scheduler &other) {
		if (this == &other) return *this;
		Scheduler copy(other);
		auto replacement = std::atomic_exchange(&copy.State_, std::shared_ptr<State>{});
		auto retired = std::atomic_exchange(&State_, std::move(replacement));
		return *this;
	}

	Scheduler::Scheduler(Scheduler &&other) noexcept {
		std::atomic_store(&State_, std::atomic_exchange(&other.State_, std::shared_ptr<State>{}));
	}

	Scheduler &Scheduler::operator=(Scheduler &&other) noexcept {
		if (this == &other) return *this;
		auto replacement = std::atomic_exchange(&other.State_, std::shared_ptr<State>{});
		auto retired = std::atomic_exchange(&State_, std::move(replacement));
		return *this;
	}

	const std::vector<Scheduler::Timing> &Scheduler::Timings() const {
		return GetState()->LastTimings;
	}
	size_t Scheduler::SystemCount() const {
		auto state = GetState();
		const std::lock_guard lock(state->Guard);
		return state->Systems.size();
	}

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
		const auto state = GetState();
		Registered pending{
			.Name = std::string(name),
			.RunPhase = phase,
			.Body = std::make_shared<System>(std::move(system)),
			.ParallelBody = {},
			.Order = std::move(order),
		};
		{
			const std::lock_guard lock(state->Guard);
			const TimingRegistration timing = PrepareTimingRegistration(state.get(), pending.Name);
			state->Systems.push_back(std::move(pending));
			if (timing.NewGroup) state->TimingGroups.push_back(TimingGroup{state->Systems.size() - 1});
			state->SystemTimingGroups.push_back(timing.Group);
			RebindTimingNames(state.get());
			state->ScheduleDirty = true;
		}
	}

	void
	Scheduler::AddParallel(std::string_view name, Phase phase, ParallelSystem system, SystemOrder order) {
		const auto state = GetState();
		Registered pending{
			.Name = std::string(name),
			.RunPhase = phase,
			.Body = {},
			.ParallelBody = std::make_shared<ParallelSystem>(std::move(system)),
			.Order = std::move(order),
			.Mode = Execution::ParallelReadOnly,
		};
		{
			const std::lock_guard lock(state->Guard);
			const TimingRegistration timing = PrepareTimingRegistration(state.get(), pending.Name);
			state->Systems.push_back(std::move(pending));
			if (timing.NewGroup) state->TimingGroups.push_back(TimingGroup{state->Systems.size() - 1});
			state->SystemTimingGroups.push_back(timing.Group);
			RebindTimingNames(state.get());
			state->ScheduleDirty = true;
		}
	}

	// Secure every timing append before state->Systems can relocate; post-insertion
	// updates are trivial.
	Scheduler::TimingRegistration Scheduler::PrepareTimingRegistration(State *state, std::string_view name) {
		for (size_t group = 0; group < state->TimingGroups.size(); group++) {
			if (state->Systems[state->TimingGroups[group].RepresentativeSystem].Name == name) {
				state->SystemTimingGroups.reserve(state->Systems.size() + 1);
				state->LastTimings.reserve(state->Systems.size() + 1);
				return {group, false};
			}
		}

		state->SystemTimingGroups.reserve(state->Systems.size() + 1);
		state->TimingGroups.reserve(state->TimingGroups.size() + 1);
		state->LastTimings.reserve(state->Systems.size() + 1);
		return {state->TimingGroups.size(), true};
	}

	bool Scheduler::Replace(std::string_view name, uint64_t revision, System system) {
		const auto state = GetState();
		if (!system) return false;
		auto replacement = std::make_shared<System>(std::move(system));
		std::shared_ptr<System> retired;
		{
			const std::lock_guard lock(state->Guard);
			Registered *matched = nullptr;
			for (Registered &registered : state->Systems) {
				if (registered.Name != name) continue;
				if (matched) return false;
				matched = &registered;
			}
			if (!matched || matched->Mode != Execution::Serial || revision <= matched->Revision) return false;
			retired = std::move(matched->Body);
			matched->Body = std::move(replacement);
			matched->Revision = revision;
		}
		return true;
	}

	bool Scheduler::ReplaceParallel(std::string_view name, uint64_t revision, ParallelSystem system) {
		const auto state = GetState();
		if (!system) return false;
		auto replacement = std::make_shared<ParallelSystem>(std::move(system));
		std::shared_ptr<ParallelSystem> retired;
		{
			const std::lock_guard lock(state->Guard);
			Registered *matched = nullptr;
			for (Registered &registered : state->Systems) {
				if (registered.Name != name) continue;
				if (matched) return false;
				matched = &registered;
			}
			if (!matched || matched->Mode != Execution::ParallelReadOnly || revision <= matched->Revision)
				return false;
			retired = std::move(matched->ParallelBody);
			matched->ParallelBody = std::move(replacement);
			matched->Revision = revision;
		}
		return true;
	}

	uint64_t Scheduler::SystemRevision(std::string_view name) const {
		const auto state = GetState();
		const std::lock_guard lock(state->Guard);
		const Registered *matched = nullptr;
		for (const Registered &registered : state->Systems) {
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
		const auto state = GetState();
		const std::lock_guard lock(state->Guard);
		return std::any_of(
			state->Systems.begin(), state->Systems.end(), [name, phase](const Registered &system) {
				return system.Name == name && system.RunPhase == phase;
			}
		);
	}

	SystemScheduleIssue Scheduler::BuildSchedule(State *state, Schedule &schedule) {
		for (auto &phase : schedule) {
			phase.clear();
		}

		for (size_t index = 0; index < state->Systems.size(); index++) {
			const Registered &system = state->Systems[index];
			if (system.Name.empty()) {
				return {SystemScheduleStatus::EmptyName, system.Name, {}};
			}
			for (size_t prior = 0; prior < index; prior++) {
				if (state->Systems[prior].Name == system.Name &&
					state->Systems[prior].RunPhase == system.RunPhase) {
					return {SystemScheduleStatus::DuplicateName, system.Name, {}};
				}
			}
		}

		std::vector<std::vector<size_t>> outgoing(state->Systems.size());
		std::vector<size_t> incoming(state->Systems.size(), 0);
		auto addDependency = [&](size_t before, size_t after) {
			auto &edges = outgoing[before];
			if (std::find(edges.begin(), edges.end(), after) == edges.end()) {
				edges.push_back(after);
				incoming[after]++;
			}
		};

		for (size_t index = 0; index < state->Systems.size(); index++) {
			const Registered &system = state->Systems[index];
			auto resolve = [&](const std::string &dependency,
							   bool systemRunsBefore,
							   bool required) -> SystemScheduleIssue {
				size_t matched = state->Systems.size();
				bool existsInAnotherPhase = false;
				for (size_t candidate = 0; candidate < state->Systems.size(); candidate++) {
					if (state->Systems[candidate].Name != dependency) {
						continue;
					}
					if (state->Systems[candidate].RunPhase == system.RunPhase) {
						matched = candidate;
						break;
					}
					existsInAnotherPhase = true;
				}
				if (matched == state->Systems.size() && !required) {
					return {};
				}
				if (matched == state->Systems.size() && !existsInAnotherPhase) {
					return {SystemScheduleStatus::UnknownDependency, system.Name, dependency};
				}
				if (matched == state->Systems.size()) {
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
			std::vector<bool> emitted(state->Systems.size(), false);
			size_t remaining = 0;
			for (const Registered &system : state->Systems) {
				remaining += static_cast<size_t>(system.RunPhase == static_cast<Phase>(phaseIndex));
			}

			while (remaining > 0) {
				Wave wave;
				wave.Parallel = true;
				for (size_t index = 0; index < state->Systems.size(); index++) {
					const Registered &system = state->Systems[index];
					if (!emitted[index] && system.RunPhase == static_cast<Phase>(phaseIndex) &&
						incoming[index] == 0) {
						wave.Systems.push_back(index);
						wave.Parallel = wave.Parallel && system.Mode == Execution::ParallelReadOnly;
					}
				}
				if (wave.Systems.empty()) {
					for (size_t index = 0; index < state->Systems.size(); index++) {
						if (!emitted[index] &&
							state->Systems[index].RunPhase == static_cast<Phase>(phaseIndex)) {
							return {SystemScheduleStatus::Cycle, state->Systems[index].Name, {}};
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
		const auto state = GetState();
		const std::lock_guard lock(state->Guard);
		Schedule schedule;
		return BuildSchedule(state.get(), schedule);
	}

	void Scheduler::EnsureSchedule(State *state) {
		if (!state->ScheduleDirty) {
			return;
		}
		auto candidate = std::make_shared<Schedule>();
		const SystemScheduleIssue issue = BuildSchedule(state, *candidate);
		if (issue.Status != SystemScheduleStatus::Ready) {
			std::string message = "invalid ECS system schedule at '" + issue.System + "'";
			if (!issue.Dependency.empty()) {
				message += " referencing '" + issue.Dependency + "'";
			}
			throw std::logic_error(message);
		}
		state->Compiled = std::move(candidate);
		state->ScheduleDirty = false;
	}

	void Scheduler::RebindTimingNames(State *state) {
		for (const TimingGroup &group : state->TimingGroups) {
			if (group.ActiveRow != NO_TIMING_ROW) {
				state->LastTimings[group.ActiveRow].Name = state->Systems[group.RepresentativeSystem].Name;
			}
		}
	}

	void Scheduler::RecordTiming(State *state, size_t systemIndex, float milliseconds) {
		const std::lock_guard lock(state->Guard);
		RecordTimingLocked(state, systemIndex, milliseconds);
	}

	void Scheduler::RecordTimingLocked(State *state, size_t systemIndex, float milliseconds) {
		const Registered &system = state->Systems[systemIndex];
		TimingGroup &group = state->TimingGroups[state->SystemTimingGroups[systemIndex]];
		if (group.ActiveRow != NO_TIMING_ROW) {
			state->LastTimings[group.ActiveRow].Milliseconds += milliseconds;
		} else {
			state->LastTimings.push_back(Timing{system.Name, system.RunPhase, milliseconds});
			group.ActiveRow = state->LastTimings.size() - 1;
		}
	}

	void Scheduler::ClearStateTimings(State *state) {
		const std::lock_guard lock(state->Guard);
		state->LastTimings.clear();
		for (TimingGroup &group : state->TimingGroups) {
			group.ActiveRow = NO_TIMING_ROW;
		}
	}

	void Scheduler::ClearTimings() {
		const auto state = GetState();
		ClearStateTimings(state.get());
	}

	void Scheduler::Tick(Store &store, float deltaSeconds) {
		// The clock moves first, so every system in the tick - including the
		// ones in PreSimulation - sees the time it is simulating rather than
		// the time it has just left.
		const auto state = GetState();
		store.AdvanceTick(deltaSeconds);
		ClearStateTimings(state.get());
		RunStatePhases(state, store, Phase::Input, Phase::Render);
	}

	void Scheduler::RunPhases(Store &store, Phase first, Phase last) {
		RunStatePhases(GetState(), store, first, last);
	}

	void
	Scheduler::RunStatePhases(const std::shared_ptr<State> &state, Store &store, Phase first, Phase last) {
		// `ECS`, not `Simulation`. Every engine and game system runs through
		// here, so this is where a game's time goes - and a category that only
		// said "simulation" could not separate the systems from the driver
		// around them.
		ENGINE_PROFILE_CAT("ecs.systems", core::ProfileCategory::ECS);
		std::shared_ptr<const Schedule> runningSchedule;
		{
			const std::lock_guard lock(state->Guard);
			EnsureSchedule(state.get());
			runningSchedule = state->Compiled;
		}

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

			for (const Wave &wave : (*runningSchedule)[index]) {
				if (wave.Parallel) {
					std::vector<float> milliseconds(wave.Systems.size(), 0.0f);
					parallel::Jobs::For(
						wave.Systems.size(),
						1,
						[&](size_t begin, size_t end) {
							for (size_t task = begin; task < end; task++) {
								std::shared_ptr<ParallelSystem> body;
								std::optional<Invocation> invocation;
								std::string_view label;
								{
									const std::lock_guard lock(state->Guard);
									const Registered &system = state->Systems[wave.Systems[task]];
									body = system.ParallelBody;
									label = system.Name;
									invocation.emplace(state.get(), body.get());
								}
								const uint64_t started = core::Clock::Nanoseconds();
								{
									ENGINE_PROFILE_PRODUCER_DYNAMIC_STABLE("system", label);
									(*body)(static_cast<const Store &>(store));
								}
								const uint64_t finished = core::Clock::Nanoseconds();
								milliseconds[task] =
									static_cast<float>(static_cast<double>(finished - started) / 1'000'000.0);
							}
						},
						1
					);
					for (size_t task = 0; task < wave.Systems.size(); task++) {
						std::string_view label;
						{
							const std::lock_guard lock(state->Guard);
							label = state->Systems[wave.Systems[task]].Name;
						}
						RecordTiming(state.get(), wave.Systems[task], milliseconds[task]);
						core::FrameGraph::ReportNamed(
							"parallel system", label, core::ProfileCategory::ECS, milliseconds[task]
						);
					}
					continue;
				}

				for (const size_t systemIndex : wave.Systems) {
					std::string_view label;
					std::shared_ptr<System> body;
					std::shared_ptr<ParallelSystem> parallelBody;
					std::optional<Invocation> invocation;
					{
						const std::lock_guard lock(state->Guard);
						const Registered &system = state->Systems[systemIndex];
						label = system.Name;
						body = system.Body;
						parallelBody = system.ParallelBody;
						invocation.emplace(
							state.get(),
							body ? static_cast<const void *>(body.get())
								 : static_cast<const void *>(parallelBody.get())
						);
					}
					const uint64_t started = core::Clock::Nanoseconds();
					{
						ENGINE_PROFILE_DYNAMIC("system", label, core::ProfileCategory::ECS);
						ENGINE_HEAP_SCOPE(label);
						if (body)
							(*body)(store);
						else
							(*parallelBody)(static_cast<const Store &>(store));
					}
					const uint64_t finished = core::Clock::Nanoseconds();
					const auto milliseconds =
						static_cast<float>(static_cast<double>(finished - started) / 1'000'000.0);
					invocation->Finish(systemIndex, milliseconds);
				}
			}
		}
	}
} // namespace engine::ecs
