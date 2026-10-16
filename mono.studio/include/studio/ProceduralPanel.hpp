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
	struct ProceduralAction {
		ProceduralActionKind Kind = ProceduralActionKind::Preview;
		uint64_t Seed = 42;
		std::string Node{};
		std::string Parameter{};
		uint64_t ParameterValue = 0;
		engine::script::ProceduralSource Source{};
		engine::procedural::AuthoredSpline Spline{};
		engine::procedural::PlacementOverride Override{};
		engine::procedural::RegionScope Scope{};
		std::string Output{};
	};
	struct ProceduralPanelState {
		std::array<char, 129> SourceName{"oakstead"};
		std::array<char, 129> TargetVillage{"oakstead"};
		std::array<char, 129> SplineName{"west-road"};
		std::array<char, 129> EntranceName{"west-gate"};
		std::array<char, 129> PlacementId{"tree/0/0"};
		std::array<char, 129> PlacementSource{};
		std::array<char, 257> Output{"forest-village.aproc"};
		std::array<int64_t, 2> SourceCell{8, 8};
		std::array<int64_t, 2> ControlCell{10, 9};
		std::array<int64_t, 2> PlacementCell{0, 0};
		int SourceKind = 0;
		int OverrideKind = 0;
		int Rotation = 0;
		int Variant = 0;
		int Material = 0;
		int RegionIndex = 0;
		float Orbit = 0.6f;
		engine::procedural::RegionScope ImageScope{}; // Scope depicted by the retained image.
		bool HasImageScope = false;
		std::string Status{};
		// Background sync or pump failures stay visible until a later successful retry.
		bool HasPreviewDiagnostic = false;
		std::vector<ProceduralAction> Pending{};
		// Queue is bounded before copying any action-owned strings.
		bool Queue(const ProceduralAction &);
	};
	bool IsProceduralAuthoringAction(ProceduralActionKind);
	bool ApplyProceduralAuthoringAction(
		engine::ecs::Store &, const ProceduralAction &, engine::procedural::Diagnostic &
	);
	void DrawProceduralPanel(
		ProceduralPanelState &,
		bool &open,
		const engine::ecs::Store *source,
		const ProceduralEditor *preview,
		void *retainedImage
	);
}
