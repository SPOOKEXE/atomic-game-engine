#pragma once

#include "Stages.hpp"

#include <engine/ang/Compile.hpp>
#include <engine/procedural/Region.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace engine::procedural::detail {

	struct QueryView {
		const SourceQuery *Villages = nullptr;
		const SourceQuery *Entrances = nullptr;
		std::vector<const SpatialLeaf *> VillageLeaves{};
		std::vector<const SpatialLeaf *> EntranceLeaves{};
		std::vector<const SplineFact *> SplineLeaves{};
		std::vector<const OverrideFact *> OverrideLeaves{};
	};

	// Chord endpoints of one road, evaluated lazily and shared by both placement stages.
	struct ChordCache {
		std::array<Point, 16> Endpoints{};
		size_t Computed = 0;
	};
	struct LocalRoad {
		RoadSpline Spline;
		std::optional<ChordCache> Chords;
	};

	// One region request moving through the recipe graph. GenerateRegion owns
	// it for the call; each graph node reaches it through its "run" input.
	struct RegionRun {
		const ForestVillageSettings *Settings = nullptr;
		const RegionRequest *Request = nullptr;
		const QueryView *Queries = nullptr;
		GenerationControl *Control = nullptr;
		Bounds Owner{};
		uint64_t RootSeed = 0;
		// Set by the seed and sources stages for the stages after them.
		uint64_t Seed = 0;
		const QueryView *Sources = nullptr;
		std::vector<LocalRoad> Roads{};
		RegionResult Result{};
		uint32_t CompletedCandidates = 0;
		// The first refusing stage's status and diagnostic.
		Status Code = Status::Ok;
		Diagnostic Failure{};
	};

	bool Contains(Bounds bounds, Point point);
	Status
	ValidateQueries(const RegionRequest &request, Bounds required, QueryView &out, Diagnostic &diagnostic);
	Status ValidateAuthoredInputs(
		const RegionRequest &,
		Bounds owner,
		Bounds required,
		const ForestVillageSettings &,
		QueryView &,
		Diagnostic &
	);
	bool Intersects(Bounds left, Bounds right);
	bool OverrideTarget(std::string_view, PlacementKind &, Point &cell);

	// Index of an authored operation kind, or KindCount when unknown.
	inline constexpr size_t KindCount = 6;
	size_t KindIndex(std::string_view kind);

	// Runs one stage of a region request. Refusal records into run.Code and run.Failure.
	Status RunStage(Stage stage, RegionRun &run);

	// Compiles the authored operations and links into a Tick graph whose nodes
	// call RunStage. Refusal preserves out.
	Status CompileRecipeGraph(const Recipe &recipe, std::shared_ptr<const ang::Plan> &out, Diagnostic &);

	// Runs the compiled graph for one request in dependency order.
	Status RunRecipeGraph(const ang::Plan &plan, RegionRun &run);
}
