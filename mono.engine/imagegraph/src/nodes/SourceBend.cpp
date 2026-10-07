#include "SourceBend.hpp"

#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "SourceRefractClean.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t MaximumBendWork = 64000000;
		struct BendVertex {
			Vector2 Position, UV;
		};
		struct BendGeometry {
			std::optional<AllocationReservation> Charge;
			std::vector<BendVertex> Vertices;
			const Image *Source = nullptr;
			uint32_t Columns = 0, Rows = 0, Width = 0, Height = 0;
			Vector2 UVScale{1, 1}, UVShift{};
			SamplerSettings Sampler;
			bool Inactive = false;
		};
		struct BendTriangle {
			BendVertex A, B, C;
			double Area = 0;
			uint32_t Left = 0, Top = 0, Right = 0, Bottom = 0;
		};
		double Cross(Vector2 a, Vector2 b, Vector2 p) {
			return (b.X - a.X) * (p.Y - a.Y) - (b.Y - a.Y) * (p.X - a.X);
		}
		bool Included(Vector2 a, Vector2 b) {
			return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
		}
		double BendRoundEven(double value) {
			const double lower = std::floor(value), fraction = value - lower;
			return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0));
		}
		bool BendSize(NodeContext &context, double value, uint32_t &result) {
			if (!std::isfinite(value))
				return context.Fail(Status::InvalidValue, "Bend dimension is nonfinite", "dimension");
			const double size = std::clamp(BendRoundEven(value), 1., 16384.);
			if (size > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "Bend dimensions exceed native limits", "dimension"
				);
			result = uint32_t(size);
			return true;
		}
		// grug build the source grid, then apply its bounds, fit and centred scale in that order.
		bool PrepareBend(NodeContext &context, BendGeometry &geometry) {
			ENGINE_PROFILE("imagegraph.source.bend.prepare");
			geometry.Source = context.Input("surface_in");
			if (!geometry.Source ||
				!ValidSurfaceLayout(
					*geometry.Source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes
				))
				return context.Fail(Status::InvalidValue, "Bend requires a valid Surface In", "surface_in");
			geometry.Inactive = !context.Boolean("active", true);
			if (context.FailureCode != Status::Ok) return false;
			if (const Value *input = context.Find("surface_in")) {
				if (const auto *atlas = std::get_if<AtlasValue>(input)) {
					if (!ValidAtlasPayload(*atlas))
						return context.Fail(
							Status::InvalidValue, "Bend Atlas payload is invalid", "surface_in"
						);
					if (!geometry.Inactive)
						return context.Fail(
							Status::UnsupportedExecution,
							"Bend raw vertex texture binding rejects Atlas",
							"surface_in"
						);
				}
			}
			if (geometry.Inactive) return true;
			const Image &source = *geometry.Source;
			const int64_t type = context.Integer("type"),
						  dimensionType = context.Integer("dimension_type", 1);
			const double axis = context.SourceChoice("axis"), amount = context.Scalar("amount", .25);
			const double frequency = context.Scalar("scale", 1), shift = context.Scalar("shift");
			geometry.UVScale = context.Vec2("uv_scale", {1, 1});
			geometry.UVShift = context.Vec2("uv_shift");
			Vector2 scale = context.Vec2("scale_2", {1, 1});
			geometry.Sampler = ReadSampler(context);
			if (context.FailureCode != Status::Ok) return false;
			if (type < 0 || type > 1 || dimensionType < 0 || dimensionType > 1 ||
				(type == 0 && amount != 0 && axis != 0 && axis != 1))
				return context.Fail(
					Status::UnsupportedExecution, "Bend source switch has no defined geometry", "type"
				);
			for (const double value :
				 {amount,
				  frequency,
				  shift,
				  scale.X,
				  scale.Y,
				  geometry.UVScale.X,
				  geometry.UVScale.Y,
				  geometry.UVShift.X,
				  geometry.UVShift.Y})
				if (!std::isfinite(value))
					return context.Fail(Status::InvalidValue, "Bend control is nonfinite", "amount");
			for (const double value :
				 {geometry.UVScale.X, geometry.UVScale.Y, geometry.UVShift.X, geometry.UVShift.Y})
				if (std::abs(value) > std::numeric_limits<float>::max())
					return context.Fail(
						Status::UnsupportedExecution,
						"Bend UV control exceeds finite shader uniforms",
						"uv_scale"
					);
			geometry.Columns = std::min(axis == 0 ? 64u : 16u, source.Width / 2);
			geometry.Rows = std::min(axis == 0 ? 16u : 64u, source.Height / 2);
			if (!geometry.Columns || !geometry.Rows)
				return context.Fail(
					Status::UnsupportedExecution, "Bend source grid is empty below two pixels", "surface_in"
				);
			const size_t count = size_t(geometry.Columns + 1) * (geometry.Rows + 1);
			geometry.Charge = context.ReserveWorkspace(count * sizeof(BendVertex), "surface_out");
			if (!geometry.Charge) return false;
			geometry.Vertices.resize(count);
			double radius = 0, centerX = 0, centerY = 0, start = 0, finish = 0;
			if (type == 0 && amount != 0) {
				const double angle = std::abs(amount) * 90;
				const double relative = 1 / (2 * std::tan(angle * (std::numbers::pi / 180)));
				radius = std::sqrt(relative * relative + .25);
				if (axis == 0) {
					centerX = .5;
					centerY = amount > 0 ? 1 + relative : -relative;
					start = amount > 0 ? 90 + angle : -90 - angle;
					finish = amount > 0 ? 90 - angle : -90 + angle;
				} else {
					centerY = .5;
					centerX = amount > 0 ? -relative : 1 + relative;
					start = amount > 0 ? angle : 180 - angle;
					finish = amount > 0 ? -angle : 180 + angle;
				}
			}
			Vector2 minimum{INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY};
			for (uint32_t x = 0; x <= geometry.Columns; ++x)
				for (uint32_t y = 0; y <= geometry.Rows; ++y) {
					auto &vertex = geometry.Vertices[size_t(x) * (geometry.Rows + 1) + y];
					vertex.UV = {double(x) / geometry.Columns, double(y) / geometry.Rows};
					vertex.Position = {vertex.UV.X * source.Width, vertex.UV.Y * source.Height};
					if (type == 0 && amount != 0) {
						const double u = axis == 0 ? vertex.UV.X : vertex.UV.Y;
						const double v = axis == 0 ? vertex.UV.Y : 1 - vertex.UV.X;
						const double r = radius + (amount > 0 ? 1 - v : v);
						const double angle = std::lerp(start, finish, u) * (std::numbers::pi / 180);
						// grug keep the source's unscaled centre offsets and negative lengthdir_y.
						vertex.Position = {
							centerX + std::cos(angle) * r * source.Width,
							centerY - std::sin(angle) * r * source.Height
						};
					} else if (type == 1) {
						const double u = axis == 0 ? vertex.UV.Y : vertex.UV.X;
						const double displacement =
							std::sin(u * std::numbers::pi * frequency - shift * std::numbers::pi * 2) *
							amount * (axis == 0 ? source.Height : source.Width) / 2;
						if (axis == 0)
							vertex.Position.X += displacement;
						else
							vertex.Position.Y += displacement;
					}
					if (!std::isfinite(vertex.Position.X) || !std::isfinite(vertex.Position.Y))
						return context.Fail(Status::InvalidValue, "Bend vertex is nonfinite", "amount");
					minimum = {
						std::min(minimum.X, vertex.Position.X), std::min(minimum.Y, vertex.Position.Y)
					};
					maximum = {
						std::max(maximum.X, vertex.Position.X), std::max(maximum.Y, vertex.Position.Y)
					};
				}
			const Vector2 bounds{maximum.X - minimum.X, maximum.Y - minimum.Y};
			if (!(bounds.X > 0) || !(bounds.Y > 0) || !std::isfinite(bounds.X) || !std::isfinite(bounds.Y))
				return context.Fail(Status::InvalidValue, "Bend geometry bounds are undefined", "amount");
			Vector2 dimension = bounds, fit{1, 1};
			if (dimensionType == 0) {
				dimension = context.Vec2("dimension", {1, 1});
				const int64_t unit = context.Integer("dimension_unit");
				if (unit != 0 && unit != 1)
					return context.Fail(
						Status::UnsupportedExecution,
						"Bend dimension unit has no defined reference",
						"dimension_unit"
					);
				if (unit == 1 && !context.IsLinked("dimension")) {
					dimension.X *= context.Project.SurfaceWidth;
					dimension.Y *= context.Project.SurfaceHeight;
				}
				fit = {dimension.X / bounds.X, dimension.Y / bounds.Y};
				if (context.Boolean("keep_ratio", true)) {
					if (fit.X < fit.Y)
						scale.Y *= fit.X / fit.Y;
					else
						scale.X *= fit.Y / fit.X;
				}
			}
			if (context.FailureCode != Status::Ok || !BendSize(context, dimension.X, geometry.Width) ||
				!BendSize(context, dimension.Y, geometry.Height))
				return false;
			const Vector2 offset{dimension.X / 2 * (1 - scale.X), dimension.Y / 2 * (1 - scale.Y)};
			for (auto &vertex : geometry.Vertices) {
				vertex.Position = {
					(vertex.Position.X - minimum.X) * fit.X * scale.X + offset.X,
					(vertex.Position.Y - minimum.Y) * fit.Y * scale.Y + offset.Y
				};
				if (!std::isfinite(vertex.Position.X) || !std::isfinite(vertex.Position.Y))
					return context.Fail(
						Status::InvalidValue, "Bend transformed vertex is nonfinite", "scale_2"
					);
			}
			return true;
		}
		bool Triangle(
			NodeContext &context,
			const BendGeometry &geometry,
			BendVertex a,
			BendVertex b,
			BendVertex c,
			BendTriangle &triangle
		) {
			triangle = {a, b, c, Cross(a.Position, b.Position, c.Position)};
			if (!std::isfinite(triangle.Area))
				return context.Fail(Status::InvalidValue, "Bend triangle area is nonfinite", "scale_2");
			if (triangle.Area < 0) {
				std::swap(triangle.B, triangle.C);
				triangle.Area = -triangle.Area;
			}
			triangle.Left = uint32_t(
				std::clamp(
					std::floor(std::min({a.Position.X, b.Position.X, c.Position.X})),
					0.,
					double(geometry.Width)
				)
			);
			triangle.Right = uint32_t(
				std::clamp(
					std::ceil(std::max({a.Position.X, b.Position.X, c.Position.X})),
					0.,
					double(geometry.Width)
				)
			);
			triangle.Top = uint32_t(
				std::clamp(
					std::floor(std::min({a.Position.Y, b.Position.Y, c.Position.Y})),
					0.,
					double(geometry.Height)
				)
			);
			triangle.Bottom = uint32_t(
				std::clamp(
					std::ceil(std::max({a.Position.Y, b.Position.Y, c.Position.Y})),
					0.,
					double(geometry.Height)
				)
			);
			return true;
		}
		template <class Visitor>
		bool VisitTriangles(NodeContext &context, const BendGeometry &geometry, Visitor visitor) {
			for (uint32_t x = 0; x < geometry.Columns; ++x)
				for (uint32_t y = 0; y < geometry.Rows; ++y) {
					const size_t a = size_t(x) * (geometry.Rows + 1) + y, b = a + geometry.Rows + 1;
					for (const auto indices :
						 {std::array<size_t, 3>{a, b, a + 1}, std::array<size_t, 3>{b, a + 1, b + 1}}) {
						BendTriangle triangle;
						if (!Triangle(
								context,
								geometry,
								geometry.Vertices[indices[0]],
								geometry.Vertices[indices[1]],
								geometry.Vertices[indices[2]],
								triangle
							) ||
							!visitor(triangle))
							return false;
					}
				}
			return true;
		}
		bool QuoteBend(NodeContext &context, const BendGeometry &geometry, uint64_t &work) {
			const auto spend = [&](uint64_t amount) {
				if (work > MaximumBendWork || amount > MaximumBendWork - work)
					return context.Fail(
						Status::LimitExceeded, "Bend complete batch exceeds work limit", "surface_out"
					);
				work += amount;
				return true;
			};
			if (geometry.Inactive) return spend(uint64_t(geometry.Source->Width) * geometry.Source->Height);
			if (!spend(uint64_t(geometry.Width) * geometry.Height + geometry.Vertices.size() * 64))
				return false;
			const uint64_t sampleCost = geometry.Sampler.Interpolation == 6	  ? 4096
										: geometry.Sampler.Interpolation == 4 ? 256
																			  : 64;
			return VisitTriangles(context, geometry, [&](const BendTriangle &triangle) {
				return triangle.Area == 0 || spend(
												 uint64_t(triangle.Right - triangle.Left) *
												 (triangle.Bottom - triangle.Top) * (1 + sampleCost)
											 );
			});
		}
		bool DrawBend(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.bend");
			BendGeometry geometry;
			uint64_t work = 0;
			if (!PrepareBend(context, geometry) || !QuoteBend(context, geometry, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			// grug source surface_verify has default RGBA8, even when Color Depth asks otherwise.
			Image *output =
				context.NewImage("surface_out", geometry.Width, geometry.Height, SurfaceFormat::RGBA8Unorm);
			if (!output) return false;
			return VisitTriangles(context, geometry, [&](const BendTriangle &triangle) {
				if (triangle.Area == 0) return true;
				for (uint32_t y = triangle.Top; y < triangle.Bottom; ++y)
					for (uint32_t x = triangle.Left; x < triangle.Right; ++x) {
						const auto &a = triangle.A, &b = triangle.B, &c = triangle.C;
						const Vector2 point{double(x) + .5, double(y) + .5};
						const double wa = Cross(b.Position, c.Position, point),
									 wb = Cross(c.Position, a.Position, point),
									 wc = Cross(a.Position, b.Position, point);
						if (wa < 0 || (wa == 0 && !Included(b.Position, c.Position)) || wb < 0 ||
							(wb == 0 && !Included(c.Position, a.Position)) || wc < 0 ||
							(wc == 0 && !Included(a.Position, b.Position)))
							continue;
						const Vector2 uv{
							((a.UV.X * wa + b.UV.X * wb + c.UV.X * wc) / triangle.Area) * geometry.UVScale.X -
								geometry.UVShift.X,
							((a.UV.Y * wa + b.UV.Y * wb + c.UV.Y * wc) / triangle.Area) * geometry.UVScale.Y -
								geometry.UVShift.Y
						};
						if (!std::isfinite(uv.X) || !std::isfinite(uv.Y))
							return context.Fail(
								Status::InvalidValue, "Bend UV coordinate is nonfinite", "uv_scale"
							);
						const auto colour = source_refract_clean::Sample(
							*geometry.Source,
							uv.X,
							uv.Y,
							geometry.Sampler,
							{double(geometry.Width), double(geometry.Height)}
						);
						const auto previous = ReadPixel(*output, x, y);
						Rgba blended{
							colour[0] + previous[0] * (1 - colour[3]),
							colour[1] + previous[1] * (1 - colour[3]),
							colour[2] + previous[2] * (1 - colour[3]),
							colour[3] + previous[3]
						};
						if (!WritePixel(*output, x, y, blended))
							return context.Fail(
								Status::InvalidValue,
								"Bend sample exceeds finite surface range",
								"surface_out"
							);
					}
				return true;
			});
		}
	}
	bool AdmitSourceBend(NodeContext &context, uint64_t &batchWork) {
		BendGeometry geometry;
		return PrepareBend(context, geometry) && QuoteBend(context, geometry, batchWork);
	}
	std::span<const ExecutorEntry> SourceBendExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.bend", DrawBend, true}};
		return entries;
	}
}
