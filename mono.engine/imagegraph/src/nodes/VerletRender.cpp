#include "Sampler.hpp"
#include "VerletNodes.hpp"

namespace engine::imagegraph::detail {
	namespace {
		using EdgeRecord = std::pair<uint64_t, size_t>;
		uint64_t EdgeKey(const VerletPoint &a, const VerletPoint &b) {
			return (uint64_t(a.SourceIndex + b.SourceIndex) << 32) | uint64_t(a.SourceIndex) * b.SourceIndex;
		}
		double Cross(Vector2 a, Vector2 b, Vector2 point) {
			return (b.X - a.X) * (point.Y - a.Y) - (b.Y - a.Y) * (point.X - a.X);
		}
		bool TopLeft(Vector2 a, Vector2 b) {
			return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
		}
		struct PixelBounds {
			uint32_t X0, Y0, X1, Y1;
		};
		PixelBounds Bounds(const Image &image, Vector2 a, Vector2 b, Vector2 c, double radius = 0) {
			return {
				uint32_t(
					std::clamp(std::ceil(std::min({a.X, b.X, c.X}) - radius - .5), 0.0, double(image.Width))
				),
				uint32_t(
					std::clamp(std::ceil(std::min({a.Y, b.Y, c.Y}) - radius - .5), 0.0, double(image.Height))
				),
				uint32_t(
					std::clamp(
						std::floor(std::max({a.X, b.X, c.X}) + radius - .5) + 1, 0.0, double(image.Width)
					)
				),
				uint32_t(
					std::clamp(
						std::floor(std::max({a.Y, b.Y, c.Y}) + radius - .5) + 1, 0.0, double(image.Height)
					)
				)
			};
		}
		bool AdmitWork(NodeContext &context, PixelBounds bounds, uint64_t &work) {
			const uint64_t count = uint64_t(bounds.X1 - bounds.X0) * (bounds.Y1 - bounds.Y0);
			if (count > 1'048'576 - work)
				return context.Fail(
					Status::LimitExceeded, "Verlet reference raster exceeds bounded sample work", "mesh"
				);
			work += count;
			return true;
		}
		bool Blend(Image &image, uint32_t x, uint32_t y, const Rgba &source) {
			const auto before = ReadPixel(image, x, y);
			Rgba after{};
			for (size_t channel = 0; channel < 4; ++channel)
				after[channel] = source[channel] * source[3] + before[channel] * (1 - source[3]);
			return WritePixel(image, x, y, after);
		}
		bool Triangle(
			NodeContext &context,
			Image &image,
			const Image *texture,
			bool filtered,
			const VerletPoint &p0,
			const VerletPoint &p1,
			const VerletPoint &p2,
			uint64_t &work
		) {
			const VerletPoint *points[]{&p0, &p1, &p2};
			Vector2 positions[]{
				p0.DrawPosition.value_or(p0.Position),
				p1.DrawPosition.value_or(p1.Position),
				p2.DrawPosition.value_or(p2.Position)
			};
			double area = Cross(positions[0], positions[1], positions[2]);
			if (!std::isfinite(area))
				return context.Fail(
					Status::InvalidValue, "Verlet reference triangle area is nonfinite", "mesh"
				);
			if (area == 0) return true;
			if (area < 0) {
				std::swap(points[1], points[2]);
				std::swap(positions[1], positions[2]);
				area = -area;
			}
			const auto bounds = Bounds(image, positions[0], positions[1], positions[2]);
			if (!AdmitWork(context, bounds, work)) return false;
			for (uint32_t y = bounds.Y0; y < bounds.Y1; ++y)
				for (uint32_t x = bounds.X0; x < bounds.X1; ++x) {
					const Vector2 center{double(x) + .5, double(y) + .5};
					const double weights[]{
						Cross(positions[1], positions[2], center),
						Cross(positions[2], positions[0], center),
						Cross(positions[0], positions[1], center)
					};
					bool inside = true;
					for (size_t index = 0; index < 3; ++index)
						if (weights[index] < 0 ||
							(weights[index] == 0 &&
							 !TopLeft(positions[(index + 1) % 3], positions[(index + 2) % 3])))
							inside = false;
					if (!inside) continue;
					Vector2 uv{};
					Rgba blend{};
					for (size_t index = 0; index < 3; ++index) {
						const double weight = weights[index] / area;
						uv.X += points[index]->UV.X * weight;
						uv.Y += points[index]->UV.Y * weight;
						for (size_t channel = 0; channel < 3; ++channel)
							blend[channel] +=
								double((points[index]->Blend >> (channel * 8)) & 255) / 255 * weight;
					}
					Rgba color = texture ? Texture(*texture, uv.X, uv.Y, filtered) : Rgba{1, 1, 1, 1};
					for (size_t channel = 0; channel < 3; ++channel)
						color[channel] *= blend[channel];
					if (!Blend(image, x, y, color))
						return context.Fail(
							Status::InvalidValue,
							"Verlet reference shading produced invalid pixels",
							"texture"
						);
				}
			return true;
		}
		bool
		Line(NodeContext &context, Image &image, Vector2 a, Vector2 b, const Rgba &color, uint64_t &work) {
			const auto bounds = Bounds(image, a, b, b, .5);
			if (!AdmitWork(context, bounds, work)) return false;
			const double dx = b.X - a.X, dy = b.Y - a.Y, length = dx * dx + dy * dy;
			if (!std::isfinite(length))
				return context.Fail(
					Status::InvalidValue, "Verlet reference line length is nonfinite", "mesh"
				);
			for (uint32_t y = bounds.Y0; y < bounds.Y1; ++y)
				for (uint32_t x = bounds.X0; x < bounds.X1; ++x) {
					const Vector2 center{double(x) + .5, double(y) + .5};
					const double t =
						length == 0
							? 0
							: std::clamp(((center.X - a.X) * dx + (center.Y - a.Y) * dy) / length, 0.0, 1.0);
					const double ex = center.X - a.X - t * dx, ey = center.Y - a.Y - t * dy;
					if (ex * ex + ey * ey <= .25 && !Blend(image, x, y, color))
						return context.Fail(
							Status::InvalidValue, "Verlet reference wire pixels are invalid", "color"
						);
				}
			return true;
		}
	}
	// Pixel-center top-left triangles and round-capped one-pixel wires are the CPU reference profile.
	// GPU rasterization coverage is a host capability, so callers requesting it receive a named gate.
	bool VerletRenderMesh(NodeContext &context) {
		if (context.InlineOwnerId.empty() || context.InlineOwnerType != "pc.verlet_sim_inline") return true;
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"source Verlet GPU raster coverage requires a captured renderer",
				"surface_out"
			);
		const auto *input = context.Find("mesh");
		const auto *source = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!source || !source->Data || !source->Data->Verlet) return true;
		if (!ValidMesh2DPayload(*source))
			return context.Fail(Status::InvalidValue, "Verlet render mesh is invalid", "mesh");
		MeshValue2D stepped;
		const MeshData2D *mesh = &*source->Data;
		if (context.Boolean("step", true)) {
			if (!VerletRenderStepMesh(context)) return false;
			for (auto &output : context.OutputValues)
				if (output.Port == "mesh") stepped = std::move(std::get<MeshValue2D>(output.Data));
			context.OutputValues.clear();
			if (!stepped.Data)
				return context.Fail(Status::InvalidOutput, "Verlet render step did not publish mesh", "mesh");
			mesh = &*stepped.Data;
		}
		const Vector2 dimension = VerletScopeDimension(context);
		if (!MeshFinite(dimension) || dimension.X < 1 || dimension.Y < 1 ||
			dimension.X != std::trunc(dimension.X) || dimension.Y != std::trunc(dimension.Y) ||
			dimension.X > Limits::MaximumDimension || dimension.Y > Limits::MaximumDimension)
			return context.Fail(
				Status::LimitExceeded, "Verlet render owner dimension exceeds native bounds", "dimension"
			);
		const int64_t type = context.Integer("type");
		const double trim = context.Scalar("trim", 1);
		const bool inverse = context.Boolean("invert_order");
		if (type < 0 || type > 1)
			return context.Fail(Status::InvalidValue, "Verlet render type is invalid", "type");
		const Image *texture = context.Input("texture");
		if (texture && (!ValidSurfaceLayout(*texture, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
						!FiniteSurfaceSamples(*texture)))
			return context.Fail(Status::InvalidValue, "Verlet render texture is invalid", "texture");
		const bool filtered = context.Integer("interpolate", context.InheritedInterpolation) > 1;
		if (context.FailureCode != Status::Ok) return false;
		auto *image = context.NewImage("surface_out", uint32_t(dimension.X), uint32_t(dimension.Y));
		if (!image) return false;
		const auto &points = mesh->Simulation.Points;
		const auto &edges = mesh->Simulation.Edges;
		const size_t available = type == 1			 ? edges.size()
								 : mesh->VerletQuads ? mesh->Quads.size()
													 : mesh->Triangles.size();
		const double requested = std::ceil(available * trim);
		if (requested > available)
			return context.Fail(
				Status::InvalidValue, "source Verlet Trim indexes beyond primitive array", "trim"
			);
		const size_t count = requested > 0 ? size_t(requested) : 0;
		uint64_t work = 0;
		if (type == 1) {
			const Colour color = context.Get<Colour>("color", Colour{255, 255, 255, 255});
			const Rgba rgba{
				double(color.Red) / 255, double(color.Green) / 255, double(color.Blue) / 255, 1.0
			};
			for (size_t index = 0; index < count; ++index) {
				const auto &edge = edges[inverse ? available - 1 - index : index];
				if (edge.Active &&
					!Line(
						context, *image, points[edge.First].Position, points[edge.Second].Position, rgba, work
					))
					return false;
			}
			return true;
		}
		auto lease = context.ReserveWorkspace(edges.size() * sizeof(EdgeRecord), "mesh");
		if (!lease) return false;
		std::vector<EdgeRecord> edgeMap;
		edgeMap.reserve(edges.size());
		for (size_t index = 0; index < edges.size(); ++index)
			edgeMap.emplace_back(EdgeKey(points[edges[index].First], points[edges[index].Second]), index);
		std::sort(edgeMap.begin(), edgeMap.end());
		const auto triangleEdges = [&](const std::array<uint32_t, 3> &triangle) {
			std::array<size_t, 3> found{size_t(-1), size_t(-1), size_t(-1)};
			for (size_t index = 0; index < 3; ++index) {
				const auto key = EdgeKey(points[triangle[index]], points[triangle[(index + 1) % 3]]);
				const auto end =
					std::upper_bound(edgeMap.begin(), edgeMap.end(), EdgeRecord{key, size_t(-1)});
				if (end != edgeMap.begin() && (end - 1)->first == key) found[index] = (end - 1)->second;
			}
			return found;
		};
		const auto submit = [&](const std::array<uint32_t, 3> &triangle) {
			return Triangle(
				context,
				*image,
				texture,
				filtered,
				points[triangle[0]],
				points[triangle[1]],
				points[triangle[2]],
				work
			);
		};
		for (size_t index = 0; index < count; ++index) {
			const size_t selected = inverse ? available - 1 - index : index;
			if (!mesh->VerletQuads) {
				const auto &triangle = mesh->Triangles[selected];
				bool active = true;
				for (const auto edge : triangleEdges(triangle))
					if (edge != size_t(-1) && !edges[edge].Active) active = false;
				if (active && !submit(triangle)) return false;
			} else {
				const auto quad = mesh->Quads[selected];
				const auto &first = mesh->Triangles[quad[0]], &second = mesh->Triangles[quad[1]];
				std::array<size_t, 6> unique{};
				size_t size = 0;
				for (const auto list : {triangleEdges(first), triangleEdges(second)})
					for (const auto edge : list)
						if (std::find(unique.begin(), unique.begin() + size, edge) == unique.begin() + size)
							unique[size++] = edge;
				bool active = true;
				for (size_t member = 0; member < std::min(size, size_t{4}); ++member)
					if (unique[member] != size_t(-1) && !edges[unique[member]].Active) active = false;
				if (active && (!submit(first) || !submit(second))) return false;
			}
		}
		return true;
	}
}
