#pragma once
#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph::detail {
	struct RigidFracturePiece {
		Vector2 Origin;
		Image Texture;
		std::vector<Vector2> Points;
	};
	struct RigidFractureSettings {
		double Threshold = .1, Expansion = 0;
	};
	Status SourceRigidFracture(
		const Image &base,
		const Image &map,
		const RigidFractureSettings &settings,
		uint64_t maximumBytes,
		std::vector<RigidFracturePiece> &pieces,
		Diagnostic &diagnostic
	);
}
namespace engine::imagegraph::detail {
	// Generates the owned source object mesh action without mutating its authored attribute.
	Status SourceRigidGenerateObjectMesh(
		const Image &texture,
		double expansion,
		bool addPixelForEmpty,
		uint64_t maximumBytes,
		std::vector<Vector2> &mesh,
		Diagnostic &diagnostic
	);
}
