#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <cmath>
#include <studio/ImageGraph.hpp>
#include <studio/Vector2Editor.hpp>

namespace studio {
	using engine::imagegraph::Vector2;
	namespace {
		bool Finite(Vector2 v) {
			return std::isfinite(v.X) && std::isfinite(v.Y);
		}
		bool Valid(const Vector2PadView &v) {
			return std::isfinite(v.MinimumX) && std::isfinite(v.MaximumX) && std::isfinite(v.MinimumY) &&
				   std::isfinite(v.MaximumY) && v.MaximumX > v.MinimumX && v.MaximumY > v.MinimumY &&
				   std::isfinite(v.MaximumX - v.MinimumX) && std::isfinite(v.MaximumY - v.MinimumY);
		}
	}
	double Vector2EditorRound(double value) {
		if (!std::isfinite(value)) return value;
		const double floor = std::floor(value);
		const double remainder = value - floor;
		return remainder < .5			  ? floor
			   : remainder > .5			  ? floor + 1
			   : std::fmod(floor, 2) == 0 ? floor
										  : floor + 1;
	}
	bool Vector2PadCoordinate(
		const Vector2PadView &view,
		const Vector2EditorRect &rect,
		Vector2 mouse,
		bool control,
		Vector2 &coordinate
	) {
		if (!Valid(view) || !Finite(mouse) || !std::isfinite(rect.X) || !std::isfinite(rect.Y) ||
			!std::isfinite(rect.Width) || !std::isfinite(rect.Height) || rect.Width <= 0 || rect.Height <= 0)
			return false;
		Vector2 result{
			view.MinimumX + (mouse.X - rect.X) / rect.Width * (view.MaximumX - view.MinimumX),
			view.MaximumY - (mouse.Y - rect.Y) / rect.Height * (view.MaximumY - view.MinimumY)
		};
		if (control) result = {Vector2EditorRound(result.X), Vector2EditorRound(result.Y)};
		if (!Finite(result)) return false;
		coordinate = result;
		return true;
	}
	bool PanVector2Pad(Vector2PadView &view, Vector2 delta, const Vector2EditorRect &rect) {
		if (!Valid(view) || !Finite(delta) || !std::isfinite(rect.Width) || !std::isfinite(rect.Height) ||
			rect.Width <= 0 || rect.Height <= 0)
			return false;
		auto result = view;
		const double x = delta.X / rect.Width * (view.MaximumX - view.MinimumX);
		const double y = delta.Y / rect.Height * (view.MaximumY - view.MinimumY);
		result.MinimumX -= x;
		result.MaximumX -= x;
		result.MinimumY += y;
		result.MaximumY += y;
		if (!Valid(result)) return false;
		view = result;
		return true;
	}
	bool ZoomVector2Pad(Vector2PadView &view, double wheel) {
		if (!Valid(view) || !std::isfinite(wheel)) return false;
		const double cx = view.MinimumX * .5 + view.MaximumX * .5;
		const double cy = view.MinimumY * .5 + view.MaximumY * .5;
		const double rx = std::clamp((view.MaximumX - view.MinimumX) * .5 + wheel, 1., 100.);
		const double ry = std::clamp((view.MaximumY - view.MinimumY) * .5 + wheel, 1., 100.);
		const Vector2PadView result{cx - rx, cx + rx, cy - ry, cy + ry};
		if (!Valid(result)) return false;
		view = result;
		return true;
	}
	bool FocusVector2Pad(Vector2PadView &view, Vector2 value) {
		if (!Finite(value)) return false;
		const Vector2PadView result{value.X - 1, value.X + 1, value.Y - 1, value.Y + 1};
		if (!Valid(result)) return false;
		view = result;
		return true;
	}
	bool SnapVector2Preview(Vector2 &value, const Vector2PreviewSnap &snap, double scale, bool control) {
		if (!Finite(value) || !std::isfinite(scale) || scale <= 0 || !Finite(snap.GridSize) ||
			snap.GridSize.X < 0 || snap.GridSize.Y < 0 ||
			snap.Guides.size() > engine::imagegraph::Limits::MaximumArrayElements)
			return false;
		Vector2 result = value;
		const Vector2 grid = control		 ? (snap.ShowGrid ? snap.GridSize : Vector2{1, 1})
							 : snap.SnapGrid ? snap.GridSize
											 : Vector2{0, 0};
		if (grid.X != 0) result.X = Vector2EditorRound(result.X / grid.X) * grid.X;
		if (grid.Y != 0) result.Y = Vector2EditorRound(result.Y / grid.Y) * grid.Y;
		for (const auto &guide : snap.Guides) {
			if (!std::isfinite(guide.Position) ||
				(guide.Axis != engine::imagegraph::PreviewRulerAxis::Horizontal &&
				 guide.Axis != engine::imagegraph::PreviewRulerAxis::Vertical))
				return false;
			if (!snap.ShowRulers) continue;
			double &component =
				guide.Axis == engine::imagegraph::PreviewRulerAxis::Vertical ? result.X : result.Y;
			if (std::abs(component - guide.Position) < 8 / scale) component = guide.Position;
		}
		if (control) result = {Vector2EditorRound(result.X), Vector2EditorRound(result.Y)};
		if (!Finite(result)) return false;
		value = result;
		return true;
	}
	bool Vector2OverlayPosition(
		Vector2 value, Vector2 factor, Vector2 offset, Vector2 origin, double scale, Vector2 &position
	) {
		if (!Finite(value) || !Finite(factor) || !Finite(offset) || !Finite(origin) || factor.X <= 0 ||
			factor.Y <= 0 || !std::isfinite(scale) || scale <= 0)
			return false;
		Vector2 result{
			origin.X + (offset.X + value.X * factor.X) * scale,
			origin.Y + (offset.Y + value.Y * factor.Y) * scale
		};
		if (!Finite(result)) return false;
		position = result;
		return true;
	}
	bool Vector2OverlayDrag(
		Vector2 start,
		Vector2 delta,
		Vector2 factor,
		double scale,
		const Vector2PreviewSnap &snap,
		bool control,
		Vector2 &value
	) {
		if (!Finite(start) || !Finite(delta) || !Finite(factor) || factor.X <= 0 || factor.Y <= 0 ||
			!std::isfinite(scale) || scale <= 0)
			return false;
		Vector2 result{start.X + delta.X / factor.X / scale, start.Y + delta.Y / factor.Y / scale};
		if (!SnapVector2Preview(result, snap, scale, control)) return false;
		value = result;
		return true;
	}
	bool HitVector2Overlay(
		Vector2 mouse, Vector2 position, double style, Vector2 size, double gizmoScale, double scale
	) {
		if (!Finite(mouse) || !Finite(position) || !Finite(size) || !std::isfinite(gizmoScale) ||
			!std::isfinite(scale) || scale <= 0 || !std::isfinite(style) || style < 0 || style > 2)
			return false;
		const double dx = mouse.X - position.X, dy = mouse.Y - position.Y;
		if (style == 0) {
			const double radius = 8 * gizmoScale;
			return radius >= 0 && std::hypot(dx, dy) <= radius;
		}
		return size.X >= 0 && size.Y >= 0 && std::abs(dx) <= size.X * scale * .5 &&
			   std::abs(dy) <= size.Y * scale * .5;
	}
	bool SetImageGraphVector2Coordinates(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		Vector2 value,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		diagnostic = {};
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
			return item.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.vector2") {
			diagnostic = {
				Status::UnknownNode, std::string(nodeId), {}, "coordinate editor requires a Vector2 node"
			};
			return false;
		}
		if (!Finite(value) || node->Values.size() > Limits::MaximumPropertiesPerNode) {
			diagnostic = {
				Status::InvalidValue, std::string(nodeId), {}, "coordinate values must be finite and bounded"
			};
			return false;
		}
		Document candidate;
		candidate.FormatVersion = document.FormatVersion;
		candidate.Nodes.push_back(*node);
		if (!SetImageGraphValue(candidate, nodeId, "x", value.X, diagnostic) ||
			!SetImageGraphValue(candidate, nodeId, "y", value.Y, diagnostic))
			return false;
		node->Values = std::move(candidate.Nodes.front().Values);
		document.FormatVersion = candidate.FormatVersion;
		return true;
	}
	bool ReserveVector2Sprite(size_t held, size_t bytes, size_t copies, size_t budget) {
		if (held > budget) return false;
		return copies == 0 || bytes <= (budget - held) / copies;
	}
	bool SetImageGraphVector2CoordinatesAtFrame(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		Vector2 value,
		uint64_t tick,
		double subframe,
		engine::imagegraph::Diagnostic &diagnostic,
		bool negativeFrame
	) {
		using namespace engine::imagegraph;
		const FrameTime time{tick, subframe, negativeFrame};
		const auto animated = [&](std::string_view port) {
			return std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == nodeId && key.Port == port;
			});
		};
		const bool x = animated("x"), y = animated("y");
		if (!ValidFrameTime(time)) {
			diagnostic = {Status::InvalidValue, std::string(nodeId), {}, "coordinate key time is invalid"};
			return false;
		}
		if (!x && !y) return SetImageGraphVector2Coordinates(document, nodeId, value, diagnostic);
		if (document.Keyframes.size() > Limits::MaximumKeyframes || !Finite(value)) {
			diagnostic = {
				Status::InvalidValue, std::string(nodeId), {}, "coordinate keys must be finite and bounded"
			};
			return false;
		}
		Document candidate = document;
		if (!SetImageGraphVector2Coordinates(candidate, nodeId, value, diagnostic)) return false;
		for (const auto &[port, component] :
			 {std::pair<std::string_view, double>{"x", value.X}, {"y", value.Y}}) {
			if (!(port == "x" ? x : y)) continue;
			auto existing =
				std::find_if(candidate.Keyframes.begin(), candidate.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == nodeId && key.Port == port && GetFrameTime(key) == time;
				});
			if (existing != candidate.Keyframes.end())
				existing->Data = component;
			else {
				if (candidate.Keyframes.size() >= Limits::MaximumKeyframes) {
					diagnostic = {
						Status::LimitExceeded,
						std::string(nodeId),
						std::string(port),
						"coordinate key limit reached"
					};
					return false;
				}
				const Keyframe *policy = nullptr;
				for (const auto &key : candidate.Keyframes) {
					if (key.NodeId != nodeId || key.Port != port) continue;
					if (!policy || (CompareFrameTime(GetFrameTime(key), time) <= 0 &&
									(CompareFrameTime(GetFrameTime(*policy), time) > 0 ||
									 CompareFrameTime(GetFrameTime(key), GetFrameTime(*policy)) > 0)))
						policy = &key;
				}
				// New source keys start with linear handles and no driver, as valueKey does.
				Keyframe key{std::string(nodeId), std::string(port), tick, component, policy->Interpolation};
				(void)SetFrameTime(key, time);
				if (key.Interpolation == "source") key.Ease = KeyframeEase{};
				candidate.Keyframes.push_back(std::move(key));
			}
			const auto original =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == nodeId;
				});
			const auto base =
				std::find_if(original->Values.begin(), original->Values.end(), [&](const auto &v) {
					return v.Port == port;
				});
			auto target = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &node) {
				return node.Id == nodeId;
			});
			auto authored = std::find_if(target->Values.begin(), target->Values.end(), [&](const auto &v) {
				return v.Port == port;
			});
			if (base != original->Values.end())
				authored->Data = base->Data;
			else
				target->Values.erase(authored);
		}
		if (subframe != 0 || negativeFrame)
			candidate.FormatVersion = std::max(candidate.FormatVersion, uint32_t{9});
		Plan plan;
		if (Compile(candidate, plan, diagnostic) != Status::Ok) return false;
		auto target = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == nodeId;
		});
		auto edited = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &node) {
			return node.Id == nodeId;
		});
		target->Values = std::move(edited->Values);
		document.Keyframes = std::move(candidate.Keyframes);
		document.FormatVersion = candidate.FormatVersion;
		return true;
	}
}
