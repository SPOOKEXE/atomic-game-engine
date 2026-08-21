// Turning an authored render graph into the frame the device is asked to run.
//
// **This is the GPU work a benchmark can measure honestly, and it is not a
// small one.** Everything past the submission belongs to the driver; everything
// before it is this - compiling a dependency graph into waves, expanding those
// waves into one invocation per world and per view, accounting the resource
// traffic each one reads and writes, finding the ownership handoffs where a
// resource crosses between queue classes, and cutting the result into command
// buffers. None of it touches a device and all of it happens every frame that
// the graph or the frame's shape changes.
//
// **Two of these run per frame and one of them runs per frame *change*, which
// is the distinction the rows are organised around.** `CompileSchedule` depends
// only on the authored graph, so a game that does not edit its pipeline pays it
// once. `PlanFrame` depends on how many worlds and views the frame has, so it
// is paid again whenever a viewport is added or a world appears - which in the
// studio is while somebody drags a splitter, at the display's rate. A planner
// that is cheap for one view and quadratic in four is a studio that stutters
// exactly when a person is interacting with it.
//
// **The scaling axes are worlds, views and graph size, and they multiply.** A
// planned invocation exists per scheduled node per distinct world per view, so
// the plan is the product of three numbers and a benchmark that varied one at a
// time would miss the term that hurts. The rows walk each axis against a fixed
// remainder so the shape of the growth in each is separable.
//
// The authored graphs here are shaped like a real pipeline - a shadow pass per
// world, several view passes reading it, a compute pass on another queue, a
// present at the end - rather than being a chain, because the interesting cost
// is in the resource relations and a chain has almost none.

#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.bench.submission")

using engine::core::Name;
using engine::testing::Consume;
using namespace engine::graph;

namespace submission_bench {
	// A frame's resolution. Fixed across every row, because it scales the byte
	// accounting and nothing else - and the byte accounting is arithmetic per
	// invocation rather than per pixel.
	constexpr uint32_t WIDTH = 1920;
	constexpr uint32_t HEIGHT = 1080;

	// One authored pipeline, built once per shape.
	//
	// `passes` view passes read the world's shadow map and write colour, one of
	// them on the compute queue so that the plan has a real queue handoff in it
	// rather than a degenerate one.
	struct Pipeline {
		size_t Passes = 0;
		RenderGraph Graph;
		ExecutionSchedule Schedule;
	};

	Node NodeOf(const std::string &name, NodeScope scope) {
		return {.Name = Name(name.c_str()), .Kind = Name(name.c_str()), .Scope = scope};
	}

	// A deque, so a reference handed out earlier survives a later shape being
	// built. `RenderGraph` and `ExecutionSchedule` are held by value.
	std::deque<Pipeline> &Pipelines() {
		static std::deque<Pipeline> built;
		return built;
	}

	const Pipeline &PipelineOf(size_t passes) {
		for (const Pipeline &pipeline : Pipelines()) {
			if (pipeline.Passes == passes) {
				return pipeline;
			}
		}

		Pipelines().emplace_back();
		Pipeline &pipeline = Pipelines().back();
		pipeline.Passes = passes;

		const ResourceId shadow = pipeline.Graph.AddResource(
			{.Name = Name("shadow"), .Kind = ResourceKind::Depth, .Format = ResourceFormat::D32F}
		);
		const ResourceId clusters = pipeline.Graph.AddResource(
			{.Name = Name("clusters"), .Kind = ResourceKind::Storage, .Format = ResourceFormat::R32F}
		);
		const ResourceId window = pipeline.Graph.AddResource(
			{.Name = Name("window"), .Kind = ResourceKind::Colour, .Format = ResourceFormat::RGBA8}
		);

		// One shadow map per world, which is what makes a world-scoped resource
		// worth having and what the plan has to instance.
		Node shadowPass = NodeOf("shadow", NodeScope::World);
		shadowPass.Writes = {shadow};
		pipeline.Graph.AddNode(shadowPass);

		// A compute pass on another queue, so the plan carries a real ownership
		// handoff between queue classes.
		Node clusterPass = NodeOf("build-clusters", NodeScope::World);
		clusterPass.Reads = {shadow};
		clusterPass.Writes = {clusters};
		clusterPass.Parameters = {{Name("queue"), "compute"}};
		pipeline.Graph.AddNode(clusterPass);

		// The view passes, chained so the schedule has depth rather than one
		// very wide wave - a wave of a hundred independent nodes is a shape no
		// authored pipeline has.
		ResourceId previous = clusters;
		for (size_t pass = 0; pass < passes; pass++) {
			const std::string name = "pass." + std::to_string(pass);
			const ResourceId colour = pipeline.Graph.AddResource(
				{.Name = Name(("colour." + std::to_string(pass)).c_str()),
				 .Kind = ResourceKind::Colour,
				 .Format = ResourceFormat::RGBA8}
			);
			Node view = NodeOf(name, NodeScope::View);
			view.Reads = {shadow, previous};
			view.Writes = {colour};
			pipeline.Graph.AddNode(view);
			previous = colour;
		}

		Node present = NodeOf("present", NodeScope::Frame);
		present.Reads = {previous};
		present.Writes = {window};
		pipeline.Graph.AddNode(present);

		Name offender;
		CompileSchedule(pipeline.Graph, pipeline.Schedule, offender);
		return pipeline;
	}

	// One world key per view. Repeated keys share world-scoped resources, which
	// is what a split-screen frame looks like and what a multi-world studio
	// frame does not.
	const std::vector<uint64_t> &Views(size_t views, size_t distinctWorlds) {
		struct Shape {
			size_t Views;
			size_t Worlds;
			std::vector<uint64_t> Keys;
		};
		static std::deque<Shape> built;

		for (const Shape &shape : built) {
			if (shape.Views == views && shape.Worlds == distinctWorlds) {
				return shape.Keys;
			}
		}

		Shape shape{views, distinctWorlds, {}};
		shape.Keys.reserve(views);
		for (size_t view = 0; view < views; view++) {
			shape.Keys.push_back(static_cast<uint64_t>(view % distinctWorlds) + 1);
		}
		built.push_back(std::move(shape));
		return built.back().Keys;
	}

	// Plans one frame and reports its size, so nothing can be elided.
	size_t Plan(const Pipeline &pipeline, const std::vector<uint64_t> &worlds) {
		FrameExecutionPlan plan;
		Name offender;
		const ExecutionPlanStatus status =
			PlanFrame(pipeline.Graph, pipeline.Schedule, worlds, WIDTH, HEIGHT, plan, offender);
		if (status != ExecutionPlanStatus::Ok) {
			return 0;
		}
		size_t invocations = 0;
		for (const PlannedWave &wave : plan.Waves) {
			invocations += wave.Invocations.size();
		}
		return invocations + plan.Transfers.size();
	}
}

using namespace submission_bench;

// --- compiling the authored graph ---------------------------------------------
//
// Paid once for a game that does not edit its pipeline, and once per keystroke
// for the studio's pipeline editor - which is the caller that decides whether
// this figure matters.

BENCH("CompileSchedule · a 8-pass pipeline", 1000) {
	const Pipeline &pipeline = PipelineOf(8);
	for (size_t call = 0; call < 1000; call++) {
		ExecutionSchedule schedule;
		Name offender;
		Consume(CompileSchedule(pipeline.Graph, schedule, offender) == ScheduleStatus::Ok);
	}
}

BENCH("CompileSchedule · a 64-pass pipeline", 100) {
	// Eight times the passes. A dependency compile that is quadratic in the
	// node count is invisible at eight and is what a pipeline editor would run
	// into first, because the graph somebody is midway through authoring is the
	// largest one that exists.
	const Pipeline &pipeline = PipelineOf(64);
	for (size_t call = 0; call < 100; call++) {
		ExecutionSchedule schedule;
		Name offender;
		Consume(CompileSchedule(pipeline.Graph, schedule, offender) == ScheduleStatus::Ok);
	}
}

// --- expanding it for a frame -------------------------------------------------

BENCH("PlanFrame · 8 passes · 1 view · 1 world", 1000) {
	// A game, full screen. The row every other one here is read against.
	const Pipeline &pipeline = PipelineOf(8);
	const std::vector<uint64_t> &worlds = Views(1, 1);
	for (size_t frame = 0; frame < 1000; frame++) {
		Consume(Plan(pipeline, worlds));
	}
}

BENCH("PlanFrame · 8 passes · 4 views · 1 world", 1000) {
	// Split screen. One world, four views, so the world-scoped shadow pass is
	// planned once and the view passes four times - which is the whole point of
	// the scope distinction and the thing that would be lost by planning per
	// view uniformly.
	const Pipeline &pipeline = PipelineOf(8);
	const std::vector<uint64_t> &worlds = Views(4, 1);
	for (size_t frame = 0; frame < 1000; frame++) {
		Consume(Plan(pipeline, worlds));
	}
}

BENCH("PlanFrame · 8 passes · 4 views · 4 worlds", 1000) {
	// The studio with four viewports onto four different worlds, which is the
	// arrangement `Mirrors-4-worlds` demonstrates. Against the row above, the
	// difference is entirely the world-scoped work no longer being shared.
	const Pipeline &pipeline = PipelineOf(8);
	const std::vector<uint64_t> &worlds = Views(4, 4);
	for (size_t frame = 0; frame < 1000; frame++) {
		Consume(Plan(pipeline, worlds));
	}
}

BENCH("PlanFrame · 8 passes · 16 views · 16 worlds", 200) {
	// Past anything anybody would open on purpose, which is where a term that
	// multiplies shows itself. Sixteen times the views of the first row: a plan
	// that grows sixteen times is linear in views and one that grows far more
	// is not.
	const Pipeline &pipeline = PipelineOf(8);
	const std::vector<uint64_t> &worlds = Views(16, 16);
	for (size_t frame = 0; frame < 200; frame++) {
		Consume(Plan(pipeline, worlds));
	}
}

BENCH("PlanFrame · 64 passes · 4 views · 4 worlds", 200) {
	// The third axis. Eight times the passes at a fixed frame shape, so this
	// against `8 passes · 4 views · 4 worlds` isolates growth in the graph from
	// growth in the frame.
	const Pipeline &pipeline = PipelineOf(64);
	const std::vector<uint64_t> &worlds = Views(4, 4);
	for (size_t frame = 0; frame < 200; frame++) {
		Consume(Plan(pipeline, worlds));
	}
}

// --- cutting it into command buffers ------------------------------------------

BENCH("PlanCommandBuffers · a 8-pass schedule", 10'000) {
	// **The last step before submission, and the cheapest thing to get wrong.**
	// Consecutive waves of one queue class share a buffer, so the number of
	// buffers is a property of how the classes alternate rather than of how
	// many nodes there are - which means a pipeline that interleaves compute
	// and raster produces far more buffers than one that batches them, at the
	// same node count. This runs per frame.
	const Pipeline &pipeline = PipelineOf(8);
	for (size_t frame = 0; frame < 10'000; frame++) {
		Consume(PlanCommandBuffers(pipeline.Schedule).size());
	}
}

BENCH("PlanCommandBuffers · a 64-pass schedule", 2000) {
	const Pipeline &pipeline = PipelineOf(64);
	for (size_t frame = 0; frame < 2000; frame++) {
		Consume(PlanCommandBuffers(pipeline.Schedule).size());
	}
}
