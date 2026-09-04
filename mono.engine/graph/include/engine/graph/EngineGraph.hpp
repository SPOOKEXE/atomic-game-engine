#pragma once

// arch-waiver public-header: forward graph API. Pipeline hosts consume this
// complete graph description contract.

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
		// Stable name, retention span, and whether a host supplies the value.
		//@{
		core::Name Name;
		EngineResourceLifetime Lifetime = EngineResourceLifetime::Tick;
		bool External = false;
		//@}
	};

	// Dense handle into an `EngineGraph` node table.
	struct EngineNodeId {
		// One-based node slot. Zero is invalid.
		uint32_t Value = 0;

		// Whether this handle names a node slot.
		bool IsValid() const {
			return Value != 0;
		}

		// Orders and compares handles by their dense slot.
		auto operator<=>(const EngineNodeId &) const = default;
	};

	// One unit of engine work before dependency compilation.
	struct EngineNode {
		// Identity and execution placement.
		//@{
		core::Name Name;
		EngineStage Stage = EngineStage::World;
		ExecutionQueue Queue = ExecutionQueue::Cpu;
		//@}

		// Named resource dependencies.
		//@{
		std::vector<core::Name> Reads;
		std::vector<core::Name> Writes;
		//@}

		// Scheduling and profiling policy.
		//@{
		bool AsyncEligible = false;
		bool Profile = true;
		//@}
	};

	// The authored, device-free orchestration graph.
	class EngineGraph {
	  public:
		// Adds one named resource, refusing duplicates or invalid names.
		bool AddResource(EngineResourceDesc resource);

		// Adds one work node and returns its stable handle.
		EngineNodeId AddNode(EngineNode node);

		// Looks up a node by handle.
		const EngineNode *Find(EngineNodeId node) const;

		// Looks up a declared resource by its stable name.
		const EngineResourceDesc *FindResource(core::Name name) const;

		// Number of authored nodes.
		size_t NodeCount() const {
			return Nodes.size();
		}

		// Authored nodes in declaration order.
		std::span<const EngineNode> AllNodes() const {
			return Nodes;
		}

		// Declared resources in declaration order.
		std::span<const EngineResourceDesc> Resources() const {
			return DeclaredResources;
		}

	  private:
		std::vector<EngineNode> Nodes;
		std::vector<EngineResourceDesc> DeclaredResources;
	};

	// One node after scheduling.
	struct EngineScheduledNode {
		// Work handle and the execution policy copied from its declaration.
		//@{
		EngineNodeId Node;
		ExecutionQueue Queue = ExecutionQueue::Cpu;
		bool AsyncEligible = false;
		bool Profile = true;
		//@}
	};

	// Independent work. A host may run every entry concurrently.
	struct EngineWave {
		// Ready work and whether a host may execute it concurrently.
		//@{
		std::vector<EngineScheduledNode> Nodes;
		bool Concurrent = false;
		//@}
	};

	// One queue ownership or visibility handover.
	struct EngineBarrier {
		// Resource and queue ownership transition.
		//@{
		core::Name Resource;
		ExecutionQueue From = ExecutionQueue::Cpu;
		ExecutionQueue To = ExecutionQueue::Cpu;
		//@}

		// Waves separated by the transition.
		//@{
		uint32_t ProducerWave = 0;
		uint32_t ConsumerWave = 0;
		//@}
	};

	// The live interval of one named resource.
	struct EngineResourcePlan {
		// Resource, retention policy, and inclusive live-wave interval.
		//@{
		core::Name Resource;
		EngineResourceLifetime Lifetime = EngineResourceLifetime::Tick;
		uint32_t FirstWave = 0;
		uint32_t LastWave = 0;
		//@}
	};

	// Complete compiled orchestration plan.
	struct EngineSchedule {
		// Execution waves, handovers, and resource live intervals.
		//@{
		std::vector<EngineWave> Waves;
		std::vector<EngineBarrier> Barriers;
		std::vector<EngineResourcePlan> Resources;
		//@}

		// Removes every compiled row so the schedule can be reused.
		void Clear() {
			Waves.clear();
			Barriers.clear();
			Resources.clear();
		}
	};

	// Result of validating and compiling an engine graph.
	enum class EngineGraphStatus : uint8_t {
		Ok,
		InvalidName,
		UnknownResource,
		MissingProducer,
		StageRegression,
		Cycle,
	};

	// Stable diagnostic label for a compilation status.
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

	// One scheduled invocation presented to an execution adapter.
	struct EngineRunContext {
		// Work, containing wave, and handovers visible at that boundary.
		//@{
		const EngineNode &Work;
		uint32_t Wave = 0;
		std::span<const EngineBarrier> Barriers;
		//@}
	};

	// Adapter boundary for CPU pools and GPU submission backends.
	class EngineNodeRunner {
	  public:
		virtual ~EngineNodeRunner() = default;

		// Opens one wave after applying its resource handovers.
		virtual bool BeginWave(uint32_t wave, bool concurrent, std::span<const EngineBarrier> barriers) = 0;

		// Executes one scheduled node.
		virtual bool Run(const EngineRunContext &context) = 0;

		// Closes a wave and waits for all of its work.
		virtual bool EndWave(uint32_t wave) = 0;
	};

	// Executes one schedule without allowing work to escape its tick.
	bool
	ExecuteEngineGraph(const EngineGraph &graph, const EngineSchedule &schedule, EngineNodeRunner &runner);
}
