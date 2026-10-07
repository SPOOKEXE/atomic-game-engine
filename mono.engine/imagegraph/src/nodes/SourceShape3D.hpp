#pragma once

// Source Draw Shape 3D preparation and fragment math. The consumer owns triangle rasterization.

#include <engine/imagegraph/Document.hpp>

#include <array>
#include <span>

namespace engine::imagegraph::detail {
	class NodeContext;
	enum class SourceShape3DKind {
		Plane,
		Cube,
		Octahedron,
		Cylinder,
		Cone,
		Capsule,
		Sphere,
		CutSphere,
		Torus
	};
	enum class SourceShape3DTransformKind { Translate, RotateX, RotateY, RotateZ, Scale };
	struct SourceShape3DTransformStep {
		SourceShape3DTransformKind Kind;
		Vector3 Value;
	};
	struct SourceShape3DRecipe {
		uint32_t Width = 0, Height = 0;
		SourceShape3DKind Shape = SourceShape3DKind::Cube;
		// Source matrix_stack_push order, with rotations in degrees.
		std::array<SourceShape3DTransformStep, 7> WorldStack{};
		Vector3 CameraEye{0, 1, 0}, CameraTarget{}, CameraUp{0, 0, -1};
		double OrthographicWidth = 1, OrthographicHeight = 1, Near = 0, Far = 2;
		int64_t Side = 8;
		Vector2 Sides{16, 8}, Radius{.75, .25};
		double Ratio = .5, HeightControl = .5, SideScale = 2;
		bool Caps = true, Smooth = false, ArrayTexture = false;
		Vector2 ViewRange{0, .25}, UVPosition{}, UVScale{1, 1};
		// Borrowed for one evaluation. Palette indexing is per source VB/submesh, wrapping by count.
		std::span<const ElementValue> Colours;
		int64_t Interpolation = 0, Oversample = 0;
		// Consumer binds background and texture from the same admitted processor row.
		// Source reads displacement controls but never submits them to this shader.
	};
	struct SourceShape3DFragment {
		std::array<double, 4> Surface{}, Depth{}, RimNormal{};
	};
	// Never publishes an output or claims a completed render.
	bool BuildSourceShape3DRecipe(NodeContext &context, SourceShape3DRecipe &recipe);
	// Texture sampling, depth test and background over-composition belong to the consumer.
	bool ShadeSourceShape3DFragment(
		const SourceShape3DRecipe &recipe,
		const std::array<double, 4> &texture,
		const std::array<double, 4> &colour,
		const std::array<double, 4> &vertexColour,
		double clipDepth,
		double normalizedViewNormalZ,
		SourceShape3DFragment &fragment
	);
}
