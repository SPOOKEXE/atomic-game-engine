#pragma once

#include "Source2DGenerator.hpp"

namespace engine::imagegraph::detail::source2d {
	// Source getters resolve Reference units before processor row selection, against Dimension row zero.
	inline std::optional<double> ReferenceCoordinate(const ElementValue &v) {
		if (const auto *d = std::get_if<double>(&v)) return *d;
		if (const auto *i = std::get_if<int64_t>(&v)) return double(*i);
		return std::nullopt;
	}
	inline std::optional<Vector2> ReferenceItem(const SourceArrayItem &item, size_t depth) {
		if (depth > Limits::MaximumArrayDepth) return std::nullopt;
		if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
			if (const auto *point = std::get_if<Vector2>(leaf)) return *point;
			if (const auto number = ReferenceCoordinate(*leaf)) return Vector2{*number, *number};
			return std::nullopt;
		}
		const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
		if (!children || children->empty()) return std::nullopt;
		if (children->size() >= 2) {
			const auto *a = std::get_if<ElementValue>(&(*children)[0].Data),
					   *b = std::get_if<ElementValue>(&(*children)[1].Data);
			const auto x = a ? ReferenceCoordinate(*a) : std::nullopt,
					   y = b ? ReferenceCoordinate(*b) : std::nullopt;
			if (x && y) return Vector2{*x, *y};
		}
		return ReferenceItem(children->front(), depth + 1);
	}
	inline std::optional<size_t> ReferenceImageIndex(const ImageArrayItem &item, size_t depth) {
		if (depth > Limits::MaximumArrayDepth) return std::nullopt;
		if (const auto *index = std::get_if<size_t>(&item.Data)) return *index;
		const auto *children = std::get_if<std::vector<ImageArrayItem>>(&item.Data);
		if (!children || children->empty()) return std::nullopt;
		return ReferenceImageIndex(children->front(), depth + 1);
	}
	inline const Image *FirstReferenceImage(const ImageArray &array) {
		if (array.Items.empty()) return array.Images.empty() ? nullptr : &array.Images.front();
		const auto index = ReferenceImageIndex(array.Items.front(), 0);
		return index && *index < array.Images.size() ? &array.Images[*index] : nullptr;
	}
	inline bool ResolveReferenceDimension(NodeContext &c, Vector2 current, Vector2 &reference) {
		reference = current;
		if (c.IsLinked("dimension"))
			for (const auto &[port, images] : c.ImageArrays)
				if (port == "dimension" && images) {
					const auto *first = FirstReferenceImage(*images);
					if (!first)
						return c.Fail(
							Status::InvalidValue, "Reference Dimension has no first source image", "dimension"
						);
					reference = {double(first->Width), double(first->Height)};
					return true;
				}
		const auto *original = GeneratorOriginal(c, "dimension");
		const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr;
		if (!array) return true;
		std::optional<Vector2> first;
		if (!array->Elements.empty()) {
			if (const auto *point = std::get_if<Vector2>(&array->Elements.front()))
				first = *point;
			else if (array->Elements.size() >= 2) {
				const auto x = ReferenceCoordinate(array->Elements[0]),
						   y = ReferenceCoordinate(array->Elements[1]);
				if (x && y) first = Vector2{*x, *y};
			}
		} else if (!array->Nested.empty() && array->Nested.front().size() >= 2) {
			const auto x = ReferenceCoordinate(array->Nested.front()[0]),
					   y = ReferenceCoordinate(array->Nested.front()[1]);
			if (x && y) first = Vector2{*x, *y};
		} else if (!array->Items.empty())
			first = ReferenceItem(array->Items.front(), 0);
		if (!first)
			return c.Fail(
				Status::UnsupportedExecution,
				"Reference units require the first prepared Dimension tuple",
				"dimension"
			);
		reference = *first;
		if (!c.IsLinked("dimension")) {
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit == 1) {
				reference.X *= c.Project.SurfaceWidth;
				reference.Y *= c.Project.SurfaceHeight;
			}
			if (unit == 2) {
				const Image *mask = c.Input("mask");
				for (const auto &[port, images] : c.ImageArrays)
					if (port == "mask" && images) mask = FirstReferenceImage(*images);
				if (!mask) return c.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
				reference.X *= mask->Width;
				reference.Y *= mask->Height;
			}
		}
		return true;
	}
	inline bool ResolveFirstReferenceDimension(NodeContext &c, Vector2 &reference) {
		auto current = c.Vec2("dimension", {1, 1});
		if (!c.IsLinked("dimension")) {
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit == 1) {
				current.X *= c.Project.SurfaceWidth;
				current.Y *= c.Project.SurfaceHeight;
			}
			if (unit == 2) {
				const Image *mask = c.Input("mask");
				if (!mask) return c.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
				current.X *= mask->Width;
				current.Y *= mask->Height;
			}
		}
		return ResolveReferenceDimension(c, current, reference);
	}

}
