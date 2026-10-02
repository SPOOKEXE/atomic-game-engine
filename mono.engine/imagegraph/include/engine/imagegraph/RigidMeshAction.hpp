#pragma once
#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph {
	// Prepares an owned attribute_mesh replacement from one captured object input generation.
	// The authoring host applies it only after this operation succeeds.
	Status PrepareRigidMeshAction(
		const EvaluationSnapshot &inputs,
		uint64_t maximumBytes,
		ArrayValue &replacement,
		Diagnostic &diagnostic
	);
}
