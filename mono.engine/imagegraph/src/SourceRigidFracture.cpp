#include "SourceRigidFracture.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint32_t MaximumFractureSide = 128;
		using Label = std::array<int32_t, 4>;
		double Direction(Vector2 a, Vector2 b) {
			double direction = std::atan2(a.Y - b.Y, b.X - a.X) * 180 / std::numbers::pi;
			if (direction < 0) direction += 360;
			return direction;
		}
		void RemoveCollinear(std::vector<Vector2> &points) {
			const auto count = points.size();
			if (!count) return;
			std::vector<size_t> removals;
			for (size_t i = 0; i < count; ++i) {
				const double first = Direction(points[i], points[(i + 1) % count]);
				const double second = Direction(points[(i + 1) % count], points[(i + 2) % count]);
				if (std::abs(first - second) <= 5) removals.push_back((i + 1) % count);
			}
			std::sort(removals.rbegin(), removals.rend());
			for (const auto index : removals)
				points.erase(points.begin() + index);
		}
		bool RemoveConcave(std::vector<Vector2> &points) {
			const auto count = points.size();
			if (count <= 3) return true;
			size_t start = 0;
			double maximumX = 0;
			for (size_t i = 0; i < count; ++i)
				if (points[i].X > maximumX) {
					maximumX = points[i].X;
					start = i;
				}
			std::vector<size_t> stack{start, (start + 1) % count}, removals;
			size_t test = (start + 2) % count;
			int acceptedSide = 1;
			for (size_t iteration = 0; iteration < count * 4; ++iteration) {
				if (stack.size() < 2) return false;
				const size_t potential = stack.back();
				stack.pop_back();
				const size_t anchor = stack.back();
				if (potential == start) {
					std::sort(removals.rbegin(), removals.rend());
					for (const auto index : removals) {
						if (index >= points.size()) return false;
						points.erase(points.begin() + index);
					}
					return true;
				}
				const auto a = points[anchor], b = points[potential], c = points[test];
				const double cross = (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
				const int side = (cross > 0) - (cross < 0);
				if (acceptedSide == 0 || acceptedSide == side) {
					acceptedSide = side;
					stack.push_back(potential);
					stack.push_back(test);
					test = (test + 1) % count;
				} else {
					if (stack.size() == 1) {
						stack.push_back(test);
						test = (test + 1) % count;
					}
					removals.push_back(potential);
				}
			}
			return false;
		}
		bool GenerateMesh(const Image &mask, double expansion, std::vector<Vector2> &points) {
			double centreX = 0, centreY = 0, count = 0;
			for (uint32_t y = 0; y < mask.Height; ++y)
				for (uint32_t x = 0; x < mask.Width; ++x) {
					SurfacePixel pixel{};
					if (!LoadSurfacePixel(mask, x, y, pixel)) return false;
					if (pixel[3] > 0) {
						centreX += x;
						centreY += y;
						++count;
					}
				}
			if (!count) return true;
			centreX /= count;
			centreY /= count;
			std::map<double, Vector2> boundary;
			for (uint32_t y = 0; y < mask.Height; ++y)
				for (uint32_t x = 0; x < mask.Width; ++x) {
					SurfacePixel pixel{};
					if (!LoadSurfacePixel(mask, x, y, pixel)) return false;
					if (pixel[3] <= 0) continue;
					const float uvX = (float(x) + .5f) / float(mask.Width),
								uvY = (float(y) + .5f) / float(mask.Height);
					const float vx = uvX * float(mask.Width) - float(centreX),
								vy = uvY * float(mask.Height) - float(centreY);
					const float length = std::sqrt(vx * vx + vy * vy);
					if (length == 0) return false;
					const float dx = vx / length / float(mask.Width), dy = vy / length / float(mask.Height);
					bool inner = false;
					for (uint32_t distance = 1; distance <= mask.Width + mask.Height; ++distance) {
						const float u = uvX + dx * float(distance), v = uvY + dy * float(distance);
						if (u < 0 || v < 0 || u > 1 || v > 1) continue;
						const float px = u * float(mask.Width), py = v * float(mask.Height);
						SurfacePixel sample{};
						if (!LoadSurfacePixel(
								mask,
								std::min(uint32_t(px), mask.Width - 1),
								std::min(uint32_t(py), mask.Height - 1),
								sample
							))
							return false;
						if (sample[3] == 1) {
							inner = true;
							break;
						}
					}
					if (!inner)
						boundary[Direction({centreX, centreY}, {double(x), double(y)})] = {
							double(x), double(y)
						};
				}
			for (auto iterator = boundary.rbegin(); iterator != boundary.rend(); ++iterator) {
				auto point = iterator->second;
				if (point.X > centreX) ++point.X;
				if (point.Y > centreY) ++point.Y;
				if (expansion != 0) {
					const double deltaX = point.X - centreX, deltaY = point.Y - centreY,
								 length = std::hypot(deltaX, deltaY);
					if (length == 0) return false;
					const double distance = std::max(.5, length + expansion);
					point = {centreX + deltaX / length * distance, centreY + deltaY / length * distance};
				}
				points.push_back(point);
			}
			RemoveCollinear(points);
			if (!RemoveConcave(points)) return false;
			std::map<double, Vector2> sorted;
			for (const auto point : points)
				sorted[Direction({centreX, centreY}, point)] = point;
			points.clear();
			for (auto iterator = sorted.rbegin(); iterator != sorted.rend(); ++iterator)
				points.push_back(iterator->second);
			return true;
		}
	}
	Status SourceRigidFracture(
		const Image &base,
		const Image &map,
		const RigidFractureSettings &settings,
		uint64_t maximumBytes,
		std::vector<RigidFracturePiece> &pieces,
		Diagnostic &diagnostic
	) {
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, "", "", std::string(message)};
			return code;
		};
		if (!ValidSurfaceLayout(base, MaximumFractureSide, Limits::MaximumArrayBytes) ||
			!ValidSurfaceLayout(map, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
			!FiniteSurfaceSamples(base) || !FiniteSurfaceSamples(map))
			return fail(Status::InvalidValue, "rigid fracture requires bounded finite image captures");
		if (!std::isfinite(settings.Threshold) || settings.Threshold < 0 || settings.Threshold > 1 ||
			!std::isfinite(settings.Expansion) || settings.Expansion < -2 || settings.Expansion > 2)
			return fail(Status::InvalidValue, "rigid fracture controls are outside their source ranges");
		const uint64_t pixelCount = uint64_t(base.Width) * base.Height;
		const uint64_t workspace =
			pixelCount * (sizeof(Label) * 2 + sizeof(SurfacePixel) + sizeof(Vector2) * 12 + 64) +
			256 * sizeof(RigidFracturePiece);
		if (workspace > maximumBytes)
			return fail(Status::LimitExceeded, "rigid fracture preallocation exceeds byte budget");
		std::vector<SurfacePixel> colours(pixelCount);
		std::vector<Label> current(pixelCount), next(pixelCount);
		Image quantized{1, 1, std::vector<uint8_t>(4)};
		for (uint32_t y = 0; y < base.Height; ++y)
			for (uint32_t x = 0; x < base.Width; ++x) {
				const size_t index = size_t(y) * base.Width + x;
				const uint32_t sourceX = std::min(uint32_t((x + .5) * map.Width / base.Width), map.Width - 1),
							   sourceY =
								   std::min(uint32_t((y + .5) * map.Height / base.Height), map.Height - 1);
				if (!LoadSurfacePixel(map, sourceX, sourceY, colours[index]))
					return fail(Status::InvalidValue, "fracture map cannot be sampled");
				if (!StoreSurfacePixel(quantized, 0, 0, colours[index]) ||
					!LoadSurfacePixel(quantized, 0, 0, colours[index]))
					return fail(Status::InvalidValue, "fracture map cannot be converted to source RGBA8");
				current[index] = {int32_t(x), int32_t(y), int32_t(x), int32_t(y)};
			}
		const auto blank = [](const SurfacePixel &colour) {
			return (colour[0] == 0 && colour[1] == 0 && colour[2] == 0) || colour[3] == 0;
		};
		constexpr std::array<std::array<int, 2>, 4> neighbours{{{-1, 0}, {0, -1}, {0, 1}, {1, 0}}};
		for (uint32_t iteration = 0; iteration < base.Width + base.Height; ++iteration) {
			for (uint32_t y = 0; y < base.Height; ++y)
				for (uint32_t x = 0; x < base.Width; ++x) {
					const size_t index = size_t(y) * base.Width + x;
					if (blank(colours[index])) {
						next[index] = {0, 0, 0, 0};
						continue;
					}
					auto label = current[index];
					for (const auto offset : neighbours) {
						const uint32_t neighbourX =
										   uint32_t(std::clamp(int(x) + offset[0], 0, int(base.Width) - 1)),
									   neighbourY =
										   uint32_t(std::clamp(int(y) + offset[1], 0, int(base.Height) - 1));
						const size_t neighbour = size_t(neighbourY) * base.Width + neighbourX;
						if (blank(colours[neighbour])) continue;
						float square = 0;
						for (size_t channel = 0; channel < 4; ++channel) {
							const float difference =
								float(colours[neighbour][channel]) - float(colours[index][channel]);
							square += difference * difference;
						}
						if (std::sqrt(square) > float(settings.Threshold)) continue;
						label[0] = std::min(label[0], current[neighbour][0]);
						label[1] = std::min(label[1], current[neighbour][1]);
						label[2] = std::max(label[2], current[neighbour][2]);
						label[3] = std::max(label[3], current[neighbour][3]);
					}
					next[index] = label;
				}
			current.swap(next);
		}
		std::map<uint32_t, Label> regions;
		for (const auto label : current)
			if (label != Label{0, 0, 0, 0})
				regions[uint32_t(label[1]) * base.Width + uint32_t(label[0])] = label;
		if (regions.size() > 256)
			return fail(Status::LimitExceeded, "rigid fracture exceeds native body count");
		uint64_t retainedBound = 0;
		for (const auto &[key, label] : regions) {
			(void)key;
			const uint64_t area = uint64_t(label[2] - label[0] + 1) * uint64_t(label[3] - label[1] + 1);
			retainedBound += sizeof(RigidFracturePiece) * 2 + area * 8 + 16 * sizeof(Vector2);
		}
		if (retainedBound > maximumBytes - workspace)
			return fail(Status::LimitExceeded, "rigid fracture piece storage exceeds admitted byte budget");
		std::vector<RigidFracturePiece> candidate;
		for (const auto &[key, label] : regions) {
			(void)key;
			const uint32_t width = uint32_t(label[2] - label[0] + 1),
						   height = uint32_t(label[3] - label[1] + 1);
			if (uint64_t(width) * height <= 4) continue;
			Image mask{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
			RigidFracturePiece piece;
			piece.Origin = {double(label[0]), double(label[1])};
			piece.Texture = {width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const uint32_t sourceX = x + uint32_t(label[0]), sourceY = y + uint32_t(label[1]);
					if (current[size_t(sourceY) * base.Width + sourceX] != label) continue;
					SurfacePixel colour{};
					if (!LoadSurfacePixel(base, sourceX, sourceY, colour) ||
						!StoreSurfacePixel(piece.Texture, x, y, colour) ||
						!StoreSurfacePixel(mask, x, y, {1, 1, 1, 1}))
						return fail(Status::InvalidValue, "rigid fracture texture capture is invalid");
				}
			if (!GenerateMesh(mask, settings.Expansion, piece.Points))
				return fail(
					Status::UnsupportedExecution,
					"rigid source mesh sampling reached an undefined or nonterminating case"
				);
			if (piece.Points.size() < 3) continue;
			if (piece.Points.size() > 8)
				return fail(
					Status::UnsupportedExecution,
					"rigid source fracture mesh exceeds pinned native Box2D convex vertex limit"
				);
			piece.Texture.Hash = SurfaceHash(piece.Texture);
			candidate.push_back(std::move(piece));
		}
		pieces = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}
}
namespace engine::imagegraph::detail {
	Status SourceRigidGenerateObjectMesh(
		const Image &texture,
		double expansion,
		bool addPixelForEmpty,
		uint64_t maximumBytes,
		std::vector<Vector2> &mesh,
		Diagnostic &diagnostic
	) {
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, "", "attribute_mesh", std::string(message)};
			return code;
		};
		if (!ValidSurfaceLayout(texture, MaximumFractureSide, Limits::MaximumArrayBytes) ||
			!FiniteSurfaceSamples(texture) || !std::isfinite(expansion) || std::abs(expansion) > 1000000)
			return fail(
				Status::InvalidValue,
				"rigid mesh action needs a finite bounded captured texture and expansion"
			);
		const uint64_t workspace = uint64_t(texture.Width) * texture.Height * 256 + 4096;
		if (workspace > maximumBytes)
			return fail(Status::LimitExceeded, "rigid mesh action preallocation exceeds byte budget");
		double centreX = 0, centreY = 0, count = 0;
		for (uint32_t y = 0; y < texture.Height; ++y)
			for (uint32_t x = 0; x < texture.Width; ++x) {
				SurfacePixel pixel{};
				if (!LoadSurfacePixel(texture, x, y, pixel))
					return fail(Status::InvalidValue, "rigid mesh action texture cannot be sampled");
				if (pixel[3] > 0) {
					centreX += x;
					centreY += y;
					++count;
				}
			}
		// The source button leaves its previous mesh unchanged for an entirely transparent texture.
		if (!count) {
			diagnostic = {};
			return Status::Ok;
		}
		centreX /= count;
		centreY /= count;
		std::vector<Vector2> candidate;
		if (!GenerateMesh(texture, expansion, candidate))
			return fail(
				Status::UnsupportedExecution,
				"rigid object mesh sampling reached an undefined or nonterminating source case"
			);
		if (candidate.empty() && addPixelForEmpty)
			candidate = {
				{centreX - .5, centreY - .5},
				{centreX + .5, centreY - .5},
				{centreX + .5, centreY + .5},
				{centreX - .5, centreY + .5}
			};
		if (!candidate.empty() && (candidate.size() < 3 || candidate.size() > 8))
			return fail(
				Status::UnsupportedExecution,
				"generated source object mesh is outside pinned native convex vertex limit"
			);
		mesh = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}
}
