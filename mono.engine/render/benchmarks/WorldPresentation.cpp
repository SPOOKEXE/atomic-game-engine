// grug measures whole camera batches over shared world rows. one call means
// every camera signed once, so more cameras cannot hide behind a per-row divisor.
// quiet and one-row edits use the same production path. no device work here.

#include <engine/core/Name.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.render.bench.world-presentation")

namespace world_presentation_bench {
	constexpr size_t BATCHES = 8;
	constexpr size_t MAX_CAMERAS = 32;

	struct WorldRows {
		std::vector<engine::scene::DrawInstance> Instances;
		std::array<engine::render::View, MAX_CAMERAS> Views;
		engine::render::ScenePresentationState State;
		size_t ChangedRow = 0;

		explicit WorldRows(size_t count) : Instances(count) {
			const engine::core::Name world("bench.signature.world");
			for (size_t index = 0; index < count; index++) {
				auto &instance = Instances[index];
				instance.Source = index + 1;
				instance.Frame.Position = {
					static_cast<float>(index % 128),
					static_cast<float>(index / 128),
					-16.0f,
				};
			}
			for (size_t index = 0; index < Views.size(); index++) {
				auto &view = Views[index];
				view.World = 1;
				view.WorldName = world;
				view.Instances = Instances;
				view.CameraFrame.Position.X = static_cast<float>(index);
			}
		}
	};

	WorldRows &RowsOf(size_t count) {
		// grug caps retained fixtures at these two sizes. no allocation per batch.
		static WorldRows small(1'024);
		static WorldRows large(16'384);
		return count == 1'024 ? small : large;
	}

	void SignBatches(size_t instanceCount, size_t cameraCount, bool changeOneRow) {
		WorldRows &world = RowsOf(instanceCount);
		for (size_t batch = 0; batch < BATCHES; batch++) {
			if (changeOneRow) {
				// grug includes one real edit per batch. every camera sees same edit.
				auto &instance = world.Instances[world.ChangedRow];
				instance.Tint.R = instance.Tint.R == 1.0f ? 0.5f : 1.0f;
				world.ChangedRow = (world.ChangedRow + 1) % instanceCount;
			}
			for (size_t camera = 0; camera < cameraCount; camera++) {
				engine::testing::Consume(
					engine::render::ScenePresentationSignaturesOf(world.Views[camera], world.State)
				);
			}
		}
	}
}

using namespace world_presentation_bench;

BENCH("Scene signatures | 1,024 rows | 1 camera | unchanged batch", BATCHES) {
	SignBatches(1'024, 1, false);
}

BENCH("Scene signatures | 1,024 rows | 2 cameras | unchanged batch", BATCHES) {
	SignBatches(1'024, 2, false);
}

BENCH("Scene signatures | 1,024 rows | 8 cameras | unchanged batch", BATCHES) {
	SignBatches(1'024, 8, false);
}

BENCH("Scene signatures | 1,024 rows | 32 cameras | unchanged batch", BATCHES) {
	SignBatches(1'024, 32, false);
}

BENCH("Scene signatures | 1,024 rows | 1 camera | one-row edit batch", BATCHES) {
	SignBatches(1'024, 1, true);
}

BENCH("Scene signatures | 1,024 rows | 2 cameras | one-row edit batch", BATCHES) {
	SignBatches(1'024, 2, true);
}

BENCH("Scene signatures | 1,024 rows | 8 cameras | one-row edit batch", BATCHES) {
	SignBatches(1'024, 8, true);
}

BENCH("Scene signatures | 1,024 rows | 32 cameras | one-row edit batch", BATCHES) {
	SignBatches(1'024, 32, true);
}

BENCH("Scene signatures | 16,384 rows | 1 camera | unchanged batch", BATCHES) {
	SignBatches(16'384, 1, false);
}

BENCH("Scene signatures | 16,384 rows | 2 cameras | unchanged batch", BATCHES) {
	SignBatches(16'384, 2, false);
}

BENCH("Scene signatures | 16,384 rows | 8 cameras | unchanged batch", BATCHES) {
	SignBatches(16'384, 8, false);
}

BENCH("Scene signatures | 16,384 rows | 32 cameras | unchanged batch", BATCHES) {
	SignBatches(16'384, 32, false);
}

BENCH("Scene signatures | 16,384 rows | 1 camera | one-row edit batch", BATCHES) {
	SignBatches(16'384, 1, true);
}

BENCH("Scene signatures | 16,384 rows | 2 cameras | one-row edit batch", BATCHES) {
	SignBatches(16'384, 2, true);
}

BENCH("Scene signatures | 16,384 rows | 8 cameras | one-row edit batch", BATCHES) {
	SignBatches(16'384, 8, true);
}

BENCH("Scene signatures | 16,384 rows | 32 cameras | one-row edit batch", BATCHES) {
	SignBatches(16'384, 32, true);
}
