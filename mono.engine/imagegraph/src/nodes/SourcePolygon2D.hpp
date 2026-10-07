#pragma once
// Pinned polygon/delaunay source algorithms, including its integer-coordinate hash collisions.
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <unordered_map>
namespace engine::imagegraph::detail::polygon2d {
	using std::pair;
	using std::swap;
	using std::unordered_map;
	using std::vector;
	struct Point {
		double x;
		double y;
	};

	struct Triangle {
		Point p1;
		Point p2;
		Point p3;
	};

	////- Utils

	inline bool points_equal(const Point &p1, const Point &p2) {
		return (p1.x == p2.x && p1.y == p2.y);
	}

	inline bool triangle_is_ccw(const Triangle &triangle) {
		double area = (triangle.p2.x - triangle.p1.x) * (triangle.p3.y - triangle.p1.y) -
					  (triangle.p3.x - triangle.p1.x) * (triangle.p2.y - triangle.p1.y);
		return area > 0;
	}

	inline bool triangle_equal(const Triangle &t1, const Triangle &t2) {
		return (points_equal(t1.p1, t2.p1) && points_equal(t1.p2, t2.p2) && points_equal(t1.p3, t2.p3)) ||
			   (points_equal(t1.p1, t2.p2) && points_equal(t1.p2, t2.p3) && points_equal(t1.p3, t2.p1)) ||
			   (points_equal(t1.p1, t2.p3) && points_equal(t1.p2, t2.p1) && points_equal(t1.p3, t2.p2));
	}

	inline bool share_vertexs(const Triangle &t1, const Triangle &t2) {
		return (
			points_equal(t1.p1, t2.p1) || points_equal(t1.p1, t2.p2) || points_equal(t1.p1, t2.p3) ||
			points_equal(t1.p2, t2.p1) || points_equal(t1.p2, t2.p2) || points_equal(t1.p2, t2.p3) ||
			points_equal(t1.p3, t2.p1) || points_equal(t1.p3, t2.p2) || points_equal(t1.p3, t2.p3)
		);
	}

	inline bool share_edge(const Triangle &triangle, const Point &p1, const Point &p2) {
		int count = 0;
		if (points_equal(triangle.p1, p1) || points_equal(triangle.p1, p2)) count++;
		if (points_equal(triangle.p2, p1) || points_equal(triangle.p2, p2)) count++;
		if (points_equal(triangle.p3, p1) || points_equal(triangle.p3, p2)) count++;
		return count == 2;
	}

	inline bool point_in_circumcircle(const Triangle &triangle, const Point &point) {
		Point a = triangle.p1;
		Point b = triangle.p2;
		Point c = triangle.p3;

		if (!triangle_is_ccw(triangle)) {
			b = triangle.p3;
			c = triangle.p2;
		}

		double ax = a.x - point.x;
		double ay = a.y - point.y;
		double bx = b.x - point.x;
		double by = b.y - point.y;
		double cx = c.x - point.x;
		double cy = c.y - point.y;

		double det = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
					 (cx * cx + cy * cy) * (ax * by - bx * ay);

		return det > 0;
	}

	inline int point_hash(const Point &point) {
		return static_cast<int32_t>((uint32_t(int32_t(point.x)) << 16) + uint32_t(int32_t(point.y)));
	}

	inline int point_pair_hash(const Point &p1, const Point &p2) {
		Point _p1 = p1;
		Point _p2 = p2;

		if (_p1.x > _p2.x)
			swap(_p1, _p2);
		else if (_p1.x == _p2.x && _p1.y > _p2.y)
			swap(_p1, _p2);

		return static_cast<int32_t>(
			(uint32_t(int32_t(_p1.x)) << 16) + uint32_t(int32_t(_p1.y)) + (uint32_t(int32_t(_p2.x)) << 8) +
			uint32_t(int32_t(_p2.y))
		);
	}

	////- Operations

	inline vector<Point> find_polygon_edges(const vector<Triangle> &triangles) {
		vector<Point> edges;
		unordered_map<int, int> edge_count;
		unordered_map<int, pair<Point, Point>> edge_map;

		for (const auto &triangle : triangles) {
			edge_map[point_pair_hash(triangle.p1, triangle.p2)] = {triangle.p1, triangle.p2};
			edge_map[point_pair_hash(triangle.p2, triangle.p3)] = {triangle.p2, triangle.p3};
			edge_map[point_pair_hash(triangle.p3, triangle.p1)] = {triangle.p3, triangle.p1};

			edge_count[point_pair_hash(triangle.p1, triangle.p2)]++;
			edge_count[point_pair_hash(triangle.p2, triangle.p3)]++;
			edge_count[point_pair_hash(triangle.p3, triangle.p1)]++;
		}

		for (const auto &edge : edge_count) {
			if (edge.second == 1) {
				auto it = edge_map.find(edge.first);
				if (it != edge_map.end()) {
					edges.emplace_back(it->second.first);
					edges.emplace_back(it->second.second);
				}
			}
		}

		return edges;
	}

	inline Triangle super_triangle(const vector<Point> &points) {
		double min_x = points[0].x;
		double min_y = points[0].y;
		double max_x = points[0].x;
		double max_y = points[0].y;

		for (const auto &point : points) {
			if (point.x < min_x) min_x = point.x;
			if (point.y < min_y) min_y = point.y;
			if (point.x > max_x) max_x = point.x;
			if (point.y > max_y) max_y = point.y;
		}

		double dx = max_x - min_x;
		double dy = max_y - min_y;
		double delta_max = dx > dy ? dx : dy;

		double center_x = (min_x + max_x) / 2;
		double center_y = (min_y + max_y) / 2;

		Point p1 = {center_x - 2 * delta_max, center_y - delta_max};
		Point p2 = {center_x, center_y + 2 * delta_max};
		Point p3 = {center_x + 2 * delta_max, center_y - delta_max};

		return {p1, p2, p3};
	}

	////- Clean

	inline void vector_remove_triangle(vector<Triangle> &triangles, const Triangle &triangle) {
		triangles.erase(
			remove_if(
				triangles.begin(),
				triangles.end(),
				[&](const Triangle &t) { return triangle_equal(t, triangle); }
			),
			triangles.end()
		);
	}

	////- Main

	inline double Cross(Vector2 a, Vector2 b, Vector2 c) {
		return (b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y);
	}
	inline bool InTriangle(Vector2 p, Vector2 a, Vector2 b, Vector2 c) {
		const double x = Cross(a, b, p), y = Cross(b, c, p), z = Cross(c, a, p);
		return !((x < 0 || y < 0 || z < 0) && (x > 0 || y > 0 || z > 0));
	}
	inline double Direction(Vector2 a, Vector2 b) {
		const double angle = -std::atan2(b.Y - a.Y, b.X - a.X) * 180 / std::numbers::pi;
		return angle < 0 ? angle + 360 : angle;
	}
	inline void Simplify(vector<Vector2> &points) {
		for (size_t i = points.size(); i > 1; --i)
			if (points[i - 1] == points[i - 2]) points.erase(points.begin() + i - 1);
		if (points.size() > 1 && points.front() == points.back()) points.pop_back();
		vector<size_t> removed;
		for (size_t i = 0; i < points.size(); ++i) {
			const auto a = points[(i + points.size() - 1) % points.size()], b = points[i],
					   c = points[(i + 1) % points.size()];
			if (a == b || std::abs(Direction(a, b) - Direction(b, c)) <= 4) removed.push_back(i);
		}
		for (auto it = removed.rbegin(); it != removed.rend(); ++it)
			points.erase(points.begin() + *it);
	}
	inline vector<std::array<uint32_t, 3>> Ear(vector<Vector2> &points) {
		vector<std::array<uint32_t, 3>> triangles;
		if (points.size() < 3) return triangles;
		Simplify(points);
		if (points.size() < 3) return triangles;
		const size_t n = points.size();
		size_t maximum = 0;
		double maxX = -99999;
		for (size_t i = 0; i < n; ++i)
			if (points[i].X > maxX) {
				maxX = points[i].X;
				maximum = i;
			}
		vector<uint32_t> convex, reflex, indices(n);
		std::iota(indices.begin(), indices.end(), 0);
		int side = 0;
		const auto sign = [](double x) { return (x > 0) - (x < 0); };
		for (size_t i = 0; i < n; ++i) {
			const size_t index = (maximum + n - 1 + i) % n;
			const int turn = sign(Cross(points[index], points[(index + 1) % n], points[(index + 2) % n]));
			if (side != 0 && side != turn)
				reflex.push_back((index + 1) % n);
			else {
				convex.push_back((index + 1) % n);
				side = turn;
			}
		}
		if (reflex.empty()) {
			for (uint32_t i = 0; i < n - 2; ++i)
				triangles.push_back({0, i + 1, i + 2});
			return triangles;
		}
		size_t repeated = 0;
		while (indices.size() > 3) {
			if (convex.empty()) return triangles;
			const uint32_t a = convex.front();
			const auto ai = std::find(indices.begin(), indices.end(), a) - indices.begin();
			const uint32_t b = indices[(ai + indices.size() - 1) % indices.size()],
						   c = indices[(ai + 1) % indices.size()];
			bool ear = true;
			for (auto index : indices)
				if (index != a && index != b && index != c &&
					InTriangle(points[index], points[a], points[b], points[c])) {
					ear = false;
					break;
				}
			std::erase(convex, a);
			if (!ear) {
				convex.push_back(a);
				if (repeated++ > indices.size()) break;
				continue;
			}
			std::erase(indices, a);
			triangles.push_back({a, b, c});
			for (auto neighbor : {b, c})
				if (std::find(reflex.begin(), reflex.end(), neighbor) != reflex.end()) {
					const size_t ni = std::find(indices.begin(), indices.end(), neighbor) - indices.begin();
					const auto p = points[indices[(ni + indices.size() - 1) % indices.size()]],
							   q = points[neighbor], r = points[indices[(ni + 1) % indices.size()]];
					if (sign(Cross(p, q, r)) == side) {
						std::erase(reflex, neighbor);
						convex.push_back(neighbor);
					}
				}
			repeated = 0;
		}
		if (indices.size() == 3) triangles.push_back({indices[0], indices[1], indices[2]});
		return triangles;
	}
	inline bool Ccw(Vector2 a, Vector2 b, Vector2 c) {
		return Cross(a, b, c) > 0;
	}
	inline bool PolygonContains(std::span<const Vector2> polygon, Vector2 center) {
		const Vector2 ray{center.X + 10000, center.Y};
		size_t intersections = 0;
		for (size_t i = 0; i < polygon.size(); ++i) {
			const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
			intersections +=
				(Ccw(center, a, b) != Ccw(ray, a, b)) && (Ccw(center, ray, a) != Ccw(center, ray, b));
		}
		return intersections % 2;
	}
	inline bool Delaunay(
		std::span<const Vector2> points,
		vector<std::array<uint32_t, 3>> &output,
		std::span<const Vector2> polygon = {},
		bool filterToPolygon = true,
		size_t maximumTriangles = Limits::MaximumLinks
	) {
		if (points.size() < 3) return true;
		vector<Point> sourcePoints;
		unordered_map<int, int> pointMap;
		for (size_t i = 0; i < points.size(); ++i) {
			const auto p = points[i];
			if (!std::isfinite(p.X) || !std::isfinite(p.Y) || p.X < INT32_MIN || p.X > INT32_MAX ||
				p.Y < INT32_MIN || p.Y > INT32_MAX)
				return false;
			sourcePoints.push_back({p.X, p.Y});
			pointMap[point_hash(sourcePoints.back())] = i;
		}
		const auto super = super_triangle(sourcePoints);
		for (auto p : {super.p1, super.p2, super.p3})
			if (p.x < INT32_MIN || p.x > INT32_MAX || p.y < INT32_MIN || p.y > INT32_MAX) return false;
		vector<Triangle> triangles{super};
		for (const auto &point : sourcePoints) {
			vector<Triangle> bad;
			for (const auto &triangle : triangles)
				if (point_in_circumcircle(triangle, point)) bad.push_back(triangle);
			const auto edges = find_polygon_edges(bad);
			for (const auto &triangle : bad)
				vector_remove_triangle(triangles, triangle);
			if (edges.size() / 2 > maximumTriangles || triangles.size() > maximumTriangles - edges.size() / 2)
				return false;
			for (size_t i = 0; i < edges.size(); i += 2)
				triangles.push_back({edges[i], edges[i + 1], point});
			if (triangles.size() > maximumTriangles) return false;
		}
		for (const auto &triangle : triangles) {
			if (share_vertexs(triangle, super)) continue;
			std::array<uint32_t, 3> indices{
				uint32_t(pointMap[point_hash(triangle.p1)]),
				uint32_t(pointMap[point_hash(triangle.p2)]),
				uint32_t(pointMap[point_hash(triangle.p3)])
			};
			const Vector2 center{
				(points[indices[0]].X + points[indices[1]].X + points[indices[2]].X) / 3,
				(points[indices[0]].Y + points[indices[1]].Y + points[indices[2]].Y) / 3
			};
			if (!filterToPolygon || PolygonContains(polygon.empty() ? points : polygon, center))
				output.push_back(indices);
		}
		return true;
	}
}
