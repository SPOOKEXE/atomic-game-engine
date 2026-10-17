#include <engine/core/Name.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.render.bench.keep-loaded")

namespace {
	using engine::scene::DrawInstance;
	using engine::testing::Consume;
	inline constexpr size_t ROW_COUNT = 100'000;

	struct Rows {
		std::vector<DrawInstance> DefaultParts = std::vector<DrawInstance>(ROW_COUNT);
		std::vector<DrawInstance> ResidentMeshes = std::vector<DrawInstance>(ROW_COUNT);
		std::vector<DrawInstance> MissingMeshes = std::vector<DrawInstance>(ROW_COUNT);
		std::vector<uint32_t> Hidden;
		std::vector<DrawInstance> Filtered;
		std::vector<uint32_t> RetainedHidden;

		Rows() {
			const engine::core::Name resident("keep-loaded.resident");
			const engine::core::Name missing("keep-loaded.missing");
			for (size_t index = 0; index < ROW_COUNT; ++index) {
				DefaultParts[index].Source = index + 1;
				ResidentMeshes[index].Source = index + 1;
				ResidentMeshes[index].Mesh = resident;
				MissingMeshes[index].Source = index + 1;
				MissingMeshes[index].Mesh = missing;
			}
			for (uint32_t index = 0; index < ROW_COUNT; index += 16)
				Hidden.push_back(index);
			Filtered.reserve(ROW_COUNT);
			RetainedHidden.reserve(Hidden.size());
		}
	};

	Rows &Fixture() {
		static Rows rows;
		return rows;
	}

	bool HasNamedMesh(std::span<const DrawInstance> rows) {
		return std::any_of(rows.begin(), rows.end(), [](const DrawInstance &row) {
			return row.Mesh.IsValid();
		});
	}

	void ProbeThenKeepLoaded(
		std::span<const DrawInstance> source,
		std::span<const uint32_t> hidden,
		std::vector<DrawInstance> &filtered,
		std::vector<uint32_t> &retainedHidden,
		bool resident
	) {
		if (!HasNamedMesh(source)) {
			retainedHidden.assign(hidden.begin(), hidden.end());
			Consume(source.data());
			Consume(retainedHidden.data());
			return;
		}
		engine::scene::KeepLoaded(
			source, [resident](const DrawInstance &) { return resident; }, filtered, hidden, &retainedHidden
		);
		Consume(filtered.data());
		Consume(retainedHidden.data());
	}
}

BENCH("KeepLoaded · copy 100k default parts", 1) {
	Rows &rows = Fixture();
	engine::scene::KeepLoaded(
		rows.DefaultParts,
		[](const DrawInstance &) { return false; },
		rows.Filtered,
		rows.Hidden,
		&rows.RetainedHidden
	);
	Consume(rows.Filtered.data());
	Consume(rows.RetainedHidden.data());
}

BENCH("Probe · retain span for 100k default parts", 1) {
	Rows &rows = Fixture();
	ProbeThenKeepLoaded(rows.DefaultParts, rows.Hidden, rows.Filtered, rows.RetainedHidden, false);
}

BENCH("KeepLoaded · copy 100k named resident meshes", 1) {
	Rows &rows = Fixture();
	engine::scene::KeepLoaded(
		rows.ResidentMeshes,
		[](const DrawInstance &) { return true; },
		rows.Filtered,
		rows.Hidden,
		&rows.RetainedHidden
	);
	Consume(rows.Filtered.data());
	Consume(rows.RetainedHidden.data());
}

BENCH("Probe · filter 100k named resident meshes", 1) {
	Rows &rows = Fixture();
	ProbeThenKeepLoaded(rows.ResidentMeshes, rows.Hidden, rows.Filtered, rows.RetainedHidden, true);
}

BENCH("KeepLoaded · drop 100k missing meshes", 1) {
	Rows &rows = Fixture();
	engine::scene::KeepLoaded(
		rows.MissingMeshes,
		[](const DrawInstance &) { return false; },
		rows.Filtered,
		rows.Hidden,
		&rows.RetainedHidden
	);
	Consume(rows.Filtered.data());
	Consume(rows.RetainedHidden.data());
}

BENCH("Probe · filter 100k missing meshes", 1) {
	Rows &rows = Fixture();
	ProbeThenKeepLoaded(rows.MissingMeshes, rows.Hidden, rows.Filtered, rows.RetainedHidden, false);
}
