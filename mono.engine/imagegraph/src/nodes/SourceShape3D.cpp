#include "SourceShape3D.hpp"

#include "../MeshPayload.hpp"
#include "../NodeExecutors.hpp"
#include "Processor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace source_shape3d_recipe {
		template <class T> bool VectorControl(NodeContext &context, std::string_view port, T &vector) {
			const Value *value = context.Find(port);
			if (!value) return true;
			if (const auto *typed = std::get_if<T>(value))
				vector = *typed;
			else if (const auto *scalar = std::get_if<double>(value)) {
				if constexpr (std::is_same_v<T, Vector2>)
					vector = {*scalar, *scalar};
				else
					vector = {*scalar, *scalar, *scalar};
			} else if (const auto *integer = std::get_if<int64_t>(value)) {
				const double number = static_cast<double>(*integer);
				if constexpr (std::is_same_v<T, Vector2>)
					vector = {number, number};
				else
					vector = {number, number, number};
			} else
				return context.Fail(Status::TypeMismatch, "Draw Shape 3D requires a numeric vector", port);
			return MeshFinite(vector) ||
				   context.Fail(Status::InvalidValue, "Draw Shape 3D vector must be finite", port);
		}
	}

	bool BuildSourceShape3DRecipe(NodeContext &context, SourceShape3DRecipe &recipe) {
		ENGINE_PROFILE("imagegraph.shape3d.prepare");
		SourceShape3DRecipe prepared;
		Vector2 dimension{1, 1};
		if (!source_shape3d_recipe::VectorControl(context, "dimension", dimension)) return false;
		if (!ResolveDimension(context, "dimension", prepared.Width, prepared.Height)) return false;
		if (prepared.Width > context.Request.MaximumImageDimension ||
			prepared.Height > context.Request.MaximumImageDimension)
			return context.Fail(
				Status::LimitExceeded, "Draw Shape 3D dimensions exceed request bounds", "dimension"
			);
		Vector2 position{.5, .5};
		Vector3 anchor{.5, .5, .5}, rotation{30, 0, 45}, scale{.5, .5, .5};
		if (!source_shape3d_recipe::VectorControl(context, "position", position) ||
			!source_shape3d_recipe::VectorControl(context, "anchor", anchor) ||
			!source_shape3d_recipe::VectorControl(context, "rotation", rotation) ||
			!source_shape3d_recipe::VectorControl(context, "scale", scale) ||
			!source_shape3d_recipe::VectorControl(context, "side_2", prepared.Sides) ||
			!source_shape3d_recipe::VectorControl(context, "radius", prepared.Radius) ||
			!source_shape3d_recipe::VectorControl(context, "view_range", prepared.ViewRange) ||
			!source_shape3d_recipe::VectorControl(context, "uv_position", prepared.UVPosition) ||
			!source_shape3d_recipe::VectorControl(context, "uv_scale", prepared.UVScale))
			return false;
		const int64_t positionUnit = context.Integer("position_unit", 1);
		if (positionUnit < 0 || positionUnit > 1)
			return context.Fail(
				Status::InvalidValue, "Draw Shape 3D position unit is invalid", "position_unit"
			);
		if (positionUnit == 1) position = {position.X * prepared.Width, position.Y * prepared.Height};
		if (!MeshFinite(position))
			return context.Fail(
				Status::InvalidValue, "Draw Shape 3D position exceeds finite units", "position"
			);
		const Vector3 translation{
			-(position.X - prepared.Width / 2.0) / prepared.Width,
			0,
			-(position.Y - prepared.Height / 2.0) / prepared.Height
		};
		const Vector3 pivot{(anchor.X - .5) * .5, (anchor.Y - .5) * .5, (anchor.Z - .5) * .5};
		prepared.WorldStack = {
			{{SourceShape3DTransformKind::Translate, translation},
			 {SourceShape3DTransformKind::Translate, pivot},
			 {SourceShape3DTransformKind::RotateX, {rotation.X, 0, 0}},
			 {SourceShape3DTransformKind::RotateY, {0, rotation.Y, 0}},
			 {SourceShape3DTransformKind::RotateZ, {0, 0, -rotation.Z}},
			 {SourceShape3DTransformKind::Translate, {-pivot.X, -pivot.Y, -pivot.Z}},
			 {SourceShape3DTransformKind::Scale, scale}}
		};
		const Value *shapeValue = context.Find("shape");
		const auto *shape = shapeValue ? std::get_if<std::string>(shapeValue) : nullptr;
		if (shapeValue && !shape)
			return context.Fail(Status::TypeMismatch, "Draw Shape 3D shape must be text", "shape");
		constexpr std::array<std::string_view, 9> NAMES{
			"Plane", "Cube", "Octahedron", "Cylinder", "Cone", "Capsule", "Sphere", "Cut Sphere", "Torus"
		};
		const auto found = std::find(NAMES.begin(), NAMES.end(), shape ? *shape : "Cube");
		if (found == NAMES.end())
			return context.Fail(Status::UnsupportedExecution, "Draw Shape 3D shape is unknown", "shape");
		prepared.Shape = static_cast<SourceShape3DKind>(found - NAMES.begin());
		prepared.Side = context.Integer("side", 8);
		prepared.Caps = context.Boolean("caps", true);
		prepared.Smooth = context.Boolean("smooth", false);
		prepared.ArrayTexture = context.Boolean("array_texture", false);
		prepared.Ratio = context.Scalar("ratio", .5);
		prepared.HeightControl = context.Scalar("height", .5);
		prepared.SideScale = context.Scalar("side_scale", 2);
		prepared.Interpolation = context.Integer("interpolate", 0);
		prepared.Oversample = context.Integer("oversample", 0);
		if (context.FailureCode != Status::Ok) return false;
		for (const auto &[port, number] :
			 {std::pair<std::string_view, double>{"ratio", prepared.Ratio},
			  {"height", prepared.HeightControl},
			  {"side_scale", prepared.SideScale}})
			if (!std::isfinite(number))
				return context.Fail(Status::InvalidValue, "Draw Shape 3D control must be finite", port);
		if (prepared.ViewRange.X == prepared.ViewRange.Y)
			return context.Fail(
				Status::InvalidValue, "Draw Shape 3D view range has zero width", "view_range"
			);
		if (prepared.Side < 3 || prepared.Side > 4096 || prepared.Sides.X < 3 || prepared.Sides.Y < 2 ||
			prepared.Sides.X > 4096 || prepared.Sides.Y > 4096 ||
			prepared.Sides.X != std::trunc(prepared.Sides.X) ||
			prepared.Sides.Y != std::trunc(prepared.Sides.Y))
			return context.Fail(
				Status::LimitExceeded, "Draw Shape 3D subdivisions exceed bounded geometry", "side_2"
			);
		if (prepared.Interpolation < 0 || prepared.Interpolation > 4 || prepared.Oversample < 0 ||
			prepared.Oversample > 12)
			return context.Fail(
				Status::InvalidValue, "Draw Shape 3D sampling selector is invalid", "interpolate"
			);
		uint64_t vertexUpperBound = 36;
		switch (prepared.Shape) {
		case SourceShape3DKind::Plane:
			vertexUpperBound = 6;
			break;
		case SourceShape3DKind::Octahedron:
			vertexUpperBound = 24;
			break;
		case SourceShape3DKind::Cylinder:
			vertexUpperBound = uint64_t(prepared.Side) * (prepared.Caps ? 12 : 6);
			break;
		case SourceShape3DKind::Cone:
			vertexUpperBound = uint64_t(prepared.Side) * 6;
			break;
		case SourceShape3DKind::Capsule:
			vertexUpperBound = uint64_t(prepared.Side) * prepared.Side * 12 + uint64_t(prepared.Side) * 6;
			break;
		case SourceShape3DKind::Sphere:
			vertexUpperBound = uint64_t(prepared.Sides.X) * uint64_t(prepared.Sides.Y) * 6;
			break;
		case SourceShape3DKind::CutSphere: {
			if (prepared.Ratio <= 0)
				return context.Fail(
					Status::UnsupportedExecution, "source Cut Sphere requires its prior cached model", "ratio"
				);
			const double rows = std::ceil(prepared.Sides.X * prepared.Ratio);
			if (!std::isfinite(rows) || rows > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Cut Sphere rows exceed bounded geometry", "ratio"
				);
			vertexUpperBound = uint64_t(prepared.Sides.Y) * (uint64_t(rows) * 6 + 3);
			break;
		}
		case SourceShape3DKind::Cube:
			break;
		case SourceShape3DKind::Torus:
			// grug source model reads sideT/sideP, ignoring node's hori/vert controls.
			vertexUpperBound = 16 * 8 * 6;
			break;
		}
		if (vertexUpperBound > Limits::MaximumArrayElements ||
			vertexUpperBound > Limits::MaximumArrayBytes / sizeof(MeshVertex3D))
			return context.Fail(
				Status::LimitExceeded, "Draw Shape 3D geometry exceeds mesh payload bounds", "side_2"
			);
		static const std::array<ElementValue, 1> WHITE{Colour{255, 255, 255, 255}};
		prepared.Colours = WHITE;
		if (const Value *value = context.Find("colors")) {
			const auto *palette = std::get_if<ArrayValue>(value);
			if (!palette)
				return context.Fail(Status::TypeMismatch, "Draw Shape 3D colors require a palette", "colors");
			if (palette->ElementType != ValueType::Colour || !palette->Nested.empty() ||
				!palette->Items.empty() || palette->Elements.empty() ||
				palette->Elements.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::InvalidValue,
					"Draw Shape 3D palette must be a nonempty flat colour array",
					"colors"
				);
			for (const auto &colour : palette->Elements)
				if (!std::holds_alternative<Colour>(colour))
					return context.Fail(
						Status::TypeMismatch, "Draw Shape 3D palette contains a non-colour", "colors"
					);
			prepared.Colours = palette->Elements;
		}
		recipe = prepared;
		return true;
	}

	bool ShadeSourceShape3DFragment(
		const SourceShape3DRecipe &recipe,
		const std::array<double, 4> &texture,
		const std::array<double, 4> &colour,
		const std::array<double, 4> &vertexColour,
		double clipDepth,
		double normalizedViewNormalZ,
		SourceShape3DFragment &fragment
	) {
		if (!std::isfinite(clipDepth) || !std::isfinite(normalizedViewNormalZ) ||
			!MeshFinite(recipe.ViewRange) || recipe.ViewRange.X == recipe.ViewRange.Y)
			return false;
		SourceShape3DFragment shaded;
		const double depth = (clipDepth - recipe.ViewRange.X) / (recipe.ViewRange.Y - recipe.ViewRange.X);
		const double rim = std::abs(normalizedViewNormalZ);
		if (!std::isfinite(depth)) return false;
		for (size_t channel = 0; channel < 4; ++channel) {
			if (!std::isfinite(texture[channel]) || !std::isfinite(colour[channel]) ||
				!std::isfinite(vertexColour[channel]))
				return false;
			shaded.Surface[channel] = texture[channel] * colour[channel] * vertexColour[channel];
			if (!std::isfinite(shaded.Surface[channel])) return false;
		}
		shaded.Depth = {depth, depth, depth, texture[3]};
		shaded.RimNormal = {rim, rim, rim, texture[3]};
		fragment = shaded;
		return true;
	}
}
