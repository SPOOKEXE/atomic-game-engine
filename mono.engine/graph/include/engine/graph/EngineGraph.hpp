#pragma once

// Device-free orchestration across the complete engine frame.
//
// Input, AI, world, physics, animation, and rendering use one resource
// dependency model. The compiled result names independent waves, queue
// handovers, resource lifetimes, and profiling boundaries. A host decides how
// CPU jobs and GPU submissions execute those decisions.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/graph/Schedule.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::graph {

	// The fixed high-level flow of one engine frame.
	enum class EngineStage : uint8_t { Input, Ai, World, Physics, Animation, Render, Count };

	// Stable stage label used by profiling and diagnostics.
	const char *Describe(EngineStage stage);

	// How long an orchestration resource remains valid.
	enum class EngineResourceLifetime : uint8_t { Wave, Tick, World, Persistent };

	// One named value crossing node boundaries by copy or owned storage.
	struct EngineResourceDesc {
		core::Name Name;
		EngineResourceLifetime Lifetime = EngineResourceLifetime::Tick;
		bool External = false;
	};

	struct EngineNodeId {
		uint32_t Value = 0;

		bool IsValid() const {
			return Value != 0;
		}

		auto operator<=>(const EngineNodeId &) const = default;
	};

	// One unit of engine work before dependency compilation.
	struct EngineNode {
		core::Name Name;
		EngineStage Stage = EngineStage::World;
		ExecutionQueue Queue = ExecutionQueue::Cpu;
		std::vector<core::Name> Reads;
		std::vector<core::Name> Writes;
		bool AsyncEligible = false;
		bool Profile = true;
	};

	// The authored, device-free orchestration graph.
	class EngineGraph {
	  public:
		bool AddResource(EngineResourceDesc resource);
		EngineNodeId AddNode(EngineNode node);

		const EngineNode *Find(EngineNodeId node) const;
		const EngineResourceDesc *FindResource(core::Name name) const;

		size_t NodeCount() const {
			return Nodes.size();
		}

		std::span<const EngineNode> AllNodes() const {
			return Nodes;
		}

		std::span<const EngineResourceDesc> Resources() const {
			return DeclaredResources;
		}

	  private:
		std::vector<EngineNode> Nodes;
		std::vector<EngineResourceDesc> DeclaredResources;
	};

	// One node after scheduling.
	struct EngineScheduledNode {
		EngineNodeId Node;
		ExecutionQueue Queue = ExecutionQueue::Cpu;
		bool AsyncEligible = false;
		bool Profile = true;
	};

	// Independent work. A host may run every entry concurrently.
	struct EngineWave {
		std::vector<EngineScheduledNode> Nodes;
		bool Concurrent = false;
	};

	// One queue ownership or visibility handover.
	struct EngineBarrier {
		core::Name Resource;
		ExecutionQueue From = ExecutionQueue::Cpu;
		ExecutionQueue To = ExecutionQueue::Cpu;
		uint32_t ProducerWave = 0;
		uint32_t ConsumerWave = 0;
	};

	// The live interval of one named resource.
	struct EngineResourcePlan {
		core::Name Resource;
		EngineResourceLifetime Lifetime = EngineResourceLifetime::Tick;
		uint32_t FirstWave = 0;
		uint32_t LastWave = 0;
	};

	struct EngineSchedule {
		std::vector<EngineWave> Waves;
		std::vector<EngineBarrier> Barriers;
		std::vector<EngineResourcePlan> Resources;

		void Clear() {
			Waves.clear();
			Barriers.clear();
			Resources.clear();
		}
	};

	enum class EngineGraphStatus : uint8_t {
		Ok,
		InvalidName,
		UnknownResource,
		MissingProducer,
		StageRegression,
		Cycle,
	};

	const char *Describe(EngineGraphStatus status);

	// Concurrent mode emits every ready node in one wave. Deterministic mode
	// emits one ready node at a time in declaration order for replay audits.
	enum class EngineExecutionMode : uint8_t { Concurrent, Deterministic };

	// Compiles dependencies, stage boundaries, barriers, and live intervals.
	EngineGraphStatus CompileEngineGraph(
		const EngineGraph &graph,
		EngineSchedule &out,
		core::Name &offender,
		EngineExecutionMode mode = EngineExecutionMode::Concurrent
	);

	// Imports a render graph into the final stage using the same queue model.
	EngineGraphStatus AppendRenderStage(
		const RenderGraph &renderGraph,
		EngineGraph &engineGraph,
		core::Name &offender,
		std::string_view prefix = "render."
	);

	struct EngineRunContext {
		const EngineNode &Work;
		uint32_t Wave = 0;
		std::span<const EngineBarrier> Barriers;
	};

	// Adapter boundary for CPU pools and GPU submission backends.
	class EngineNodeRunner {
	  public:
		virtual ~EngineNodeRunner() = default;
		virtual bool BeginWave(uint32_t wave, bool concurrent, std::span<const EngineBarrier> barriers) = 0;
		virtual bool Run(const EngineRunContext &context) = 0;
		virtual bool EndWave(uint32_t wave) = 0;
	};

	// Executes one schedule without allowing work to escape its tick.
	bool
	ExecuteEngineGraph(const EngineGraph &graph, const EngineSchedule &schedule, EngineNodeRunner &runner);
}
