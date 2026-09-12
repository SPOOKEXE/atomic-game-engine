#pragma once

// arch-waiver public-header: forward examples API. Programs and tooling can
// install this complete pipeline recipe without duplicating its graph edits.

// The render pipeline paired with RenderFeaturesDemo.luau.
//
// It starts from the production PBR graph, replaces the frame tail with two
// lazy visual attachment stages and FXAA, then presents through the ordinary
// interface compositor. The checked-in text asset is verified against this
// recipe so Studio users and tests exercise the same graph.
//
// @tier L10 · shared

#include <engine/graph/PipelineDocument.hpp>

namespace engine::examples {

	// Builds the inspectable render-features example pipeline.
	graph::PipelineDocument RenderFeaturesDemoPipeline();
}
