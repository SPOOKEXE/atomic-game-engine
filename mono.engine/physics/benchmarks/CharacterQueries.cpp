// The production camera obstruction and character grounding callers, including their writes.
#include <engine/core/HeapProfile.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.characterqueries")
TEST_DEPENDS("engine.physics.characters")
TEST_DEPENDS("engine.physics.query")

namespace {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	constexpr size_t CALLS = 64, CAPACITY = 512;
	void Require(bool okay, const char *message) {
		if (!okay) throw std::runtime_error(message);
	}
	bool Flag(const char *name) {
		const char *raw = std::getenv(name);
		if (!raw) return false;
		const std::string_view value(raw);
		Require(value == "0" || value == "1", "character query flags require 0 or 1");
		return value == "1";
	}
	size_t Option(const char *name, size_t fallback, size_t low, size_t high) {
		const char *raw = std::getenv(name);
		if (!raw) return fallback;
		const std::string_view value(raw);
		size_t number = 0;
		const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
		Require(
			result.ec == std::errc{} && result.ptr == value.data() + value.size() && number >= low &&
				number <= high,
			"character query option outside bounded range"
		);
		return number;
	}
	enum class Kind { Clear, Wall, Trigger, Tagged, Rolled, Portal, Ground, GroundMiss, GroundTrigger };
	const char *NameOf(Kind kind) {
		switch (kind) {
		case Kind::Clear:
			return "camera_clear";
		case Kind::Wall:
			return "camera_wall";
		case Kind::Trigger:
			return "camera_trigger_then_wall";
		case Kind::Tagged:
			return "camera_tagged_sheet_then_wall";
		case Kind::Rolled:
			return "camera_rolled_wall";
		case Kind::Portal:
			return "camera_open_portal_clear";
		case Kind::Ground:
			return "character_floor";
		case Kind::GroundMiss:
			return "character_no_floor";
		case Kind::GroundTrigger:
			return "character_trigger_not_floor";
		}
		throw std::runtime_error("unknown character query row");
	}
	bool IsGround(Kind kind) {
		return kind == Kind::Ground || kind == Kind::GroundMiss || kind == Kind::GroundTrigger;
	}
	bool Blocked(Kind kind) {
		return kind == Kind::Wall || kind == Kind::Trigger || kind == Kind::Tagged || kind == Kind::Rolled;
	}
	struct Fixture {
		ecs::Store Store{"bench.character.queries"};
		ecs::Entity Subject, Eye, Blocker, Ignored, Trigger, Humanoid;
		core::CFrame Basis;
		size_t Obstacles = 0;
		Kind Scenario;
		Fixture(size_t obstacles, Kind kind) : Scenario(kind) {
			scene::RegisterSceneClasses();
			scene::InstallServices(Store);
			physics::PreparePhysicsWorld(Store, 4);
			if (kind == Kind::Rolled) Basis = core::CFrame::Angles(.23f, .41f, -.17f);
			scene::PartDesc body;
			body.Simulated = false;
			body.Size = {1, 2, 1};
			if (IsGround(kind)) body.Frame.Position = {0, 1.05f, 0};
			Subject = scene::MakePart(Store, body);
			Store.SetParent(Subject, scene::WorkspaceOf(Store));
			Eye = Store.CreateInstance(scene::CameraClass(), "Eye");
			Store.Set(Eye, scene::Transform{});
			Store.Set(Eye, scene::CameraSubject{.Target = Subject});
			Store.SetResource(scene::ActiveCamera{Eye});
			scene::CameraController controller;
			controller.Distance = 10;
			controller.HeadHeight = 0;
			controller.Angles = {};
			controller.Basis = Basis;
			Store.SetResource(controller);
			if (Blocked(kind)) Blocker = Wall(5, .2f);
			if (kind == Kind::Trigger) {
				Trigger = Wall(2, .2f);
				auto collider = *Store.Get<scene::Collider>(Trigger);
				collider.Trigger = true;
				Store.Set(Trigger, collider);
			}
			if (kind == Kind::Tagged) {
				// A finite thin sheet fits the caller's bounded .01m skip progression.
				Ignored = Wall(3, .02f);
				Require(
					scene::AddTag(Store, Ignored, core::Name("IgnorePoppercam")), "camera ignore tag refused"
				);
			}
			if (kind == Kind::Portal) {
				const auto near = Wall(5, .2f);
				const auto far = Box(core::CFrame({100, 0, 0}), {4, 4, .2f});
				const auto link = [&](ecs::Entity from, ecs::Entity to) {
					const auto hole =
						Store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "Hole");
					Store.Set(hole, scene::SurfaceCamera{});
					Store.Set(hole, scene::Portal{to});
					Store.SetParent(hole, from);
				};
				link(near, far);
				link(far, near);
				Require(scene::OpenPortals(Store) == 2, "camera benchmark portal pair did not open");
			}
			if (IsGround(kind)) {
				Humanoid = Store.CreateInstance(scene::HumanoidClass(), "Steering");
				auto humanoid = *Store.Get<scene::Humanoid>(Humanoid);
				humanoid.RootPart = Subject;
				humanoid.Height = 2;
				humanoid.GroundTolerance = .2f;
				humanoid.Enabled = true;
				Store.Set(Humanoid, humanoid);
				if (kind != Kind::GroundMiss) {
					const auto floor = Box(core::CFrame({0, -1, 0}), {4, 2, 4});
					if (kind == Kind::GroundTrigger) {
						auto collider = *Store.Get<scene::Collider>(floor);
						collider.Trigger = true;
						Store.Set(floor, collider);
					}
				}
			}
			Require(Obstacles <= obstacles, "character query fixture exceeds obstacle budget");
			while (Obstacles < obstacles) {
				const size_t index = Obstacles;
				auto pose = core::CFrame::Angles(.2f, .3f, -.1f);
				pose.Position = {256 + float(index % 128) * 8, 30, 256 + float(index / 128) * 8};
				const auto entity = Box(pose, {1, 1, 1});
				if (index % 4 == 0) Store.Set(entity, scene::Motion{});
			}
			physics::SyncBroadphase(Store);
		}
		ecs::Entity Box(const core::CFrame &frame, core::Vector3 size) {
			scene::PartDesc desc;
			desc.Frame = frame;
			desc.Size = size;
			desc.Simulated = false;
			const auto entity = scene::MakePart(Store, desc);
			Store.SetParent(entity, scene::WorkspaceOf(Store));
			++Obstacles;
			return entity;
		}
		ecs::Entity Wall(float z, float depth) {
			auto pose = Basis;
			pose.Position = Basis.PointToWorldSpace({0, 0, z});
			return Box(pose, {4, 4, depth});
		}
		void Reset() {
			if (IsGround(Scenario)) {
				auto humanoid = *Store.Get<scene::Humanoid>(Humanoid);
				humanoid.Grounded = Scenario != Kind::Ground;
				Store.Set(Humanoid, humanoid);
				return;
			}
			auto *controller = Store.ResourceMutable<scene::CameraController>();
			controller->Mode = scene::CameraMode::Scriptable;
			(void)physics::UpdatePoppercam(Store);
			Require(
				controller->OccludedDistance < 0 && scene::LocalTransparencyOf(Store, Subject) == 0,
				"camera clear transition failed"
			);
			if (Blocker)
				Require(
					scene::LocalTransparencyOf(Store, Blocker) == 0, "previous camera blocker remained faded"
				);
			controller->Mode = scene::CameraMode::Classic;
		}
		void Verify() {
			if (IsGround(Scenario)) {
				Require(
					Store.Get<scene::Humanoid>(Humanoid)->Grounded == (Scenario == Kind::Ground),
					"character analytic grounded state differs"
				);
				return;
			}
			const auto &controller = *Store.Resource<scene::CameraController>();
			Require(std::abs(controller.Distance - 10) < .001f, "camera authored arm distance changed");
			Require(
				Blocked(Scenario) ? std::abs(controller.OccludedDistance - 4.75f) < .002f
								  : controller.OccludedDistance < 0,
				"camera analytic obstruction distance differs"
			);
			if (Blocker)
				Require(
					std::abs(scene::LocalTransparencyOf(Store, Blocker) - .6f) < .001f,
					"camera actual blocker did not fade"
				);
			Require(scene::LocalTransparencyOf(Store, Subject) == 0, "camera faded its own subject");
			if (Ignored)
				Require(scene::LocalTransparencyOf(Store, Ignored) == 0, "camera faded tagged sheet");
			if (Trigger) Require(scene::LocalTransparencyOf(Store, Trigger) == 0, "camera faded trigger");
		}
	};
	struct Report {
		Kind Scenario = Kind::Clear;
		size_t Obstacles = 0, Colliders = 0, Trials = 0, Warmups = 0;
		bool Memory = false, Heap = false;
		std::array<uint64_t, CAPACITY> Samples{};
		uint64_t CallerCalls = 0, ChangedStates = 0, TestedHumanoids = 0, AllocatedBytes = 0,
				 AllocatedBlocks = 0;
		core::HeapTotals LastHeap;
		physics::PhysicsMemoryStats LastPhysics;
	};
	std::vector<Report> Latest;
	void Print() {
		for (const auto &report : Latest) {
			std::cout << "character-queries row=" << NameOf(report.Scenario)
					  << " obstacles=" << report.Obstacles << " total_colliders=" << report.Colliders
					  << " samples=" << report.Trials << " warmups=" << report.Warmups
					  << " actual_caller_calls=" << report.CallerCalls
					  << " actual_changed_state_returns=" << report.ChangedStates
					  << " actual_humanoids_tested=" << report.TestedHumanoids
					  << " underlying_ray_calls=unavailable changed_state_is_hit_count=false "
						 "tagged_sheet_depth=.02 setup_oracles=excluded"
					  << " heap_compiled=" << report.Heap
					  << " timing_valid=" << (!report.Memory && !report.Heap) << " measurement_mode="
					  << (report.Memory ? "memory_diagnostic"
						  : report.Heap ? "heap_instrumented_latency"
										: "heap_off_latency");
			if (report.Memory) {
				if (report.Heap)
					std::cout << " allocated_bytes=" << report.AllocatedBytes
							  << " allocated_blocks=" << report.AllocatedBlocks
							  << " process_live_bytes=" << report.LastHeap.LiveBytes
							  << " process_live_blocks=" << report.LastHeap.LiveBlocks
							  << " process_lifetime_peak_bytes=" << report.LastHeap.PeakBytes
							  << " profiler_overhead_bytes=" << report.LastHeap.OverheadBytes;
				else
					std::cout
						<< " allocated_bytes=unavailable allocated_blocks=unavailable "
						   "process_live_bytes=unavailable process_live_blocks=unavailable "
						   "process_lifetime_peak_bytes=unavailable profiler_overhead_bytes=unavailable";
				const auto bytes = report.LastPhysics.Total();
				std::cout << " physics_live_bytes=" << bytes.LiveBytes
						  << " physics_retained_bytes=" << bytes.RetainedBytes
						  << " physics_scope=world_containers_excludes_ecs_geometry "
							 "heap_scope=sum_measured_windows\n";
				continue;
			}
			auto times = report.Samples;
			std::sort(times.begin(), times.begin() + report.Trials);
			const auto rank = [&](size_t p) { return times[(report.Trials * p + 99) / 100 - 1]; };
			double total = 0;
			for (size_t i = 0; i < report.Trials; ++i)
				total += double(report.Samples[i]);
			std::cout << " batch_p50_ns=" << rank(50) << " batch_p95_ns=" << rank(95)
					  << " batch_p99_ns=" << rank(99) << " batch_max_ns=" << times[report.Trials - 1]
					  << " derived_mean_ns_per_caller=" << total / double(report.CallerCalls)
					  << " quantile=nearest_rank individual_caller_tails=not_measured tail_status="
					  << (report.Trials >= 100 ? "at_least100" : "insufficient_below100") << '\n';
		}
	}
	void Measure(size_t obstacles, Kind kind, size_t trials, size_t warmups, bool memory) {
		Fixture fixture(obstacles, kind);
		Report report;
		report.Scenario = kind;
		report.Obstacles = obstacles;
		report.Trials = trials;
		report.Warmups = warmups;
		report.Memory = memory;
		report.Heap = core::HeapProfile::IsCompiledIn();
		const auto *world = fixture.Store.Resource<physics::PhysicsWorld>();
		report.Colliders = world->StaticColliders() + world->DynamicColliders();
		std::array<bool, CALLS> changed{};
		std::array<size_t, CALLS> tested{};
		const auto call = [&] {
			for (size_t index = 0; index < CALLS; ++index)
				if (IsGround(kind))
					tested[index] = physics::GroundCharacters(fixture.Store);
				else
					changed[index] = physics::UpdatePoppercam(fixture.Store);
		};
		fixture.Reset();
		call();
		fixture.Verify();
		for (size_t sample = 0; sample < trials + warmups; ++sample) {
			fixture.Reset();
			core::HeapTotals before;
			if (memory && report.Heap) before = core::HeapProfile::Totals();
			const auto begin = Clock::now();
			call();
			const auto elapsed =
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
			core::HeapTotals after;
			if (memory && report.Heap) after = core::HeapProfile::Totals();
			fixture.Verify();
			for (size_t index = 0; index < CALLS; ++index)
				if (IsGround(kind))
					Require(tested[index] == 1, "grounding caller failed to test enabled humanoid");
				else
					Require(
						changed[index] == (Blocked(kind) && index == 0), "camera changed-state return differs"
					);
			if (sample < warmups) continue;
			report.Samples[sample - warmups] = static_cast<uint64_t>(elapsed);
			report.CallerCalls += CALLS;
			for (size_t index = 0; index < CALLS; ++index) {
				report.ChangedStates += changed[index];
				report.TestedHumanoids += tested[index];
			}
			if (memory && report.Heap) {
				Require(
					before.DroppedScopes == after.DroppedScopes, "character benchmark heap scopes dropped"
				);
				report.AllocatedBytes += after.TotalBytes - before.TotalBytes;
				report.AllocatedBlocks += after.TotalBlocks - before.TotalBlocks;
				report.LastHeap = after;
			}
			report.LastPhysics = world->MemoryStats();
		}
		Latest.push_back(report);
	}
	void Run() {
		const bool enabled = Flag("MONO_CHARACTER_QUERIES_BENCH"),
				   memory = Flag("MONO_CHARACTER_QUERIES_MEMORY");
		if (!enabled) {
			std::cout << "character-queries skipped=opt_in_required\n";
			return;
		}
		const size_t trials = Option("MONO_CHARACTER_QUERIES_TRIALS", 101, 1, CAPACITY),
					 warmups = Option("MONO_CHARACTER_QUERIES_WARMUPS", 3, 0, 32);
		static const bool registered = [] {
			Require(std::atexit(Print) == 0, "character report registration failed");
			return true;
		}();
		(void)registered;
		Latest.clear();
		Latest.reserve(23);
		for (size_t count : {0u, 8u, 1024u, 8192u}) {
			Measure(count, Kind::Clear, trials, warmups, memory);
			Measure(count, count ? Kind::Ground : Kind::GroundMiss, trials, warmups, memory);
			if (count)
				for (const auto kind : {Kind::Wall, Kind::Trigger, Kind::Tagged, Kind::Rolled})
					Measure(count, kind, trials, warmups, memory);
			if (count == 8192)
				for (const auto kind : {Kind::Portal, Kind::GroundMiss, Kind::GroundTrigger})
					Measure(count, kind, trials, warmups, memory);
		}
	}
}

BENCH("Character queries | real poppercam and grounding callers", 1) {
	Run();
}
