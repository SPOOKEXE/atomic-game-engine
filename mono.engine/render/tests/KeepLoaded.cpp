#include <engine/core/Name.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.render.keep-loaded")

namespace {
	using engine::scene::DrawInstance;

	struct PreparedRows {
		std::span<const DrawInstance> SourceRows;
		std::vector<DrawInstance> OwnedRows;
		std::vector<uint32_t> Hidden;
		bool UsesOwnedRows = false;

		std::span<const DrawInstance> Rows() const {
			return UsesOwnedRows ? std::span<const DrawInstance>(OwnedRows) : SourceRows;
		}
	};

	bool HasNamedMesh(std::span<const DrawInstance> rows) {
		for (const DrawInstance &row : rows) {
			if (row.Mesh.IsValid()) return true;
		}
		return false;
	}

	template <class Resident>
	PreparedRows
	PrepareRows(std::span<const DrawInstance> rows, std::span<const uint32_t> hidden, Resident resident) {
		PreparedRows prepared;
		if (!HasNamedMesh(rows)) {
			prepared.SourceRows = rows;
			prepared.Hidden.assign(hidden.begin(), hidden.end());
			return prepared;
		}

		engine::scene::KeepLoaded(rows, resident, prepared.OwnedRows, hidden, &prepared.Hidden);
		prepared.UsesOwnedRows = true;
		return prepared;
	}

	std::vector<DrawInstance> Rows(size_t count, engine::core::Name mesh = {}) {
		std::vector<DrawInstance> rows(count);
		for (size_t index = 0; index < rows.size(); ++index) {
			rows[index].Source = index + 1;
			rows[index].Mesh = mesh;
		}
		return rows;
	}

	void CheckSameRows(std::span<const DrawInstance> left, std::span<const DrawInstance> right) {
		REQUIRE(left.size() == right.size());
		for (size_t index = 0; index < left.size(); ++index) {
			CHECK(left[index].Source == right[index].Source);
			CHECK(left[index].Mesh == right[index].Mesh);
		}
	}
}

TEST_CASE("unnamed default parts retain their span and hidden row order", "[render][keep-loaded]") {
	const std::vector<DrawInstance> source = Rows(5);
	const std::vector<uint32_t> hidden{0, 2, 4};
	std::vector<DrawInstance> baseline;
	std::vector<uint32_t> baselineHidden;
	size_t baselineResidencyCalls = 0;
	engine::scene::KeepLoaded(
		source,
		[&baselineResidencyCalls](const DrawInstance &) {
			baselineResidencyCalls++;
			return false;
		},
		baseline,
		hidden,
		&baselineHidden
	);

	size_t candidateResidencyCalls = 0;
	PreparedRows prepared = PrepareRows(source, hidden, [&candidateResidencyCalls](const DrawInstance &) {
		candidateResidencyCalls++;
		return false;
	});
	CHECK(prepared.Rows().data() == source.data());
	CheckSameRows(prepared.Rows(), baseline);
	CHECK(prepared.Hidden == baselineHidden);
	CHECK(prepared.Hidden == hidden);
	CHECK(baselineResidencyCalls == 0);
	CHECK(candidateResidencyCalls == 0);
}

TEST_CASE("named resident meshes preserve rows and hidden indices", "[render][keep-loaded]") {
	const engine::core::Name resident("keep-loaded.resident");
	std::vector<DrawInstance> source = Rows(5, resident);
	const std::vector<uint32_t> hidden{0, 2, 4};
	std::vector<DrawInstance> baseline;
	std::vector<uint32_t> baselineHidden;
	auto isResident = [](const DrawInstance &) { return true; };
	engine::scene::KeepLoaded(source, isResident, baseline, hidden, &baselineHidden);

	PreparedRows prepared = PrepareRows(source, hidden, isResident);
	CheckSameRows(prepared.Rows(), baseline);
	CHECK(prepared.Hidden == baselineHidden);
	CHECK(prepared.Hidden == hidden);
}

TEST_CASE("missing named meshes compact rows and hidden indices", "[render][keep-loaded]") {
	const engine::core::Name missing("keep-loaded.missing");
	std::vector<DrawInstance> source = Rows(5, missing);
	const std::vector<uint32_t> hidden{0, 2, 4};
	std::vector<DrawInstance> baseline;
	std::vector<uint32_t> baselineHidden;
	auto isResident = [](const DrawInstance &) { return false; };
	engine::scene::KeepLoaded(source, isResident, baseline, hidden, &baselineHidden);

	PreparedRows prepared = PrepareRows(source, hidden, isResident);
	CheckSameRows(prepared.Rows(), baseline);
	CHECK(prepared.Hidden == baselineHidden);
	CHECK(prepared.Rows().empty());
	CHECK(prepared.Hidden.empty());
}
