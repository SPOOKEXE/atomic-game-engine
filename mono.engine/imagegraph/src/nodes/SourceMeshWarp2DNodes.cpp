#include "../Mesh2DPayload.hpp"
#include "../SourceRandom.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "SourcePolygon2D.hpp"

namespace engine::imagegraph::detail {
	namespace {
		const Value *SourceProperty(const NodeContext &context, std::string_view name) {
			for (const auto &property : context.Authored.SourceProperties)
				if (property.Port == name) return &property.Data;
			return nullptr;
		}
		struct WarpLink {
			uint32_t First, Second;
			double Length, Spring;
		};
		double Distance(Vector2 a, Vector2 b) {
			return std::hypot(a.X - b.X, a.Y - b.Y);
		}
		bool Numbers(const Value &value, std::array<double, 7> &numbers) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array || array->Elements.size() != 7 || !array->Nested.empty() || !array->Items.empty())
				return false;
			for (size_t i = 0; i < 7; ++i) {
				const auto &element = array->Elements[i];
				if (const auto *scalar = std::get_if<double>(&element))
					numbers[i] = *scalar;
				else if (const auto *integer = std::get_if<int64_t>(&element))
					numbers[i] = double(*integer);
				else
					return false;
				if (!std::isfinite(numbers[i])) return false;
			}
			return true;
		}
	}
	bool SourceMeshWarp(NodeContext &context) {
		const Image *source = context.Input("surface_in");
		MeshValue2D result;
		auto &mesh = result.Data.emplace();
		mesh.Warp = true;
		mesh.Bounds = {0, 0, 1, 1};
		if (!source) {
			context.SetValue("mesh_data", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		const int64_t sample = context.Integer("sample", 8), type = context.Integer("mesh_type");
		if (sample < 1 || sample > 63 || type < 0 || type > 1)
			return context.Fail(
				Status::InvalidValue, "mesh warp grid or type is outside its range", "sample"
			);
		const size_t side = size_t(sample) + 1;
		auto workspace = context.ReserveWorkspace(32ull * 1024 * 1024, "mesh_data");
		if (!workspace || !context.ReserveOutput(
							  sizeof(MeshData2D) + Limits::MaximumArrayElements * sizeof(VerletPoint) +
								  Limits::MaximumLinks * sizeof(std::array<uint32_t, 3>),
							  "mesh_data"
						  ))
			return false;
		std::vector<WarpLink> links;
		links.reserve(Limits::MaximumLinks);
		const auto addPoint = [&](Vector2 position) -> int32_t {
			if (mesh.Simulation.Points.size() == Limits::MaximumArrayElements) return -1;
			VerletPoint point;
			point.Position = position;
			point.UV = {position.X / source->Width, position.Y / source->Height};
			mesh.Simulation.Points.push_back(point);
			return int32_t(mesh.Simulation.Points.size() - 1);
		};
		const auto addLink = [&](int32_t first, int32_t second, double spring = 1) {
			if (first < 0 || second < 0) return true;
			if (links.size() == Limits::MaximumLinks) return false;
			links.push_back(
				{uint32_t(first),
				 uint32_t(second),
				 Distance(mesh.Simulation.Points[first].Position, mesh.Simulation.Points[second].Position),
				 spring}
			);
			return true;
		};
		if (type == 0) {
			std::vector<int32_t> grid(side * side, -1);
			std::vector<bool> filled(side * side, true);
			const double cellX = double(source->Width) / sample, cellY = double(source->Height) / sample;
			if (!context.Boolean("full_mesh"))
				for (size_t y = 0; y < side; ++y)
					for (size_t x = 0; x < side; ++x) {
						const auto pixel = ReadPixel(
							*source,
							std::min<uint32_t>(uint32_t(std::round(x * cellX)), source->Width - 1),
							std::min<uint32_t>(uint32_t(std::round(y * cellY)), source->Height - 1)
						);
						filled[y * side + x] =
							(pixel[0] * .2126 + pixel[1] * .7152 + pixel[2] * .0722) * pixel[3] > 0;
					}
			for (size_t y = 0; y < side; ++y)
				for (size_t x = 0; x < side; ++x) {
					bool present = false;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx) {
							const int64_t nx = int64_t(x) + dx, ny = int64_t(y) + dy;
							if (nx >= 0 && ny >= 0 && nx < int64_t(side) && ny < int64_t(side))
								present = present || filled[ny * side + nx];
						}
					if (!present) continue;
					const int32_t p = grid[y * side + x] = addPoint(
						{std::min(x * cellX, double(source->Width)),
						 std::min(y * cellY, double(source->Height))}
					);
					if (p < 0)
						return context.Fail(Status::LimitExceeded, "mesh warp exceeds point limit", "sample");
					if (y == 0) continue;
					if (x && grid[(y - 1) * side + x] >= 0 && grid[y * side + x - 1] >= 0)
						mesh.Triangles.push_back(
							{uint32_t(grid[(y - 1) * side + x]),
							 uint32_t(grid[y * side + x - 1]),
							 uint32_t(p)}
						);
					if (x < size_t(sample) && grid[(y - 1) * side + x] >= 0 &&
						grid[(y - 1) * side + x + 1] >= 0)
						mesh.Triangles.push_back(
							{uint32_t(grid[(y - 1) * side + x]),
							 uint32_t(grid[(y - 1) * side + x + 1]),
							 uint32_t(p)}
						);
				}
			for (size_t y = 0; y < side; ++y)
				for (size_t x = 0; x < side; ++x) {
					const int32_t p0 = y && x ? grid[(y - 1) * side + x - 1] : -1,
								  p1 = y ? grid[(y - 1) * side + x] : -1,
								  p2 = x ? grid[y * side + x - 1] : -1, p3 = grid[y * side + x];
					if (!addLink(p3, p1) || !addLink(p3, p2))
						return context.Fail(Status::LimitExceeded, "mesh warp exceeds link limit", "sample");
					const bool diagonal0 = p0 >= 0 && p3 >= 0, diagonal1 = p1 >= 0 && p2 >= 0;
					if (context.Boolean("diagonal_link") || diagonal0 != diagonal1)
						if (!addLink(p0, p3, context.Scalar("spring_force", .5)) ||
							!addLink(p1, p2, context.Scalar("spring_force", .5)))
							return context.Fail(
								Status::LimitExceeded, "mesh warp exceeds link limit", "sample"
							);
				}
		} else {
			std::vector<Vector2> polygon;
			const Value *bound = SourceProperty(context, "mesh_bound");
			if (!bound) bound = context.Find("attribute_mesh_bound");
			const auto *array = bound ? std::get_if<ArrayValue>(bound) : nullptr;
			if (array)
				for (const auto &element : array->Elements) {
					const auto *point = std::get_if<Vector2>(&element);
					if (!point)
						return context.Fail(
							Status::InvalidValue, "custom mesh boundary requires XY points", "mesh_bound"
						);
					polygon.push_back(*point);
				}
			if (polygon.size() >= 3) {
				Vector2 minimum = polygon.front(), maximum = minimum;
				size_t boundaryCount = polygon.size();
				const double spacing = double(std::min(source->Width, source->Height)) / sample;
				for (size_t i = 0; i < polygon.size(); ++i) {
					minimum.X = std::min(minimum.X, polygon[i].X);
					minimum.Y = std::min(minimum.Y, polygon[i].Y);
					maximum.X = std::max(maximum.X, polygon[i].X);
					maximum.Y = std::max(maximum.Y, polygon[i].Y);
					const double count =
						std::round(Distance(polygon[i], polygon[(i + 1) % polygon.size()]) / spacing);
					if (!std::isfinite(count) || count > Limits::MaximumArrayElements - boundaryCount)
						return context.Fail(
							Status::LimitExceeded, "custom mesh boundary exceeds point limit", "mesh_bound"
						);
					boundaryCount += size_t(count);
				}
				if (boundaryCount + side * side > Limits::MaximumArrayElements)
					return context.Fail(Status::LimitExceeded, "custom mesh exceeds point limit", "sample");
				std::vector<Vector2> interior;
				SourceRandom random(uint32_t(context.Integer("seed")));
				const auto range = [&](double amplitude) {
					if (amplitude == 0) return 0.;
					const double value = (2 * random.Unit() - 1) * amplitude;
					random.Unit();
					return value;
				};
				for (size_t x = 0; x < side; ++x)
					for (size_t y = 0; y < side; ++y) {
						Vector2 point{
							std::lerp(minimum.X, maximum.X, double(x) / sample) +
								range(double(source->Width) / sample / 3 * context.Scalar("randomness", .5)),
							std::lerp(minimum.Y, maximum.Y, double(y) / sample) +
								range(double(source->Height) / sample / 3 * context.Scalar("randomness", .5))
						};
						if (polygon2d::PolygonContains(polygon, point)) interior.push_back(point);
					}
				for (size_t i = 0; i < polygon.size(); ++i) {
					const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
					addPoint(a);
					const size_t count = size_t(std::round(Distance(a, b) / spacing));
					for (size_t j = 0; j < count; ++j)
						addPoint(
							{std::lerp(a.X, b.X, double(j) / count), std::lerp(a.Y, b.Y, double(j) / count)}
						);
				}
				for (const auto point : interior)
					addPoint(point);
				std::vector<Vector2> positions;
				for (const auto &point : mesh.Simulation.Points)
					positions.push_back(point.Position);
				if (!polygon2d::Delaunay(positions, mesh.Triangles, polygon))
					return context.Fail(
						Status::LimitExceeded, "custom mesh triangulation exceeds native bounds", "mesh_bound"
					);
				for (const auto &triangle : mesh.Triangles)
					for (size_t i = 0; i < 3; ++i)
						if (!addLink(triangle[i], triangle[(i + 1) % 3]))
							return context.Fail(
								Status::LimitExceeded, "custom mesh exceeds link limit", "mesh_bound"
							);
			}
		}
		if (const auto *pinValue = SourceProperty(context, "pin")) {
			const auto *pins = std::get_if<ArrayValue>(pinValue);
			if (!pins) return context.Fail(Status::InvalidValue, "mesh pins require integer indices", "pin");
			for (const auto &element : pins->Elements) {
				const auto *index = std::get_if<int64_t>(&element);
				if (!index || *index < 0 || uint64_t(*index) >= mesh.Simulation.Points.size())
					return context.Fail(Status::InvalidValue, "mesh pin index is outside topology", "pin");
				mesh.Simulation.Points[*index].Pin = true;
			}
		}
		std::vector<std::array<double, 7>> controls;
		for (const auto &input : context.Authored.DynamicInputs) {
			const Value *value = context.Find(input.Id);
			if (!value) value = SourceProperty(context, input.Id);
			if (!value) continue;
			std::array<double, 7> control;
			if (!Numbers(*value, control))
				return context.Fail(
					Status::InvalidValue, "Puppet requires seven numeric source fields", input.Id
				);
			if (control[0] < 0 || control[0] > 2 || std::trunc(control[0]) != control[0])
				return context.Fail(Status::InvalidValue, "Puppet mode is invalid", input.Id);
			controls.push_back(control);
		}
		std::vector<Vector2> moves(mesh.Simulation.Points.size());
		for (size_t i = 0; i < mesh.Simulation.Points.size(); ++i) {
			auto &point = mesh.Simulation.Points[i];
			if (point.Pin) continue;
			double totalWeight = 0;
			Vector2 puppetMove{};
			for (const auto &control : controls) {
				const Vector2 origin{control[1], control[2]};
				const double angle = -control[4] * std::numbers::pi / 180;
				if (control[0] == 2) {
					const double distance = Distance(point.Position, origin);
					if (distance == 0)
						return context.Fail(
							Status::InvalidValue, "Puppet weight is undefined at its center", "control_point"
						);
					const double weight = 1 / distance;
					totalWeight += weight;
					puppetMove.X += weight * control[3];
					puppetMove.Y += weight * control[4];
					continue;
				}
				if (control[5] == 0)
					return context.Fail(
						Status::InvalidValue, "Puppet influence width is zero", "control_point"
					);
				const double distance = control[0] == 0
											? Distance(point.Position, origin)
											: std::abs(
												  (point.Position.X - origin.X) * std::sin(angle) -
												  (point.Position.Y - origin.Y) * std::cos(angle)
											  );
				const double ratio = std::clamp(1 - distance / control[5], 0., 1.),
							 influence =
								 ratio < .5 ? 4 * ratio * ratio * ratio : 1 - std::pow(-2 * ratio + 2, 3) / 2;
				moves[i].X += control[3] * influence * (control[0] == 0 ? 1 : std::cos(angle));
				moves[i].Y += (control[0] == 0 ? control[4] : control[3] * std::sin(angle)) * influence;
			}
			if (totalWeight) {
				moves[i].X += puppetMove.X / totalWeight;
				moves[i].Y += puppetMove.Y / totalWeight;
			}
		}
		const int64_t iterations = context.Integer("attribute_iteration", 4);
		if (iterations < 0 || iterations > 4096)
			return context.Fail(
				Status::InvalidValue, "mesh warp iterations are outside native bounds", "attribute_iteration"
			);
		for (int64_t step = 0; step < iterations; ++step) {
			for (size_t i = 0; i < moves.size(); ++i)
				if (!mesh.Simulation.Points[i].Pin) {
					mesh.Simulation.Points[i].Position.X += moves[i].X / iterations;
					mesh.Simulation.Points[i].Position.Y += moves[i].Y / iterations;
				}
			const double strength = context.Scalar("link_strength");
			if (strength <= 0) continue;
			for (const auto &link : links) {
				auto &a = mesh.Simulation.Points[link.First], &b = mesh.Simulation.Points[link.Second];
				const double distance = Distance(a.Position, b.Position),
							 force = link.Spring * (distance - std::lerp(distance, link.Length, strength));
				const double scale = distance ? force / distance / 2 : 0;
				const Vector2 delta{
					(b.Position.X - a.Position.X) * scale, (b.Position.Y - a.Position.Y) * scale
				};
				if (!a.Pin) {
					a.Position.X += delta.X;
					a.Position.Y += delta.Y;
				}
				if (!b.Pin) {
					b.Position.X -= delta.X;
					b.Position.Y -= delta.Y;
				}
			}
		}
		if (!ValidMesh2DPayload(result))
			return context.Fail(Status::InvalidValue, "mesh warp produced nonfinite geometry", "mesh_data");
		if (uint64_t(source->Width) * source->Height >
			128ull * 1024 * 1024 / std::max<size_t>(1, mesh.Triangles.size()))
			return context.Fail(
				Status::LimitExceeded, "mesh warp rasterization exceeds work limit", "surface_out"
			);
		// Source attribute_surface_depth initializes image-input nodes to the Input choice.
		const auto format = context.Find("attribute_color_depth")
								? ResolveProcessorSurfaceFormat(context, source)
								: std::optional<SurfaceFormat>{source->Format};
		if (!format) return false;
		const auto sampler = ReadSampler(context);
		if (!SupportedSampler(context, sampler)) return false;
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				Rgba color = mesh.Triangles.empty() ? ReadPixel(*source, x, y) : Rgba{};
				const Vector2 pixel{x + .5, y + .5};
				for (const auto &triangle : mesh.Triangles) {
					const auto &a = mesh.Simulation.Points[triangle[0]],
							   &b = mesh.Simulation.Points[triangle[1]],
							   &c = mesh.Simulation.Points[triangle[2]];
					const double determinant = (b.Position.Y - c.Position.Y) * (a.Position.X - c.Position.X) +
											   (c.Position.X - b.Position.X) * (a.Position.Y - c.Position.Y);
					if (determinant == 0) continue;
					const double wa = ((b.Position.Y - c.Position.Y) * (pixel.X - c.Position.X) +
									   (c.Position.X - b.Position.X) * (pixel.Y - c.Position.Y)) /
									  determinant,
								 wb = ((c.Position.Y - a.Position.Y) * (pixel.X - c.Position.X) +
									   (a.Position.X - c.Position.X) * (pixel.Y - c.Position.Y)) /
									  determinant,
								 wc = 1 - wa - wb;
					if (wa < 0 || wb < 0 || wc < 0) continue;
					color = SampleTexture(
						*source,
						wa * a.UV.X + wb * b.UV.X + wc * c.UV.X,
						wa * a.UV.Y + wb * b.UV.Y + wc * c.UV.Y,
						sampler
					);
				}
				if (!WritePixel(*output, x, y, color))
					return context.Fail(
						Status::InvalidValue, "mesh warp texture sample is nonfinite", "surface_out"
					);
			}
		context.SetValue("mesh_data", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
