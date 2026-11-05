#pragma once

#include "PointerDecision.hpp"

#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Input.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace engine::gui::detail {
	using core::Vector2;
	using ecs::Entity;
	using ecs::NULL_ENTITY;

	inline Vector2 PointerUnrotated(float degrees, const core::Rect &bounds, Vector2 point) {
		if (degrees == 0) return point;
		const Vector2 pivot{(bounds.Min.X + bounds.Max.X) * .5f, (bounds.Min.Y + bounds.Max.Y) * .5f};
		const float angle = -degrees * (3.14159265f / 180.0f);
		const float sine = std::sin(angle), cosine = std::cos(angle);
		const float x = point.X - pivot.X, y = point.Y - pivot.Y;
		return {pivot.X + x * cosine - y * sine, pivot.Y + x * sine + y * cosine};
	}
	template <class Access> Vector2 PointerLogical(const Access &access, Entity collector, Vector2 point) {
		for (const auto &transform : access.Transforms()) {
			if (transform.Collector != collector) continue;
			if (!(transform.Scale.X > 0) || !(transform.Scale.Y > 0)) return point;
			return {
				(point.X - transform.Origin.X) / transform.Scale.X,
				(point.Y - transform.Origin.Y) / transform.Scale.Y
			};
		}
		return point;
	}
	template <class Access>
	bool PointerCollectorAllowed(const Access &access, const Pointer &pointer, Entity collector) {
		return (pointer.Collector == NULL_ENTITY || pointer.Collector == collector) &&
			   (!pointer.ScreenOnly || !access.IsSpatial(collector));
	}
	template <class Access, class Command>
	bool PointerContains(const Access &access, const Command &command, Vector2 point) {
		const Vector2 logical = PointerLogical(access, command.Collector, point);
		auto bounds = command.Bounds;
		if (access.IsLink(command.Source) && bounds.Height() < 8.0f) {
			const float padding = (8.0f - bounds.Height()) * .5f;
			bounds.Min.Y -= padding;
			bounds.Max.Y += padding;
		}
		return bounds.Contains(PointerUnrotated(command.Rotation, command.Bounds, logical)) &&
			   command.Clip.Contains(logical);
	}
	inline bool PointerScrolls(const Scrolling &scrolling, const ScrollState &state, bool vertical) {
		const auto axis = static_cast<uint8_t>(vertical ? ScrollingDirection::Y : ScrollingDirection::X);
		return scrolling.Enabled && (static_cast<uint8_t>(scrolling.Direction) & axis) &&
			   (vertical ? state.CanvasSize.Y > state.WindowSize.Y : state.CanvasSize.X > state.WindowSize.X);
	}
	inline bool PointerElastic(const Scrolling &scrolling, const ScrollState &state, bool vertical) {
		if (scrolling.Elastic == ElasticBehavior::Never) return false;
		if (scrolling.Elastic == ElasticBehavior::Always) return true;
		return PointerScrolls(scrolling, state, vertical);
	}
	// Fields used when a displayed scrollbar or canvas gesture starts.
	template <class Access> PointerScrollWitness PointerScrollState(const Access &access, Entity node) {
		PointerScrollWitness result;
		if (const auto *scroll = access.ScrollingOf(node)) {
			result.Flags |= 1;
			if (scroll->Enabled) result.Flags |= 2;
			result.CanvasPosition = scroll->CanvasPosition;
			result.Direction = static_cast<uint8_t>(scroll->Direction);
			result.Elastic = static_cast<uint8_t>(scroll->Elastic);
		}
		if (const auto *state = access.ScrollStateOf(node)) {
			result.Flags |= 4;
			result.CanvasSize = state->CanvasSize;
			result.WindowSize = state->WindowSize;
			result.VerticalThumb = state->VerticalThumb;
			result.HorizontalThumb = state->HorizontalThumb;
		}
		if (const auto *motion = access.ScrollMotionOf(node)) {
			result.Flags |= 8;
			result.Overshoot = motion->Overshoot;
		}
		if (const auto *resolved = access.ResolvedOf(node)) {
			result.Flags |= 16;
			result.Position = resolved->AbsolutePosition;
			result.Size = resolved->AbsoluteSize;
			result.Rotation = resolved->AbsoluteRotation;
		}
		return result;
	}
	struct VisiblePick {
		Entity Source, Collector, Collection;
		std::string_view Key;
		uint32_t Index = UINT32_MAX;
		core::Rect Bounds, Clip;
		Vector2 Point;
		float Rotation = 0.0f;
		size_t Order = 0;
		bool operator==(const VisiblePick &) const = default;
	};
	// Authoring selects drawn commands even when they do not accept game input.
	template <class Access> VisiblePick PointerVisible(const Access &access, const Pointer &pointer) {
		if (!pointer.Inside) return {};
		for (size_t index = access.CommandCount(); index > 0; --index) {
			const auto &command = access.CommandAt(index - 1);
			if (!PointerCollectorAllowed(access, pointer, command.Collector)) continue;
			const auto point = PointerLogical(access, command.Collector, pointer.Position);
			if (!command.Bounds.Contains(PointerUnrotated(command.Rotation, command.Bounds, point)) ||
				!command.Clip.Contains(point))
				continue;
			return {
				command.Source,
				command.Collector,
				command.Collection,
				command.Key,
				command.Index,
				command.Bounds,
				command.Clip,
				point,
				command.Rotation,
				index
			};
		}
		return {};
	}
	struct BarPick {
		Entity Target;
		float Grab = 0;
		bool Vertical = false;
		size_t Index = SIZE_MAX;
	};
	struct DragPick {
		Entity Detector, Target;
		Vector2 Point;
		core::UDim2 Start;
		float Angle = 0;
		size_t Index = SIZE_MAX;
	};
	struct CanvasPick {
		Entity Target;
		Vector2 Point;
		size_t Index = SIZE_MAX;
	};
	struct WheelPick {
		Entity Target;
		Vector2 Origin, Point;
		bool Graph = false;
	};
	template <class Access> Entity PointerPick(const Access &a, const Pointer &p) {
		if (!p.Inside) return NULL_ENTITY;
		for (size_t i = a.CommandCount(); i > 0; --i) {
			const auto &c = a.CommandAt(i - 1);
			if (!PointerCollectorAllowed(a, p, c.Collector) || !PointerContains(a, c, p.Position)) continue;
			const auto reach = a.Reach(c.Source);
			if (reach == 2) return a.Allows(c.Source, a.Collector(c.Source)) ? c.Source : NULL_ENTITY;
			if (reach == 1) return NULL_ENTITY;
		}
		return NULL_ENTITY;
	}
	template <class Access> BarPick PointerBar(const Access &a, const Pointer &p) {
		for (size_t i = a.CommandCount(); i > 0; --i) {
			const auto &c = a.CommandAt(i - 1);
			if (!PointerCollectorAllowed(a, p, c.Collector) || !a.Allows(c.Source, a.Collector(c.Source)))
				continue;
			const auto *scroll = a.ScrollingOf(c.Source);
			const auto *state = a.ScrollStateOf(c.Source);
			if (!scroll || !state || !a.ResolvedOf(c.Source) || !scroll->Enabled) continue;
			const auto logical = PointerLogical(a, c.Collector, p.Position);
			if (!c.Clip.Contains(logical)) continue;
			const auto point = PointerUnrotated(c.Rotation, c.Bounds, logical);
			if (state->VerticalThumb.Contains(point))
				return {c.Source, point.Y - state->VerticalThumb.Min.Y, true, i - 1};
			if (state->HorizontalThumb.Contains(point))
				return {c.Source, point.X - state->HorizontalThumb.Min.X, false, i - 1};
		}
		return {};
	}
	template <class Access> DragPick PointerDrag(const Access &a, const Pointer &p) {
		for (size_t i = a.CommandCount(); i > 0; --i) {
			const auto &c = a.CommandAt(i - 1);
			if (!PointerCollectorAllowed(a, p, c.Collector) || !a.Allows(c.Source, a.Collector(c.Source)))
				continue;
			const auto point = PointerLogical(a, c.Collector, p.Position);
			if (!c.Bounds.Contains(PointerUnrotated(c.Rotation, c.Bounds, point)) || !c.Clip.Contains(point))
				continue;
			const auto *element = a.ElementOf(c.Source);
			const Entity detector = a.Detector(c.Source);
			if (element && detector != NULL_ENTITY)
				return {detector, c.Source, point, element->Position, element->Rotation, i - 1};
		}
		return {};
	}
	template <class Access> CanvasPick PointerCanvas(const Access &a, const Pointer &p) {
		if (!p.Inside) return {};
		for (size_t i = a.CommandCount(); i > 0; --i) {
			const auto &c = a.CommandAt(i - 1);
			if (!PointerCollectorAllowed(a, p, c.Collector) || !a.Allows(c.Source, a.Collector(c.Source)))
				continue;
			const auto *scroll = a.ScrollingOf(c.Source);
			const auto *state = a.ScrollStateOf(c.Source);
			const auto point = PointerLogical(a, c.Collector, p.Position);
			if (!scroll || !state || !scroll->Enabled || !c.Clip.Contains(point) ||
				!c.Bounds.Contains(PointerUnrotated(c.Rotation, c.Bounds, point)))
				continue;
			if (PointerScrolls(*scroll, *state, true) || PointerElastic(*scroll, *state, true) ||
				PointerScrolls(*scroll, *state, false) || PointerElastic(*scroll, *state, false))
				return {c.Source, point, i - 1};
		}
		return {};
	}

	template <class Access> WheelPick PointerWheel(const Access &a, const Pointer &pointer) {
		Entity graph;
		int32_t graphOrder = 0, graphDepth = 0;
		core::Vector2 graphOrigin, graphPoint;
		a.EachGraph([&](Entity node, const NodeCanvas &, const Resolved &resolved) {
			const Entity collector = a.Collector(node);
			if (!resolved.Rendered || !a.Allows(node, collector) ||
				!PointerCollectorAllowed(a, pointer, collector))
				return;
			const auto point = PointerLogical(a, collector, pointer.Position);
			const core::Rect bounds{
				resolved.AbsolutePosition, resolved.AbsolutePosition + resolved.AbsoluteSize
			};
			const auto unrotated = PointerUnrotated(resolved.AbsoluteRotation, bounds, point);
			if (!bounds.Contains(unrotated) || !resolved.Clip.Contains(point)) return;
			if (graph == NULL_ENTITY || resolved.Order > graphOrder ||
				(resolved.Order == graphOrder && resolved.Depth > graphDepth)) {
				graph = node;
				graphOrder = resolved.Order;
				graphDepth = resolved.Depth;
				graphOrigin = resolved.AbsolutePosition;
				graphPoint = unrotated;
			}
		});
		if (graph != NULL_ENTITY) return {graph, graphOrigin, graphPoint, true};
		Entity best;
		int32_t bestOrder = 0, bestDepth = 0;
		a.EachScrolling(
			[&](Entity node, const Scrolling &scrolling, const ScrollState &state, const Resolved &resolved) {
				const Entity collector = a.Collector(node);
				if (!resolved.Rendered || !a.Allows(node, collector) ||
					!PointerCollectorAllowed(a, pointer, collector) ||
					(!PointerScrolls(scrolling, state, true) && !PointerScrolls(scrolling, state, false)))
					return;
				const auto point = PointerLogical(a, collector, pointer.Position);
				const core::Rect bounds{
					resolved.AbsolutePosition, resolved.AbsolutePosition + resolved.AbsoluteSize
				};
				if (!bounds.Contains(PointerUnrotated(resolved.AbsoluteRotation, bounds, point)) ||
					!resolved.Clip.Contains(point))
					return;
				const bool inside = best != NULL_ENTITY && a.IsDescendant(node, best);
				const bool contains = best != NULL_ENTITY && a.IsDescendant(best, node);
				if (best != NULL_ENTITY && !inside &&
					(contains || !(resolved.Order > bestOrder ||
								   (resolved.Order == bestOrder && resolved.Depth > bestDepth))))
					return;
				best = node;
				bestOrder = resolved.Order;
				bestDepth = resolved.Depth;
			}
		);
		return {best, {}, {}, false};
	}
	template <class Access>
	std::optional<int32_t> PointerCaret(const Access &access, const Pointer &pointer, Entity wanted) {
		for (size_t index = access.CommandCount(); index > 0; --index) {
			const auto &command = access.CommandAt(index - 1);
			if (command.Kind != DrawKind::Text || command.Source != wanted || command.Shaping.Lines.empty())
				continue;
			const auto point = PointerLogical(access, command.Collector, pointer.Position);
			const float height = static_cast<float>(command.TextSize) * std::max(command.LineHeight, 0.0f);
			if (!(height > 0.0f)) break;
			float top = command.Bounds.Min.Y;
			if (command.YAlignment == TextYAlignment::Center)
				top += (command.Bounds.Height() - height * command.Shaping.Lines.size()) * .5f;
			else if (command.YAlignment == TextYAlignment::Bottom)
				top = command.Bounds.Max.Y - height * command.Shaping.Lines.size();
			const uint32_t line = static_cast<uint32_t>(std::clamp(
				static_cast<int32_t>((point.Y - top) / height),
				0,
				static_cast<int32_t>(command.Shaping.Lines.size() - 1)
			));
			const auto &placed = command.Shaping.Lines[line];
			float origin = command.Bounds.Min.X;
			if (command.XAlignment == TextXAlignment::Center)
				origin += (command.Bounds.Width() - placed.Advance) * .5f;
			else if (command.XAlignment == TextXAlignment::Right)
				origin = command.Bounds.Max.X - placed.Advance;
			const uint32_t source = SourceAt(command.Shaping, line, point.X - origin);
			const auto boundary = std::lower_bound(
				command.Shaping.GraphemeBoundaries.begin(), command.Shaping.GraphemeBoundaries.end(), source
			);
			return static_cast<int32_t>(std::distance(command.Shaping.GraphemeBoundaries.begin(), boundary)) +
				   1;
		}
		return {};
	}
}
