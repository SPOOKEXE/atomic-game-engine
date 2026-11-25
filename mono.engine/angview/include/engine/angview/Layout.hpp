#pragma once

// Node and port geometry in graph space, and hit testing against it.
//
// Kept apart from View so tests and hosts can ask "where is port x" without
// driving input, and so a painter never computes geometry of its own.
// @tier L4 · shared

#include <engine/ang/Compile.hpp>
#include <engine/angview/Theme.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace engine::angview {

	struct Rect {
		core::Vector2 Min{};
		core::Vector2 Max{};

		bool Contains(core::Vector2 point) const {
			return point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y;
		}
		bool Overlaps(const Rect &other) const {
			return Min.X <= other.Max.X && Max.X >= other.Min.X && Min.Y <= other.Max.Y &&
				   Max.Y >= other.Min.Y;
		}
		core::Vector2 Centre() const {
			return {(Min.X + Max.X) * 0.5f, (Min.Y + Max.Y) * 0.5f};
		}
		// Smallest rect holding both. An empty (zero) rect is not special.
		Rect Union(const Rect &other) const;
	};

	struct PortLayout {
		std::string Name{};
		ang::TypeId Type{};
		ang::Direction Dir = ang::Direction::Input;
		// Centre of the port marker, graph space.
		core::Vector2 Centre{};
	};

	struct NodeLayout {
		std::string Id{};
		std::string Title{};
		// Graph space, including the header.
		Rect Bounds{};
		std::vector<PortLayout> Inputs{};
		std::vector<PortLayout> Outputs{};
		bool Known = true;

		const PortLayout *Find(std::string_view port, ang::Direction dir) const;
	};

	// Geometry of a whole graph. Rebuilt when the graph revision or theme changes.
	class Layout {
	  public:
		// `offset` moves `moving` nodes, for a drag preview that is not yet an edit.
		void Build(
			const ang::Graph &graph,
			const ang::Registry &registry,
			const Theme &theme,
			const std::vector<std::string> &moving = {},
			core::Vector2 offset = {}
		);

		const std::vector<NodeLayout> &Nodes() const {
			return NodeList;
		}
		const NodeLayout *Find(std::string_view node) const;

		// Bounds of every node, or of the named nodes. Zero rect when none.
		Rect Bounds(const std::vector<std::string> *only = nullptr) const;

	  private:
		std::vector<NodeLayout> NodeList{};
		std::unordered_map<std::string, size_t> Index{};
	};

	// Port list for a node, from its definition, a subgraph, or its links when unknown.
	std::vector<ang::PortDef> PortsOf(
		const ang::Graph &graph,
		const ang::Registry &registry,
		const ang::NodeInstance &node,
		ang::Direction dir
	);

	// The curve a wire follows, graph or view space alike. Returns the four bezier
	// points; Straight and Orthogonal styles use only the ends.
	struct WireCurve {
		core::Vector2 From{};
		core::Vector2 ControlFrom{};
		core::Vector2 ControlTo{};
		core::Vector2 To{};
	};
	WireCurve CurveBetween(core::Vector2 from, core::Vector2 to, float scale);

	// Shortest distance from `point` to the wire, sampled along the curve.
	float DistanceToWire(const WireCurve &curve, WireStyle style, core::Vector2 point);
}
