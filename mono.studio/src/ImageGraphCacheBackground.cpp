#include "ImageGraphCacheBackground.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>
#include <unordered_set>

namespace studio::detail {
	namespace {
		constexpr uint64_t MaximumBytes = 64u * 1024u * 1024u;
		constexpr size_t MaximumBoxes = engine::imagegraph::Limits::MaximumNodes * 8;
		uint64_t RetainedBytes(const ImageGraphCacheBackgrounds &backgrounds) {
			uint64_t bytes = backgrounds.Shapes.capacity() * sizeof(ImageGraphCacheBackground);
			for (const auto &shape : backgrounds.Shapes)
				bytes += shape.Boxes.capacity() * sizeof(ImageGraphCacheBox) +
						 shape.Hull.capacity() * sizeof(ImVec2);
			return bytes;
		}
		double Cross(ImVec2 a, ImVec2 b, ImVec2 c) {
			return (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
		}
	}
	std::vector<ImVec2> BuildImageGraphCacheHull(std::span<const ImageGraphCacheBox> boxes) {
		ENGINE_PROFILE("studio.imagegraph.cache_group_hull");
		if (boxes.size() < 2 || boxes.size() > MaximumBoxes) return {};
		std::vector<ImVec2> points;
		points.reserve(boxes.size() * 28);
		for (size_t index = 0; index < boxes.size(); ++index) {
			const auto &box = boxes[index];
			if (!std::isfinite(box.X) || !std::isfinite(box.Y) || !std::isfinite(box.Width) ||
				!std::isfinite(box.Height) || box.Width < 0 || box.Height < 0)
				return {};
			const float left = box.X - 28, top = box.Y - 28;
			const float right = box.X + (box.Id == boxes.front().Id ? box.Width / 2 : box.Width + 28);
			const float bottom = box.Y + box.Height + 28;
			if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
				!std::isfinite(bottom))
				return {};
			for (int corner = 0; corner < 4; ++corner)
				for (int step = 0; step <= 6; ++step) {
					const double angle = (corner * 90 + step * 15) * std::numbers::pi / 180;
					points.push_back(
						{float((corner == 0 || corner == 3 ? right : left) + 4 * std::cos(angle)),
						 float((corner < 2 ? top : bottom) - 4 * std::sin(angle))}
					);
				}
		}
		std::sort(points.begin(), points.end(), [](ImVec2 a, ImVec2 b) {
			return a.x < b.x || (a.x == b.x && a.y < b.y);
		});
		points.erase(
			std::unique(
				points.begin(), points.end(), [](ImVec2 a, ImVec2 b) { return a.x == b.x && a.y == b.y; }
			),
			points.end()
		);
		if (points.size() < 3) return {};
		std::vector<ImVec2> hull;
		hull.reserve(points.size() * 2);
		for (const auto point : points) {
			while (hull.size() > 1 && Cross(hull[hull.size() - 2], hull.back(), point) <= 0)
				hull.pop_back();
			hull.push_back(point);
		}
		const size_t lower = hull.size();
		for (size_t i = points.size() - 1; i-- > 0;) {
			const auto point = points[i];
			while (hull.size() > lower && Cross(hull[hull.size() - 2], hull.back(), point) <= 0)
				hull.pop_back();
			hull.push_back(point);
		}
		hull.pop_back();
		return hull;
	}
	bool DrawImageGraphCacheBackgrounds(
		const nodegraph::Graph &graph,
		const nodegraph::Canvas &canvas,
		const nodegraph::ViewFrame &view,
		const engine::imagegraph::CacheGroupReplayState &groups,
		const std::unordered_map<std::string, nodegraph::NodeId> &ids,
		ImageGraphCacheBackgrounds &backgrounds,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("studio.imagegraph.cache_group_background");
		const auto fail = [&](const char *message) {
			backgrounds.Clear();
			diagnostic = {engine::imagegraph::Status::LimitExceeded, {}, {}, message};
			return false;
		};
		if (!maximumBytes || maximumBytes > MaximumBytes ||
			groups.Owners.size() > engine::imagegraph::Limits::MaximumNodes ||
			graph.Nodes().size() > engine::imagegraph::Limits::MaximumNodes ||
			backgrounds.Shapes.size() > engine::imagegraph::Limits::MaximumNodes)
			return fail("Cache group background cap is outside bounds");
		size_t count = 0;
		uint64_t nameBytes = 0;
		for (const auto &owner : groups.Owners) {
			if (owner.Members.empty()) continue;
			if (owner.Members.size() >= MaximumBoxes - count)
				return fail("Cache group background membership exceeds bounds");
			count += owner.Members.size() + 1;
			if (owner.NodeId.size() > MaximumBytes - nameBytes)
				return fail("Cache group background name work exceeds bounds");
			nameBytes += owner.NodeId.size();
			for (const auto &member : owner.Members) {
				if (member.size() > MaximumBytes - nameBytes)
					return fail("Cache group background name work exceeds bounds");
				nameBytes += member.size();
			}
		}
		if (uint64_t(count) * 28 * 64 > MaximumBytes - nameBytes)
			return fail("Cache group background geometry work exceeds bounds");
		// grug original cache, replacement boxes, points, two hull chains and screen copy coexist.
		const uint64_t workspace =
			uint64_t(count) * (sizeof(ImageGraphCacheBox) * 2 + 28 * sizeof(ImVec2) * 5) +
			groups.Owners.size() * (2 * sizeof(ImageGraphCacheBackground) + 64) +
			backgrounds.Shapes.size() * 64 + graph.Nodes().size() * 160;
		const auto resident = RetainedBytes(backgrounds);
		if (resident > maximumBytes || workspace > maximumBytes - resident)
			return fail("Cache group background live bytes exceed bounds");
		backgrounds.Shapes.reserve(groups.Owners.size());
		std::unordered_map<nodegraph::NodeId, size_t> prior;
		prior.reserve(backgrounds.Shapes.size());
		for (size_t i = 0; i < backgrounds.Shapes.size(); ++i)
			prior.emplace(backgrounds.Shapes[i].Owner, i);
		std::unordered_set<nodegraph::NodeId> retained;
		retained.reserve(groups.Owners.size());
		std::unordered_map<nodegraph::NodeId, const nodegraph::Node *> nodes;
		nodes.reserve(graph.Nodes().size());
		for (const auto &node : graph.Nodes())
			nodes.emplace(node.Id, &node);
		std::unordered_map<nodegraph::NodeId, ImageGraphCacheBox> measured;
		measured.reserve(graph.Nodes().size());
		const auto nodeFor = [&](const std::string &id) -> const nodegraph::Node * {
			const auto found = ids.find(id);
			if (found == ids.end()) return nullptr;
			const auto node = nodes.find(found->second);
			return node == nodes.end() ? nullptr : node->second;
		};
		for (const auto &owner : groups.Owners) {
			if (owner.Members.empty()) continue;
			const auto *anchor = nodeFor(owner.NodeId);
			if (!anchor) continue;
			retained.insert(anchor->Id);
			std::vector<ImageGraphCacheBox> boxes;
			boxes.reserve(owner.Members.size() + 1);
			const auto add = [&](const nodegraph::Node &node) {
				const auto [found, inserted] = measured.try_emplace(node.Id);
				if (inserted) {
					const auto layout = canvas.MeasureNode(node);
					found->second = {node.Id, node.X, node.Y, layout.Width, layout.Height};
				}
				boxes.push_back(found->second);
			};
			add(*anchor);
			for (const auto &member : owner.Members)
				if (const auto *node = nodeFor(member)) add(*node);
			const auto found = prior.find(anchor->Id);
			const size_t index = found == prior.end() ? backgrounds.Shapes.size() : found->second;
			if (index == backgrounds.Shapes.size()) backgrounds.Shapes.push_back({anchor->Id, {}, {}});
			auto &shape = backgrounds.Shapes[index];
			if (shape.Boxes != boxes) {
				auto hull = BuildImageGraphCacheHull(boxes);
				if (boxes.size() > 1 && hull.size() < 3)
					return fail("Cache group background geometry is invalid");
				shape.Boxes = std::move(boxes);
				shape.Hull = std::move(hull);
			}
		}
		std::erase_if(backgrounds.Shapes, [&](const auto &shape) { return !retained.contains(shape.Owner); });
		if (RetainedBytes(backgrounds) > maximumBytes)
			return fail("Cache group background retention exceeds bounds");
		for (const auto &shape : backgrounds.Shapes) {
			const auto found = nodes.find(shape.Owner);
			const auto *anchor = found == nodes.end() ? nullptr : found->second;
			if (!anchor || anchor->Owner != canvas.Inside() || shape.Hull.size() < 3) continue;
			const auto &box = shape.Boxes.front();
			const float left = (box.X + view.PanX) * view.Scale, top = (box.Y + view.PanY) * view.Scale;
			if (left + box.Width * view.Scale < -32 || top + box.Height * view.Scale < -32 ||
				left > view.Width + 32 || top > view.Height + 64)
				continue;
			std::vector<ImVec2> screen;
			screen.reserve(shape.Hull.size());
			for (const auto point : shape.Hull)
				screen.push_back(
					{view.X + (point.x + view.PanX) * view.Scale, view.Y + (point.y + view.PanY) * view.Scale}
				);
			auto *draw = ImGui::GetWindowDrawList();
			draw->AddConvexPolyFilled(
				screen.data(), int(screen.size()), ImGui::ColorConvertFloat4ToU32({1, 1, 1, 0.025f})
			);
			draw->AddPolyline(
				screen.data(),
				int(screen.size()),
				ImGui::ColorConvertFloat4ToU32({1, 1, 1, 0.3f}),
				1.0f,
				ImDrawFlags_Closed
			);
		}
		return true;
	} catch (const std::bad_alloc &) {
		backgrounds.Clear();
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded, {}, {}, "Cache group background allocation refused"
		};
		return false;
	}
}
