#pragma once

#include <engine/collision/ConvexHull.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace hull_fixture {
	using engine::collision::ConvexHull;
	using engine::core::Vector3;
	inline void Require(bool condition, const char *reason) {
		if (!condition) throw std::runtime_error(reason);
	}
	inline bool Finite(const Vector3 &p) {
		return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z);
	}
	struct Plane {
		Vector3 Normal;
		float Offset = 0;
	};
	struct Input {
		std::vector<Vector3> Points;
		std::vector<Vector3> Corners;
		std::vector<Plane> Planes;
		float Tolerance = .0002f;
		bool Flat = false;
		bool Capped = false;
	};
	inline Input Make(size_t row) {
		Input input;
		if (row < 2) {
			for (float x : {-2.f, 2.f})
				for (float y : {-2.f, 2.f})
					for (float z : {-2.f, 2.f})
						input.Corners.push_back({x, y, z});
			input.Planes = {
				{{1, 0, 0}, 2},
				{{-1, 0, 0}, 2},
				{{0, 1, 0}, 2},
				{{0, -1, 0}, 2},
				{{0, 0, 1}, 2},
				{{0, 0, -1}, 2}
			};
		} else if (row < 4) {
			constexpr std::array<std::array<float, 2>, 8> ring{
				{{2, 1}, {1, 2}, {-1, 2}, {-2, 1}, {-2, -1}, {-1, -2}, {1, -2}, {2, -1}}
			};
			for (float z : {-1.f, 1.f})
				for (const auto &p : ring)
					input.Corners.push_back({p[0], p[1], z});
			const float inverse = 1 / std::sqrt(2.f);
			input.Planes = {
				{{1, 0, 0}, 2},
				{{-1, 0, 0}, 2},
				{{0, 1, 0}, 2},
				{{0, -1, 0}, 2},
				{{0, 0, 1}, 1},
				{{0, 0, -1}, 1},
				{{inverse, inverse, 0}, 3 * inverse},
				{{-inverse, inverse, 0}, 3 * inverse},
				{{-inverse, -inverse, 0}, 3 * inverse},
				{{inverse, -inverse, 0}, 3 * inverse}
			};
		}
		if (row < 4) {
			input.Points = input.Corners;
			const size_t count = row == 1 ? 30000 : 1024;
			for (size_t index = 0; index < count; ++index) {
				const Vector3 point{
					static_cast<float>(index % 31) / 32 - .5f,
					static_cast<float>((index / 31) % 31) / 32 - .5f,
					static_cast<float>((index / 961) % 31) / 32 - .5f
				};
				input.Points.push_back(point);
				if (row == 1 && index % 3 == 0) input.Points.push_back(point);
			}
			if (row == 3) {
				// Interior seam points straddle weld cells and must retain the first
				// representative. They cannot perturb the literal outer polyhedron.
				for (int index = 0; index < 64; ++index) {
					const float home = static_cast<float>(index) * engine::collision::HULL_WELD_DISTANCE;
					input.Points.push_back({home, 0, 0});
					input.Points.push_back({home - engine::collision::HULL_WELD_DISTANCE * .25f, 0, 0});
				}
			}
		} else if (row == 4) {
			input.Flat = true;
			for (int x = 0; x < 8; ++x)
				for (int z = 0; z < 8; ++z)
					input.Points.push_back({x * .125f, 0, z * .125f});
			input.Points.push_back(input.Points.front());
			input.Points.push_back({std::numeric_limits<float>::quiet_NaN(), 0, 0});
		} else {
			input.Capped = true;
			// All 128 paraboloid points are exposed by a distinct tangent support
			// direction, so this exercises the64-corner refusal rather than size alone.
			for (int x = 0; x < 16; ++x)
				for (int y = 0; y < 8; ++y) {
					const float a = (x - 8) * .125f, b = (y - 4) * .125f;
					input.Points.push_back({a, b, a * a + b * b});
				}
		}
		return input;
	}
	inline std::vector<Vector3> ReferenceWeld(std::span<const Vector3> points) {
		std::vector<Vector3> kept;
		constexpr float squared =
			engine::collision::HULL_WELD_DISTANCE * engine::collision::HULL_WELD_DISTANCE;
		for (const auto &point : points) {
			if (!Finite(point)) continue;
			bool duplicate = false;
			for (const auto &seen : kept) {
				const float x = seen.X - point.X, y = seen.Y - point.Y, z = seen.Z - point.Z;
				if (x * x + y * y + z * z <= squared) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) kept.push_back(point);
		}
		return kept;
	}
	inline std::vector<uint32_t> Canonical(const ConvexHull &hull) {
		std::vector<uint32_t> words;
		const auto scalar = [&](float value) { words.push_back(std::bit_cast<uint32_t>(value)); };
		const auto point = [&](const Vector3 &p) {
			scalar(p.X);
			scalar(p.Y);
			scalar(p.Z);
		};
		words.push_back(static_cast<uint32_t>(hull.Points.size()));
		for (const auto &p : hull.Points)
			point(p);
		words.push_back(static_cast<uint32_t>(hull.Faces.size()));
		for (const auto &face : hull.Faces) {
			words.push_back(face.FirstIndex);
			words.push_back(face.IndexCount);
			point(face.Normal);
			scalar(face.Offset);
		}
		words.push_back(static_cast<uint32_t>(hull.Loops.size()));
		words.insert(words.end(), hull.Loops.begin(), hull.Loops.end());
		point(hull.Bounds.Minimum);
		point(hull.Bounds.Maximum);
		words.push_back(hull.Solid());
		return words;
	}
	inline void Verify(const Input &input, const ConvexHull &hull) {
		Require(
			!hull.Points.empty() && hull.Points.size() <= engine::collision::MAXIMUM_HULL_POINTS,
			"hull point admission"
		);
		Vector3 minimum = hull.Points.front(), maximum = minimum;
		for (const auto &p : hull.Points) {
			Require(
				Finite(p) && std::find(input.Points.begin(), input.Points.end(), p) != input.Points.end(),
				"hull invented point"
			);
			minimum = {std::min(minimum.X, p.X), std::min(minimum.Y, p.Y), std::min(minimum.Z, p.Z)};
			maximum = {std::max(maximum.X, p.X), std::max(maximum.Y, p.Y), std::max(maximum.Z, p.Z)};
		}
		Require(hull.Bounds.Minimum == minimum && hull.Bounds.Maximum == maximum, "hull bounds oracle");
		if (input.Flat) {
			Require(
				!hull.Solid() && hull.Faces.empty() && hull.Loops.empty() &&
					hull.Points == ReferenceWeld(input.Points),
				"hull flat representative order"
			);
			return;
		}
		Require(hull.Solid(), "hull missing solid");
		if (input.Capped)
			Require(
				hull.Points.size() == engine::collision::MAXIMUM_HULL_POINTS,
				"hull exposed cloud failed cap control"
			);
		if (!input.Capped) {
			Require(
				hull.Points.size() == input.Corners.size() && hull.Faces.size() == input.Planes.size(),
				"hull analytical topology count"
			);
			for (const auto &corner : input.Corners)
				Require(
					std::find(hull.Points.begin(), hull.Points.end(), corner) != hull.Points.end(),
					"hull lost literal corner"
				);
		}
		std::vector<std::pair<uint32_t, uint32_t>> edges;
		std::vector<bool> planeSeen(input.Planes.size(), false);
		for (const auto &face : hull.Faces) {
			Require(
				face.IndexCount >= 3 &&
					static_cast<uint64_t>(face.FirstIndex) + face.IndexCount <= hull.Loops.size() &&
					Finite(face.Normal) && std::isfinite(face.Offset),
				"hull invalid face range"
			);
			if (!input.Capped) {
				size_t match = input.Planes.size();
				for (size_t index = 0; index < input.Planes.size(); ++index) {
					const auto &plane = input.Planes[index];
					if ((face.Normal - plane.Normal).MagnitudeSquared() < 1e-8f &&
						std::abs(face.Offset - plane.Offset) < .0001f)
						match = index;
				}
				Require(
					match < input.Planes.size() && !planeSeen[match],
					"hull incorrect or duplicate analytical plane"
				);
				planeSeen[match] = true;
			}
			for (uint32_t step = 0; step < face.IndexCount; ++step) {
				const uint32_t a = hull.Loops[face.FirstIndex + step],
							   b = hull.Loops[face.FirstIndex + (step + 1) % face.IndexCount];
				Require(
					a < hull.Points.size() && b < hull.Points.size() && a != b, "hull invalid loop vertex"
				);
				edges.emplace_back(a, b);
				Require(
					std::abs(face.Normal.Dot(hull.Points[a]) - face.Offset) < .001f, "hull loop outside plane"
				);
			}
			const auto &a = hull.Points[hull.Loops[face.FirstIndex]],
					   &b = hull.Points[hull.Loops[face.FirstIndex + 1]],
					   &c = hull.Points[hull.Loops[face.FirstIndex + 2]];
			Require((b - a).Cross(c - a).Dot(face.Normal) > 0, "hull reversed face winding");
			for (const auto &p : input.Capped ? hull.Points : input.Points)
				if (Finite(p))
					Require(face.Normal.Dot(p) - face.Offset <= .001f, "hull failed allowed enclosure");
		}
		for (const auto &edge : edges)
			Require(
				std::count(edges.begin(), edges.end(), std::pair{edge.second, edge.first}) == 1,
				"hull not closed"
			);
		if (!input.Capped)
			for (const Vector3 direction : std::array<Vector3, 6>{
					 {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}, {1, 2, -1}, {-2, 1, 3}, {-1, -1, -1}}
				 }) {
				float expected = -std::numeric_limits<float>::infinity();
				for (const auto &corner : input.Corners)
					expected = std::max(expected, corner.Dot(direction));
				Require(
					std::abs(engine::collision::SupportDistance(hull, direction) - expected) < .0001f,
					"hull analytical support"
				);
			}
	}
	inline uint64_t Hash(std::span<const uint32_t> words) {
		uint64_t hash = 14695981039346656037ULL;
		for (uint32_t word : words)
			for (size_t byte = 0; byte < 4; ++byte) {
				hash ^= (word >> (8 * byte)) & 255;
				hash *= 1099511628211ULL;
			}
		return hash;
	}
	inline uint64_t InputHash(const Input &input) {
		uint64_t hash = 14695981039346656037ULL;
		const auto add = [&](uint32_t word) {
			for (size_t byte = 0; byte < 4; ++byte) {
				hash ^= (word >> (8 * byte)) & 255;
				hash *= 1099511628211ULL;
			}
		};
		add(static_cast<uint32_t>(input.Points.size()));
		add(std::bit_cast<uint32_t>(input.Tolerance));
		add(std::bit_cast<uint32_t>(engine::collision::HULL_WELD_DISTANCE));
		add(input.Flat);
		add(input.Capped);
		add(engine::collision::MAXIMUM_HULL_POINTS);
		for (const auto &point : input.Points) {
			add(std::bit_cast<uint32_t>(point.X));
			add(std::bit_cast<uint32_t>(point.Y));
			add(std::bit_cast<uint32_t>(point.Z));
		}
		return hash;
	}
	inline void DumpPreflight(size_t row, size_t permutation, const Input &input, const ConvexHull &hull) {
		std::printf(
			"# hull-preflight-canonical row=%zu permutation=%zu input_hash=%016llx words=",
			row,
			permutation,
			static_cast<unsigned long long>(InputHash(input))
		);
		for (uint32_t word : Canonical(hull))
			std::printf("%08x", word);
		std::printf("\n");
	}
	inline void Preflight(bool dump = false) {
		Input seams;
		seams.Flat = true;
		seams.Tolerance = 1;
		const float weld = engine::collision::HULL_WELD_DISTANCE;
		seams.Points = {
			{weld * .1f, 0, 0},
			{-weld * .1f, 0, 0},
			{weld * 3, 0, 0},
			{weld * 3.25f, 0, 0},
			{weld * 6, 0, 0},
			{weld * 8, 0, 0},
			{std::numeric_limits<float>::infinity(), 0, 0}
		};
		// Hull plane tolerance is intentionally huge: weld representatives
		// must still use the fixed 0.0001 distance and remain input ordered.
		const auto seamHull = engine::collision::BuildConvexHull(seams.Points, seams.Tolerance);
		Verify(seams, seamHull);
		if (dump) DumpPreflight(6, 0, seams, seamHull);
		for (size_t row = 0; row < 6; ++row) {
			Input original = Make(row);
			for (size_t permutation = 0; permutation < 3; ++permutation) {
				Input input = original;
				// Preserve the literal outer-corner prefix for uncapped analytic
				// clouds: reversing interior-first input could hit the admission
				// cap before the known corners arrive, a different allowed result.
				const size_t split = input.Corners.size();
				if (permutation == 1) {
					std::reverse(input.Points.begin(), input.Points.begin() + split);
					std::reverse(input.Points.begin() + split, input.Points.end());
				}
				if (permutation == 2) {
					std::rotate(
						input.Points.begin(), input.Points.begin() + split / 3, input.Points.begin() + split
					);
					std::rotate(
						input.Points.begin() + split,
						input.Points.begin() + split + (input.Points.size() - split) / 3,
						input.Points.end()
					);
				}
				const uint64_t hash = InputHash(input);
				const auto first = engine::collision::BuildConvexHull(input.Points, input.Tolerance);
				Verify(input, first);
				if (dump) DumpPreflight(row, permutation, input, first);
				const auto second = engine::collision::BuildConvexHull(input.Points, input.Tolerance);
				Require(Canonical(first) == Canonical(second), "hull repeat full-word parity");
				Require(InputHash(input) == hash, "hull builder mutated source");
			}
		}
	}
}
