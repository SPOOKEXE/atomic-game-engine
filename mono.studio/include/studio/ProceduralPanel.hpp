#pragma once

// Fixed forest/village authoring controls. Draw only reads ECS facts; bounded
// actions are applied by the editor after every panel has finished drawing.
#include <engine/script/ProceduralInputs.hpp>
#include <engine/script/ProceduralJobs.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace studio {
	class ProceduralEditor;
	// Identifies a procedural authoring edit or a preview, activation, bake, or publish request.
	enum class ProceduralActionKind : uint8_t {
		Initialize,
		Seed,
		Parameter,
		SetSource,
		RemoveSource,
		SetSpline,
		RemoveSpline,
		SetOverride,
		RemoveOverride,
		Preview,
		Cancel,
		Activate,
		Bake,
		Publish
	};
	// One bounded request captured by the panel and applied after panel drawing completes.
	struct ProceduralAction {
		// Requested operation.
		ProceduralActionKind Kind = ProceduralActionKind::Preview;
		// Recipe seed used by initialization or seed changes.
		uint64_t Seed = 42;
		// Stable recipe node id for a parameter edit.
		std::string Node{};
		// Parameter name for a recipe edit.
		std::string Parameter{};
		// Integer value supplied for the named parameter.
		uint64_t ParameterValue = 0;
		// Source fact to set or identify for removal.
		engine::script::ProceduralSource Source{};
		// Spline fact to set or identify for removal.
		engine::procedural::AuthoredSpline Spline{};
		// Placement override to set or identify for removal.
		engine::procedural::PlacementOverride Override{};
		// Region selected for baking or publishing.
		engine::procedural::RegionScope Scope{};
		// Requested baked artifact path.
		std::string Output{};
	};
	// Editable controls, retained preview state, and queued work for the procedural panel.
	struct ProceduralPanelState {
		// Name of the source fact being edited.
		std::array<char, 129> SourceName{"oakstead"};
		// Village targeted by an entrance source.
		std::array<char, 129> TargetVillage{"oakstead"};
		// Name of the road control being edited.
		std::array<char, 129> SplineName{"west-road"};
		// Entrance associated with the road control.
		std::array<char, 129> EntranceName{"west-gate"};
		// Stable placement id for an override.
		std::array<char, 129> PlacementId{"tree/0/0"};
		// Replacement source name for an override.
		std::array<char, 129> PlacementSource{};
		// Output path for a bake or publish request.
		std::array<char, 257> Output{"forest-village.aproc"};
		// Source X/Z position in authored cells.
		std::array<int64_t, 2> SourceCell{8, 8};
		// Spline control X/Z position in authored cells.
		std::array<int64_t, 2> ControlCell{10, 9};
		// Override X/Z position in authored cells.
		std::array<int64_t, 2> PlacementCell{0, 0};
		// Source kind selection: village or entrance.
		int SourceKind = 0;
		// Override action selection: delete or replace.
		int OverrideKind = 0;
		// Replacement rotation in quarter turns.
		int Rotation = 0;
		// Replacement variant index.
		int Variant = 0;
		// Replacement material index.
		int Material = 0;
		// Selected region in the authored authority.
		int RegionIndex = 0;
		// Preview camera orbit angle in radians.
		float Orbit = 0.6f;
		engine::procedural::RegionScope ImageScope{}; // Scope depicted by the retained image.
		// Whether ImageScope identifies the retained image.
		bool HasImageScope = false;
		// Latest panel action or preview status shown to the user.
		std::string Status{};
		// Background sync or pump failures stay visible until a later successful retry.
		bool HasPreviewDiagnostic = false;
		// Queued actions awaiting editor application.
		std::vector<ProceduralAction> Pending{};
		// Queue is bounded before copying any action-owned strings.
		bool Queue(const ProceduralAction &);
	};
	// Returns whether an action changes authored source, spline, or placement data.
	bool IsProceduralAuthoringAction(ProceduralActionKind);
	// Applies one authoring action to the store and reports validation or application failures.
	bool ApplyProceduralAuthoringAction(
		engine::ecs::Store &, const ProceduralAction &, engine::procedural::Diagnostic &
	);
	// Draws the panel from optional source facts, preview progress, and a retained preview image.
	void DrawProceduralPanel(
		ProceduralPanelState &,
		bool &open,
		const engine::ecs::Store *source,
		const ProceduralEditor *preview,
		void *retainedImage
	);
}
