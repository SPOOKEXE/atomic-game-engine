#include "../Mesh2DPayload.hpp"
#include "Families.hpp"
#include "Processor.hpp"
#include "SourcePolygon2D.hpp"
#include "SourceShape2D.hpp"

namespace engine::imagegraph::detail {
	namespace {
		Vector2 SourceRotate(Vector2 point, double degrees) {
			const double angle = -degrees * std::numbers::pi / 180;
			return {
				point.X * std::cos(angle) - point.Y * std::sin(angle),
				point.X * std::sin(angle) + point.Y * std::cos(angle)
			};
		}
		Vector2 PixelVector(NodeContext &context, std::string_view name, uint32_t width, uint32_t height) {
			auto value = context.Vec2(name);
			if (!context.IsLinked(name) && context.Integer(std::string(name) + "_unit", 1) == 1) {
				value.X *= width;
				value.Y *= height;
			}
			return value;
		}
		Rgba Multiply(Colour a, Colour b, Colour palette) {
			Rgba output{};
			const std::array<unsigned, 3> first{a.Red, a.Green, a.Blue}, second{b.Red, b.Green, b.Blue},
				third{palette.Red, palette.Green, palette.Blue};
			for (size_t i = 0; i < 3; ++i)
				output[i] = double((first[i] * second[i] / 255) * third[i] / 255) / 255;
			output[3] = 1;
			return output;
		}
		struct ColoredTriangle {
			std::array<Vector2, 3> Points;
			std::array<Rgba, 3> Colors;
		};
		Rgba RasterSample(std::span<const ColoredTriangle> triangles, Vector2 pixel) {
			Rgba result{};
			for (const auto &triangle : triangles) {
				const auto a = triangle.Points[0], b = triangle.Points[1], c = triangle.Points[2];
				const double area = polygon2d::Cross(a, b, c);
				if (area == 0) continue;
				const double x = polygon2d::Cross(b, c, pixel) / area,
							 y = polygon2d::Cross(c, a, pixel) / area, z = 1 - x - y;
				if (x < 0 || y < 0 || z < 0) continue;
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] = x * triangle.Colors[0][channel] + y * triangle.Colors[1][channel] +
									  z * triangle.Colors[2][channel];
			}
			return result;
		}
	}
	bool SourceShapePolygon(NodeContext &context) {
		uint32_t width = 0, height = 0;
		if (!ResolveDimension(context, "dimension", width, height)) return false;
		const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
		if (!format) return false;
		const auto shape = context.Integer("shape"), ssaa = context.Integer("ssaa"),
				   background = context.Integer("background"), bgBlend = context.Integer("bg_blend_mode");
		if (shape < 0 || shape > 19 || shape == 4 || shape == 11 || shape == 15 || shape == 17 || ssaa < 0 ||
			ssaa > 3 || background < 0 || background > 2 || bgBlend < 0 || bgBlend > 1)
			return context.Fail(
				Status::InvalidValue, "polygon shape or render choice is outside its range", "shape"
			);
		shape2d::Data data;
		const auto scale = PixelVector(context, "scale", width, height),
				   position = PixelVector(context, "position", width, height),
				   range = context.Vec2("angle_range", {0, 360});
		data.scale = {scale.X, scale.Y};
		data.radRan = {range.X, range.Y};
		data.side = context.Integer("sides", 16);
		data.inner = context.Scalar("inner_radius", .5);
		data.radius = context.Scalar("radius", .5);
		data.teeth = context.Integer("teeth", 6);
		data.teethH = context.Scalar("teeth_height", .2);
		data.teethT = context.Scalar("teeth_taper");
		data.cap = context.Boolean("round_cap");
		data.explode = context.Scalar("explode");
		data.trep = context.Scalar("trapezoid_sides", .5);
		data.palAng = context.Scalar("skew", .5);
		data.factor = context.Scalar("factor", 3);
		const size_t bound = static_cast<size_t>(
			std::max({4 * std::max(3., data.side) + 4, 6 * std::max(3., data.teeth), 10.})
		);
		if (bound > Limits::MaximumArrayElements / 3)
			return context.Fail(Status::LimitExceeded, "polygon shape exceeds topology limits", "sides");
		const Value *inputValue = context.Find("mesh");
		const auto *input = inputValue ? std::get_if<MeshValue2D>(inputValue) : nullptr;
		const uint64_t inputTriangles = input && input->Data ? input->Data->Triangles.size() : 0;
		auto workspace =
			context.ReserveWorkspace(bound * 2048 + inputTriangles * sizeof(ColoredTriangle), "shape");
		if (!workspace) return false;
		if (!context.ReserveOutput(
				sizeof(MeshData2D) + bound * 3 * sizeof(VerletPoint) +
					bound * sizeof(std::array<uint32_t, 3>) + sizeof(Path2D) + bound * 2 * sizeof(PathAnchor),
				"mesh"
			))
			return false;
		MeshValue2D mesh;
		auto &outputMesh = mesh.Data.emplace();
		outputMesh.Bounds = {0, 0, 1, 1};
		Path2D path;
		path.Segmented = true;
		std::vector<ColoredTriangle> triangles;
		if (input && input->Data) {
			if (!ValidMesh2DPayload(*input))
				return context.Fail(Status::InvalidValue, "polygon input mesh is invalid", "mesh");
			for (const auto &indices : input->Data->Triangles) {
				ColoredTriangle triangle;
				for (size_t i = 0; i < 3; ++i) {
					triangle.Points[i] = input->Data->Simulation.Points[indices[i]].Position;
					triangle.Colors[i] = {1, 1, 1, 1};
				}
				triangles.push_back(triangle);
			}
		} else {
			using Kernel = shape2d::Geometry (*)(const shape2d::Data &);
			static constexpr Kernel kernels[] = {
				shape2d::rectangle,
				shape2d::diamond,
				shape2d::trapezoid,
				shape2d::parallelogram,
				nullptr,
				shape2d::circle,
				shape2d::arc,
				shape2d::ring,
				shape2d::crescent,
				shape2d::pie,
				shape2d::squircle,
				nullptr,
				shape2d::reg_poly,
				shape2d::star,
				shape2d::cross,
				nullptr,
				shape2d::capsule,
				nullptr,
				shape2d::leaf,
				shape2d::gear
			};
			auto geometry = kernels[shape](data);
			const double rotation = context.Scalar("rotation"),
						 pieceRotation = context.Scalar("piece_rotation"),
						 pieceScale = context.Scalar("piece_scale", 1);
			const Colour shapeColor = context.Get<Colour>("shape_color", {255, 255, 255, 255});
			const std::array<Colour, 3> vertexColors{
				context.Get<Colour>("vertex_color_1", {255, 255, 255, 255}),
				context.Get<Colour>("vertex_color_2", {255, 255, 255, 255}),
				context.Get<Colour>("vertex_color_3", {255, 255, 255, 255})
			};
			std::vector<Colour> palette;
			if (const auto *value = context.Find("shape_palette"))
				if (const auto *array = std::get_if<ArrayValue>(value))
					for (const auto &item : array->Elements)
						if (const auto *color = std::get_if<Colour>(&item)) palette.push_back(*color);
			for (const auto &point : geometry.Segment) {
				const auto transformed = SourceRotate(point, rotation);
				path.Anchors.push_back(
					{{position.X + transformed.X, position.Y + transformed.Y, 0, 0, 0, 0}}
				);
			}
			for (auto &object : geometry.Objects)
				for (size_t index = 0; index < object.Triangles.size(); ++index) {
					auto points = object.Triangles[index];
					const Vector2 center{
						(points[0].X + points[1].X + points[2].X) / 3,
						(points[0].Y + points[1].Y + points[2].Y) / 3
					};
					const size_t paletteIndex = object.Rectangle ? index / 2 : index;
					const Colour color =
						palette.empty() ? Colour{255, 255, 255, 255} : palette[paletteIndex % palette.size()];
					ColoredTriangle triangle;
					std::array<uint32_t, 3> indices{};
					for (size_t i = 0; i < 3; ++i) {
						const auto local =
							SourceRotate({points[i].X - center.X, points[i].Y - center.Y}, pieceRotation);
						const auto transformed = SourceRotate(
							{center.X + pieceScale * local.X, center.Y + pieceScale * local.Y}, rotation
						);
						triangle.Points[i] = {position.X + transformed.X, position.Y + transformed.Y};
						triangle.Colors[i] = Multiply(shapeColor, vertexColors[i], color);
						VerletPoint point;
						point.Position = triangle.Points[i];
						indices[i] = outputMesh.Simulation.Points.size();
						outputMesh.Simulation.Points.push_back(point);
					}
					triangles.push_back(triangle);
					outputMesh.Triangles.push_back(indices);
				}
			if (!outputMesh.Triangles.empty()) {
				outputMesh.Bounds = {INFINITY, INFINITY, -INFINITY, -INFINITY};
				for (const auto &point : outputMesh.Simulation.Points) {
					outputMesh.Center.X += point.Position.X;
					outputMesh.Center.Y += point.Position.Y;
					outputMesh.Bounds[0] = std::min(outputMesh.Bounds[0], point.Position.X);
					outputMesh.Bounds[1] = std::min(outputMesh.Bounds[1], point.Position.Y);
					outputMesh.Bounds[2] = std::max(outputMesh.Bounds[2], point.Position.X);
					outputMesh.Bounds[3] = std::max(outputMesh.Bounds[3], point.Position.Y);
				}
				outputMesh.Center.X /= outputMesh.Simulation.Points.size();
				outputMesh.Center.Y /= outputMesh.Simulation.Points.size();
			}
		}
		if (!ValidMesh2DPayload(mesh))
			return context.Fail(Status::InvalidValue, "polygon generated nonfinite geometry", "shape");
		const uint32_t aa = 1u << ssaa;
		// CPU primitive rasterization has an explicit work ceiling as well as payload limits.
		constexpr uint64_t maximumTriangleTests = 128ull * 1024 * 1024;
		const uint64_t samples = uint64_t(width) * height * aa * aa;
		if (samples > maximumTriangleTests / std::max<size_t>(1, triangles.size()))
			return context.Fail(
				Status::LimitExceeded, "polygon rasterization exceeds primitive work limit", "ssaa"
			);
		context.SetValue("mesh", std::move(mesh));
		context.SetValue("path", std::move(path));
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		const Image *bg = context.Input("bg_surface");
		const auto solid = context.Get<Colour>("bg_color", {0, 0, 0, 255});
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				Rgba sum{};
				for (uint32_t i = 0; i < aa; ++i)
					for (uint32_t j = 0; j < aa; ++j) {
						// sh_downsample reads from the high-resolution sample at the pixel center, then
						// advances.
						const Vector2 sample{
							std::min(width * aa - 1., std::floor((x + .5) * aa) + i) + .5,
							std::min(height * aa - 1., std::floor((y + .5) * aa) + j) + .5
						};
						const auto color = RasterSample(triangles, {sample.X / aa, sample.Y / aa});
						for (size_t c = 0; c < 4; ++c)
							sum[c] += color[c];
					}
				const double alpha = sum[3] / (aa * aa);
				if (sum[3] > 0)
					for (size_t c = 0; c < 3; ++c)
						sum[c] /= sum[3];
				sum[3] = alpha;
				Rgba base{};
				if (background == 1)
					base = {solid.Red / 255., solid.Green / 255., solid.Blue / 255., 1};
				else if (background == 2 && bg)
					base = SampleNearest(*bg, (x + .5) / width, (y + .5) / height);
				for (size_t c = 0; c < 3; ++c)
					sum[c] = sum[c] * alpha + base[c] * (background == 2 && bgBlend == 1 ? 1 : 1 - alpha);
				sum[3] = alpha + base[3] * (background == 2 && bgBlend == 1 ? 1 : 1 - alpha);
				if (!WritePixel(*output, x, y, sum))
					return context.Fail(
						Status::InvalidValue, "polygon render generated nonfinite sample", "surface_out"
					);
			}
		return true;
	}
}
