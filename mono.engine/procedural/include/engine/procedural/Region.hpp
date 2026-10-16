#pragma once

// Pure forest and village placement over copied, bounded spatial facts.
// The owner verifies digests, schedules work and publishes at its fixed barrier.
// @tier L3 · shared

#include <engine/graphcore/Schema.hpp>
#include <engine/procedural/Canonical.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::procedural {

	// One world cell is divided into this many integer coordinate units per axis.
	inline constexpr int64_t SUBCELLS_PER_CELL = 1024;

	// Integer position in fixed subcell units.
	struct Point {
		int64_t X = 0; // Horizontal coordinate in subcells.
		int64_t Y = 0; // Vertical coordinate in subcells.
		// Compares both fixed-coordinate components.
		bool operator==(const Point &) const = default;
	};

	// Half-open rectangle in fixed subcell units, including Min and excluding Max.
	struct Bounds {
		Point Min{}; // Inclusive lower corner.
		Point Max{}; // Exclusive upper corner.
		// Compares both corners exactly.
		bool operator==(const Bounds &) const = default;
	};

	// Identifies a region by its integer cell origin and side length.
	struct RegionScope {
		int32_t X = 0;				// Region coordinate on the horizontal axis.
		int32_t Y = 0;				// Region coordinate on the vertical axis.
		uint16_t CellsPerSide = 16; // Number of world cells on each side.
	};

	// Spatial source categories accepted by a region query.
	enum class SourceKind : uint8_t {
		Village, // Named village anchor.
		Entrance // Named entrance that targets a village.
	};

	// One immutable ECS/spatial fact copied on the owner thread. An entrance
	// names its target village; villages leave TargetVillage empty.
	struct SpatialLeaf {
		SourceKind Kind = SourceKind::Village; // Kind of copied spatial fact.
		std::string Name{};					   // Stable identity used in output and keys.
		Point Anchor{};						   // Position in fixed subcell units.
		std::string TargetVillage{};		   // Stable village identity for entrances.
		Digest ContentDigest{};				   // Opaque digest verified by the host.
	};

	// A host-verified local index witness covers this exact half-open query. It
	// remains present when Matches is empty. Complete and CoverageDigest are
	// claims from the host's bounded spatial index, not self-authentication.
	// The host must derive CoverageDigest from only this local source coverage.
	struct SourceQuery {
		std::string Source{};				// Exactly "villages" or "entrances".
		Bounds Covered{};					// Exact half-open bounds queried.
		Digest CoverageDigest{};			// Opaque host-computed local coverage digest.
		bool Complete = false;				// True only when the bounded query is complete.
		std::vector<SpatialLeaf> Matches{}; // Matching leaves, possibly empty when complete.
	};

	// Hard work, input, and output ceilings for one region request.
	struct GenerationLimits {
		uint32_t MaximumCandidateCells = 65536; // Candidate world cells examined.
		uint32_t MaximumQueryLeaves = 512;		// Source leaves accepted across queries.
		uint32_t MaximumWorkUnits = 8000000;	// Bounded generation work estimate.
		uint32_t MaximumSpatialTests = 4000000; // Pairwise and coverage tests performed.
		uint32_t MaximumPlacements = 4096;		// Placement records returned.
		uint32_t MaximumRoads = 256;			// Road splines returned.
		uint32_t MaximumOutputBytes = 1048576;	// Combined bounded result storage.
	};

	// Generated placement categories.
	enum class PlacementKind : uint8_t {
		Tree,		// Forest instance.
		VillageLot, // Reserved village ground area.
		House,		// Village building instance.
		Entrance	// Placement associated with a named entrance.
	};

	// Stable generated object identity and authored placement choices.
	struct Placement {
		std::string Id{};						  // Stable identity derived before filtering.
		PlacementKind Kind = PlacementKind::Tree; // Generated category.
		std::string SourceName{};				  // Stable source identity, when applicable.
		Point Position{};						  // Position in fixed subcell units.
		uint8_t RotationQuarterTurns = 0;		  // Clockwise quarter turns.
		uint16_t Variant = 0;					  // Generator-selected asset variant index.
		uint16_t Material = 0;					  // Generator-selected material index.
	};

	// Quadratic spline controls, with exact authored entrance and village ends.
	struct RoadSpline {
		std::string Id{};				 // Stable road identity.
		std::string EntranceName{};		 // Stable connected entrance identity.
		std::string VillageName{};		 // Stable connected village identity.
		std::array<Point, 3> Controls{}; // Quadratic controls in fixed subcell units.
	};

	// Authored control for an existing named entrance-to-village road. Endpoints
	// come from the source facts, so moving an entrance does not duplicate them.
	struct AuthoredSpline {
		std::string Name{};			// Stable authored fact identity.
		std::string EntranceName{}; // Existing entrance whose road this controls.
		Point Control{};			// Absolute quadratic middle control in subcell units.
	};

	// Sparse manual edits apply after baseline spacing and do not reseed candidates.
	enum class OverrideAction : uint8_t { Delete, Replace };

	// A tree/x/y or house/x/y stable ID and its complete authored replacement.
	// Move is represented by Replace with the same ID and choices at a new position.
	// A house edit also edits its associated lot/x/y record. Replacements bypass
	// procedural spacing and leave other candidates' original choices unchanged.
	struct PlacementOverride {
		std::string TargetId{};							// Canonical generated target identity.
		OverrideAction Action = OverrideAction::Delete; // Delete or complete replacement.
		Point Position{};								// Replacement position; zero for Delete.
		uint8_t RotationQuarterTurns = 0;				// Replacement clockwise orientation.
		uint16_t Variant = 0;	  // Replacement variant, checked against recipe choices.
		uint16_t Material = 0;	  // Replacement material, checked against recipe choices.
		std::string SourceName{}; // Village attribution for houses, empty for trees/Delete.
	};

	// Immutable spline input and its host-verified canonical content digest.
	struct SplineFact {
		AuthoredSpline Value{}; // Copied authored value.
		Digest ContentDigest{}; // Opaque host-verified digest of EncodeAuthoredSpline.
	};

	// Immutable manual edit and its host-verified canonical content digest.
	struct OverrideFact {
		PlacementOverride Value{}; // Copied authored value.
		Digest ContentDigest{};	   // Opaque host-verified digest of EncodePlacementOverride.
	};

	// Complete local coverage for authored inputs, including an explicit empty witness.
	// Splines are relevant when their expanded influence intersects the owner. Edits
	// are relevant when their home cell or replacement destination intersects the owner.
	struct AuthoredInputQuery {
		Bounds Covered{};					   // Same exact query halo as the two spatial source queries.
		Digest CoverageDigest{};			   // Host-verified digest of the local sorted fact manifest.
		bool Complete = false;				   // Incomplete and absent empty witnesses refuse.
		std::vector<SplineFact> Splines{};	   // Copied relevant spline facts.
		std::vector<OverrideFact> Overrides{}; // Copied relevant sparse placement edits.
	};

	// Complete copied input for deterministic generation of one region.
	struct RegionRequest {
		RegionScope Scope{};				// Region coordinates and cell extent.
		Digest RecipeDigest{};				// Opaque digest verified by the host.
		std::vector<SourceQuery> Queries{}; // Bounded local source coverage witnesses.
		GenerationLimits Limits{};			// Per-request hard ceilings.
		// These host lifecycle values never enter random streams or region keys.
		uint64_t ExpectedSourceRevision = 0; // Host revision expected at publication.
		uint64_t RequestSequence = 0;		 // Host ordering token, excluded from content.
		AuthoredInputQuery Authored{};		 // Complete local authored-input coverage.
	};

	// Bounded work counters reported for a completed generation request.
	struct GenerationFacts {
		uint32_t CandidateCells = 0; // Candidate world cells examined.
		uint32_t WorkUnits = 0;		 // Generation work estimate consumed.
		uint32_t SpatialTests = 0;	 // Spatial comparisons performed.
		uint32_t OutputBytes = 0;	 // Bounded result bytes produced.
	};

	// Complete regional result; KeyBytes are opaque until verified by the host.
	struct RegionResult {
		std::vector<uint8_t> KeyBytes{};	 // Canonical regional key bytes, host-verified.
		std::vector<Placement> Placements{}; // Stable identity-sorted placements.
		std::vector<RoadSpline> Roads{};	 // Stable identity-sorted roads.
		GenerationFacts Facts{};			 // Work counters for this result.
	};

	// One worker-owned control per in-flight call. The host may read progress
	// concurrently; these counters never enter canonical keys or artifacts.
	struct GenerationControl {
		std::atomic_bool StopRequested{false};	 // Host sets this to cancel before publication.
		std::atomic<uint32_t> CandidateCells{0}; // Global cells visited so far.
		std::atomic<uint32_t> WorkUnits{0};		 // Bounded generation work consumed.
		std::atomic<uint32_t> SpatialTests{0};	 // Spatial comparisons performed.
		std::atomic<uint32_t> Placements{0};	 // Output placements assembled so far.
		std::atomic<uint32_t> Roads{0};			 // Output roads assembled so far.
	};

	// Typed defaults for the bounded forest and village generator.
	struct ForestVillageSettings {
		uint32_t ForestDensityPpm = 0;			// Forest density in parts per million.
		uint32_t HouseDensityPpm = 0;			// House density in parts per million.
		uint16_t TreeMinimumDistanceCells = 0;	// Minimum spacing between trees.
		uint16_t HouseMinimumDistanceCells = 0; // Minimum spacing between houses.
		uint16_t VillageRadiusCells = 0;		// Village lot search radius.
		uint16_t EntranceClearanceCells = 0;	// Exclusion radius around entrances.
		uint16_t RoadClearanceCells = 0;		// Exclusion radius around roads.
		uint16_t MaximumRoadReachCells = 0;		// Maximum entrance-to-village reach.
		uint16_t RoadBendCells = 0;				// Maximum lateral road bend.
		uint16_t TreeVariants = 0;				// Available tree variants.
		uint16_t TreeMaterials = 0;				// Available tree materials.
		uint16_t HouseVariants = 0;				// Available house variants.
		uint16_t HouseMaterials = 0;			// Available house materials.
	};

	// Immutable validated recipe and its canonical representation.
	class CompiledRecipe {
	  public:
		// Returns the validated authored definition.
		const Recipe &Definition() const {
			return Definition_;
		}
		// Returns the canonical bytes used for deterministic regional identity.
		const std::vector<uint8_t> &CanonicalBytes() const {
			return CanonicalBytes_;
		}

	  private:
		Recipe Definition_{};
		std::vector<uint8_t> CanonicalBytes_{};
		ForestVillageSettings Settings_{};
		std::array<uint8_t, 6> Stages_{};

		friend Status CompileForestVillage(const Recipe &, CompiledRecipe &, Diagnostic &);
		friend Status
		SplineInfluence(const CompiledRecipe &, const AuthoredSpline &, Point, Point, Bounds &, Diagnostic &);
		friend Status
		ValidatePlacementOverride(const CompiledRecipe &, const PlacementOverride &, Diagnostic &);
		friend Status
		RequiredRegionBounds(const CompiledRecipe &, RegionScope, Bounds &, Bounds &, Diagnostic &);
		friend Status
		BuildRegionKey(const CompiledRecipe &, const RegionRequest &, std::vector<uint8_t> &, Diagnostic &);
		friend Status GenerateRegion(
			const CompiledRecipe &, const RegionRequest &, RegionResult &, Diagnostic &, GenerationControl *
		);
	};

	// Canonical SPL1 bytes for one bounded authored spline. Refusal preserves out.
	Status EncodeAuthoredSpline(const AuthoredSpline &, std::vector<uint8_t> &out, Diagnostic &);
	// Reads canonical SPL1 bytes only; unknown/trailing data refuses transactionally.
	Status DecodeAuthoredSpline(std::span<const uint8_t>, AuthoredSpline &out, Diagnostic &);
	// Canonical OVR1 bytes; Delete has no inactive replacement fields.
	Status EncodePlacementOverride(const PlacementOverride &, std::vector<uint8_t> &out, Diagnostic &);
	// Reads canonical OVR1 bytes only and preserves out on malformed input.
	Status DecodePlacementOverride(std::span<const uint8_t>, PlacementOverride &out, Diagnostic &);
	// Checks canonical target syntax and replacement choices against this recipe.
	Status ValidatePlacementOverride(const CompiledRecipe &, const PlacementOverride &, Diagnostic &);

	// Validates bounded controls and returns their hull expanded for clearance and
	// competitor spacing. The host resolves endpoint names from its ECS facts and
	// intersects both old and new influence with registered owner bounds.
	Status SplineInfluence(
		const CompiledRecipe &,
		const AuthoredSpline &,
		Point entrance,
		Point village,
		Bounds &out,
		Diagnostic &
	);
	// Returns separate half-open home and destination bounds, without joining the
	// intervening area. Delete returns its home cell for both. Refusal preserves both.
	Status OverrideInfluence(const PlacementOverride &, Bounds &home, Bounds &destination, Diagnostic &);

	// Supplies the exact v1 forest and village parameter names and typed defaults.
	Recipe ForestVillageDefaults(std::string name, uint64_t rootSeed);

	// The authored node and port declarations used by the domain compiler.
	// The returned storage is process-lifetime and read-only.
	std::span<const graphcore::NodeSchema> ForestVillageNodeSchemas();

	// Checks the domain schema, canonical bytes and shared graphcore stage order.
	// Refusal preserves out.
	Status CompileForestVillage(const Recipe &recipe, CompiledRecipe &out, Diagnostic &diagnostic);

	// Returns owner and sufficient query halo bounds in fixed subcell units.
	// The host uses both old and new query bounds when moving a source.
	Status RequiredRegionBounds(
		const CompiledRecipe &recipe, RegionScope scope, Bounds &owner, Bounds &query, Diagnostic &diagnostic
	);

	// Canonical regional bytes include recipe bytes and its host-validated digest,
	// sorted relevant local leaves, and complete local coverage witnesses,
	// including empty ones. Request revision and sequence are omitted.
	Status BuildRegionKey(
		const CompiledRecipe &recipe,
		const RegionRequest &request,
		std::vector<uint8_t> &out,
		Diagnostic &diagnostic
	);

	// Generates seeded forest trees, village lots and houses, and connected roads.
	// Candidate names derive from global cells before any filtering. Refusal
	// preserves the complete previous RegionResult. Control is optional and must
	// outlive this call; stop and progress have no effect on canonical bytes.
	Status GenerateRegion(
		const CompiledRecipe &recipe,
		const RegionRequest &request,
		RegionResult &out,
		Diagnostic &diagnostic,
		GenerationControl *control = nullptr
	);
}
