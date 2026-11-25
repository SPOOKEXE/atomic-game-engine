#include <engine/angview/Layout.hpp>

#include <algorithm>
#include <cmath>

namespace engine::angview {

	Rect Rect::Union(const Rect &other) const {
		return {
			{std::min(Min.X, other.Min.X), std::min(Min.Y, other.Min.Y)},
			{std::max(Max.X, other.Max.X), std::max(Max.Y, other.Max.Y)},
		};
	}

	const PortLayout *NodeLayout::Find(std::string_view port, ang::Direction dir) const {
		const auto &ports = dir == ang::Direction::Input ? Inputs : Outputs;
		for (const auto &entry : ports) {
			if (entry.Name == port) {
				return &entry;
			}
		}
		return nullptr;
	}

	std::vector<ang::PortDef> PortsOf(
		const ang::Graph &graph,
		const ang::Registry &registry,
		const ang::NodeInstance &node,
		ang::Direction dir
	) {
		const bool input = dir == ang::Direction::Input;
		if (const auto def = registry.FindNode(node.Def)) {
			std::vector<ang::PortDef> ports = input ? def->Inputs : def->Outputs;
			for (auto &port : ports) {
				if (!port.Generic.empty()) {
					port.Type = ang::PortType(graph, registry, node.Id, port.Name, dir);
				}
			}
			return ports;
		}
		if (const ang::SubgraphDef *subgraph = registry.FindSubgraph(node.Def)) {
			std::vector<ang::PortDef> ports;
			for (const auto &exposed : input ? subgraph->Inputs : subgraph->Outputs) {
				ports.push_back(exposed.Port);
			}
			return ports;
		}
		// Unknown definition: show the ports its links name, so nothing is lost.
		std::vector<ang::PortDef> ports;
		for (const ang::Link &wire : graph.Links()) {
			const bool mine = input ? wire.ToNode == node.Id : wire.FromNode == node.Id;
			const std::string &name = input ? wire.ToPort : wire.FromPort;
			const bool seen = std::any_of(ports.begin(), ports.end(), [&](const ang::PortDef &port) {
				return port.Name == name;
			});
			if (mine && !seen) {
				ports.push_back(
					{.Name = name, .Type = ang::TypeId(ang::ANY_TYPE), .Dir = dir, .MaximumLinks = 0}
				);
			}
		}
		return ports;
	}

	void Layout::Build(
		const ang::Graph &graph,
		const ang::Registry &registry,
		const Theme &theme,
		const std::vector<std::string> &moving,
		core::Vector2 offset
	) {
		NodeList.clear();
		Index.clear();
		for (const ang::NodeInstance &node : graph.Nodes()) {
			NodeLayout layout;
			layout.Id = node.Id;
			const auto def = registry.FindNode(node.Def);
			const ang::SubgraphDef *subgraph = registry.FindSubgraph(node.Def);
			layout.Known = def != nullptr || subgraph != nullptr;
			layout.Title = def != nullptr									 ? def->Label
						   : subgraph != nullptr && !subgraph->Label.empty() ? subgraph->Label
																			 : node.Def;
			core::Vector2 origin = node.Position;
			if (std::find(moving.begin(), moving.end(), node.Id) != moving.end()) {
				origin = origin + offset;
			}
			const auto inputs = PortsOf(graph, registry, node, ang::Direction::Input);
			const auto outputs = PortsOf(graph, registry, node, ang::Direction::Output);
			const size_t rows = std::max<size_t>(1, std::max(inputs.size(), outputs.size()));
			const float height =
				theme.HeaderHeight + theme.NodePadding * 2.0f + theme.RowHeight * static_cast<float>(rows);
			layout.Bounds = {origin, {origin.X + theme.NodeWidth, origin.Y + height}};
			const float firstRow = origin.Y + theme.HeaderHeight + theme.NodePadding + theme.RowHeight * 0.5f;
			for (size_t index = 0; index < inputs.size(); ++index) {
				layout.Inputs.push_back(
					{inputs[index].Name,
					 inputs[index].Type,
					 ang::Direction::Input,
					 {origin.X, firstRow + theme.RowHeight * static_cast<float>(index)}}
				);
			}
			for (size_t index = 0; index < outputs.size(); ++index) {
				layout.Outputs.push_back(
					{outputs[index].Name,
					 outputs[index].Type,
					 ang::Direction::Output,
					 {origin.X + theme.NodeWidth, firstRow + theme.RowHeight * static_cast<float>(index)}}
				);
			}
			Index.emplace(layout.Id, NodeList.size());
			NodeList.push_back(std::move(layout));
		}
	}

	const NodeLayout *Layout::Find(std::string_view node) const {
		const auto found = Index.find(std::string(node));
		return found == Index.end() ? nullptr : &NodeList[found->second];
	}

	Rect Layout::Bounds(const std::vector<std::string> *only) const {
		bool first = true;
		Rect bounds{};
		for (const NodeLayout &node : NodeList) {
			if (only != nullptr && std::find(only->begin(), only->end(), node.Id) == only->end()) {
				continue;
			}
			bounds = first ? node.Bounds : bounds.Union(node.Bounds);
			first = false;
		}
		return bounds;
	}

	WireCurve CurveBetween(core::Vector2 from, core::Vector2 to, float scale) {
		// Handles reach out horizontally so a wire leaves an output rightwards and
		// enters an input from the left, even when the input is behind the output.
		const float reach = std::max(50.0f * scale, std::abs(to.X - from.X) * 0.5f);
		return {from, {from.X + reach, from.Y}, {to.X - reach, to.Y}, to};
	}

	namespace {
		core::Vector2 BezierAt(const WireCurve &curve, float t) {
			const float u = 1.0f - t;
			const float a = u * u * u;
			const float b = 3.0f * u * u * t;
			const float c = 3.0f * u * t * t;
			const float d = t * t * t;
			return {
				a * curve.From.X + b * curve.ControlFrom.X + c * curve.ControlTo.X + d * curve.To.X,
				a * curve.From.Y + b * curve.ControlFrom.Y + c * curve.ControlTo.Y + d * curve.To.Y,
			};
		}

		float SegmentDistance(core::Vector2 a, core::Vector2 b, core::Vector2 point) {
			const core::Vector2 edge = b - a;
			const float lengthSquared = edge.Dot(edge);
			float t = lengthSquared > 0.0f ? (point - a).Dot(edge) / lengthSquared : 0.0f;
			t = std::clamp(t, 0.0f, 1.0f);
			const core::Vector2 nearest = a + edge * t;
			const core::Vector2 gap = point - nearest;
			return std::sqrt(gap.Dot(gap));
		}
	}

	float DistanceToWire(const WireCurve &curve, WireStyle style, core::Vector2 point) {
		if (style == WireStyle::Straight) {
			return SegmentDistance(curve.From, curve.To, point);
		}
		if (style == WireStyle::Orthogonal) {
			const float middle = (curve.From.X + curve.To.X) * 0.5f;
			const core::Vector2 bendA{middle, curve.From.Y};
			const core::Vector2 bendB{middle, curve.To.Y};
			return std::min(
				{SegmentDistance(curve.From, bendA, point),
				 SegmentDistance(bendA, bendB, point),
				 SegmentDistance(bendB, curve.To, point)}
			);
		}
		constexpr int SEGMENTS = 24;
		float best = std::numeric_limits<float>::max();
		core::Vector2 previous = curve.From;
		for (int step = 1; step <= SEGMENTS; ++step) {
			const core::Vector2 next = BezierAt(curve, static_cast<float>(step) / SEGMENTS);
			best = std::min(best, SegmentDistance(previous, next, point));
			previous = next;
		}
		return best;
	}
}
