// Measures cached collection for 1,024 static parts, collection after a visual
// edit or transform epoch change, and collection after 100,000 transform writes.
// The optional profile report records collection and label-reuse scopes with no
// attribute table, alongside allocation counts.

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.render.bench.presentation")

namespace presentation_bench {
	constexpr size_t MOVING_PART_COUNT = 100'000;

	struct StaticParts {
		engine::ecs::Store World{"bench.presentation"};
		engine::ecs::Entity First;

		StaticParts() {
			engine::scene::RegisterSceneClasses();
			engine::render::RegisterPresentationComponents();
			World.SetResource(engine::render::DrawList{});
			const engine::ecs::Entity workspace = engine::scene::InstallServices(World);
			for (size_t index = 0; index < 1'024; index++) {
				const engine::ecs::Entity part = engine::scene::MakePart(World, engine::scene::PartDesc{});
				(void)World.SetParent(part, workspace);
				if (index == 0) {
					First = part;
				}
			}
			(void)engine::scene::SyncRendered(World);
			engine::render::CollectInstances(World);
			World.ClearChanges();
		}
	};

	struct MovingParts {
		engine::ecs::Store World{"bench.presentation.moving"};
		std::vector<engine::ecs::Entity> Parts;

		MovingParts() {
			engine::scene::RegisterSceneClasses();
			engine::render::RegisterPresentationComponents();
			World.SetResource(engine::render::DrawList{});
			const engine::ecs::Entity workspace = engine::scene::InstallServices(World);
			Parts.reserve(MOVING_PART_COUNT);
			for (size_t index = 0; index < MOVING_PART_COUNT; index++) {
				engine::scene::PartDesc desc;
				desc.Simulated = true;
				desc.Frame.Position = {
					static_cast<float>(index % 160) * 1.5f,
					static_cast<float>((index / 25'600) % 4) * 1.5f,
					static_cast<float>((index / 160) % 160) * 1.5f,
				};
				const engine::ecs::Entity part = engine::scene::MakePart(World, desc);
				(void)World.SetParent(part, workspace);
				Parts.push_back(part);
			}
			(void)engine::scene::SyncRendered(World);
			engine::render::CollectInstances(World);
			World.ClearChanges();
		}

		void AdvanceAndCollect() {
			for (const engine::ecs::Entity part : Parts) {
				auto transform = *World.Get<engine::scene::Transform>(part);
				transform.Frame.Position.X += 0.001f;
				World.Set(part, transform);
			}
			engine::render::CollectInstances(World);
			World.ClearChanges();
		}
	};

	StaticParts &CachedWorld() {
		static StaticParts world;
		return world;
	}

	void ReportCachedCollection(StaticParts &fixture) {
		static bool reported = false;
		if (reported) return;
		reported = true;
		if (std::getenv("MONO_PRESENTATION_REPORT") == nullptr) return;

		using namespace engine;
		constexpr size_t frames = 64;
		const bool wasEnabled = core::FrameGraph::IsEnabled();
		struct RestoreRecording {
			bool Enabled;
			~RestoreRecording() {
				core::FrameGraph::SetEnabled(Enabled);
			}
		} restore{wasEnabled};
		core::FrameGraph::SetEnabled(true);
		const auto collect = [&] {
			ENGINE_PROFILE_CAT("presentation.collect", core::ProfileCategory::Render);
			render::CollectInstances(fixture.World);
		};
		// Warm profiler scope storage separately from the timed benchmark loops.
		for (size_t warm = 0; warm < 8; ++warm) {
			core::FrameGraph::BeginFrame();
			collect();
			core::FrameGraph::EndFrame();
		}
		uint64_t allocatedBytes = 0, allocatedBlocks = 0, nanoseconds = 0, dropped = 0;
		int64_t liveBytes = 0, liveBlocks = 0;
		constexpr std::array<std::string_view, 3> labelScopes{
			"reuse draw list.object labels",
			"reuse draw list.semantic labels",
			"reuse draw list.part labels",
		};
		std::array<double, 3> labelMilliseconds{};
		for (size_t frame = 0; frame < frames; ++frame) {
			core::FrameGraph::BeginFrame();
			const auto before = core::HeapProfile::Totals();
			const auto started = std::chrono::steady_clock::now();
			collect();
			const auto elapsed = std::chrono::steady_clock::now() - started;
			nanoseconds +=
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
			const auto after = core::HeapProfile::Totals();
			allocatedBytes += after.TotalBytes - before.TotalBytes;
			allocatedBlocks += after.TotalBlocks - before.TotalBlocks;
			liveBytes += after.LiveBytes - before.LiveBytes;
			liveBlocks += after.LiveBlocks - before.LiveBlocks;
			core::FrameGraph::EndFrame();
			dropped += core::FrameGraph::Dropped();
			for (const auto &span : core::FrameGraph::Spans()) {
				for (size_t label = 0; label < labelScopes.size(); ++label) {
					if (span.Name == labelScopes[label]) labelMilliseconds[label] += span.Milliseconds;
				}
			}
		}
		std::cout << "presentation-collect-report rows="
				  << fixture.World.Resource<render::DrawList>()->Instances.size()
				  << " attributes_present=" << fixture.World.HasResource<ecs::AttributeTable>()
				  << " frames=" << frames << " profiling=1 heap_hooks=" << core::HeapProfile::IsCompiledIn()
				  << " collect_ns_mean=" << nanoseconds / frames
				  << " allocated_bytes_per_collect=" << allocatedBytes / frames
				  << " allocated_blocks_per_collect=" << allocatedBlocks / frames
				  << " live_bytes_delta=" << liveBytes << " live_blocks_delta=" << liveBlocks
				  << " object_labels_ns_mean=" << labelMilliseconds[0] * 1.0e6 / frames
				  << " semantic_labels_ns_mean=" << labelMilliseconds[1] * 1.0e6 / frames
				  << " part_labels_ns_mean=" << labelMilliseconds[2] * 1.0e6 / frames
				  << " dropped_spans=" << dropped << '\n';
	}

	StaticParts &RebuiltWorld() {
		static StaticParts world;
		return world;
	}

	MovingParts &StressPhysicsWorld() {
		static MovingParts world;
		return world;
	}
}

BENCH("CollectInstances · 1,024 static parts, cached", 10'000) {
	auto &bench = presentation_bench::CachedWorld();
	presentation_bench::ReportCachedCollection(bench);
	for (size_t pass = 0; pass < 10'000; pass++) {
		engine::render::CollectInstances(bench.World);
	}
}

BENCH("CollectInstances · 1,024 static parts, rebuild", 1'000) {
	auto &bench = presentation_bench::RebuiltWorld();
	for (size_t pass = 0; pass < 1'000; pass++) {
		auto visual = *bench.World.Get<engine::scene::Visual>(bench.First);
		visual.Tint.R = (pass & 1u) == 0 ? 0.25f : 0.75f;
		bench.World.Set(bench.First, visual);
		engine::render::CollectInstances(bench.World);
	}
}

BENCH("CollectInstances · 1,024 static parts, pose epoch", 1'000) {
	auto &bench = presentation_bench::CachedWorld();
	for (size_t pass = 0; pass < 1'000; pass++) {
		bench.World.MarkChanged<engine::scene::Transform>(bench.First);
		engine::render::CollectInstances(bench.World);
	}
}

// Models the presentation input writes, not physics integration or GPU residency.
BENCH("StressPhysics presentation · 100,000 transform writes plus collection", 1) {
	presentation_bench::StressPhysicsWorld().AdvanceAndCollect();
}
