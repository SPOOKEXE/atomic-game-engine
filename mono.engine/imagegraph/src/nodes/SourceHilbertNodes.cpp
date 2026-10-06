// Source Hilbert point ordering with the shared pixel-centre CPU line coverage profile.
#include "Families.hpp"
#include "Source2DGenerator.hpp"
#include "SourcePlotRaster.hpp"

#include <array>

namespace engine::imagegraph::detail {
	bool SourceKisrhombille(NodeContext &);
	namespace {
		struct HilbertGroup {
			std::array<Vector2, 4> Points;
			int Orientation;
			bool Flip;
		};
		HilbertGroup HilbertPoints(Vector2 center, double halfWidth, int orientation, bool flip) {
			// The pinned source uses halfWidth for both coordinates, including nonsquare surfaces.
			const Vector2 p00{center.X - halfWidth, center.Y - halfWidth};
			const Vector2 p01{center.X - halfWidth, center.Y + halfWidth};
			const Vector2 p10{center.X + halfWidth, center.Y - halfWidth};
			const Vector2 p11{center.X + halfWidth, center.Y + halfWidth};
			switch (orientation) {
			case 0:
				return {{p01, p11, p10, p00}, orientation, flip};
			case 1:
				return {{p01, p00, p10, p11}, orientation, flip};
			case 2:
				return {{p10, p00, p01, p11}, orientation, flip};
			default:
				return {{p10, p11, p01, p00}, orientation, flip};
			}
		}
		struct HilbertDraw {
			NodeContext &Context;
			SourcePlotRaster Raster;
			const Gradient &Colours;
			double Shift;
			double Thickness;
			uint64_t PointCount;
			uint64_t PointIndex = 0;
			Vector2 Previous;
			bool HasPrevious = false;
			bool Visit(const HilbertGroup &group, double halfWidth, int remaining) {
				if (remaining == 0) {
					for (Vector2 point : group.Points) {
						const double progress =
							ShaderFract(ShaderFract(double(PointIndex++) / double(PointCount) + Shift) + 1);
						const auto colour = SourceCachedGradient(Colours, progress);
						if (!colour)
							return Context.Fail(
								Status::InvalidValue, "Hilbert gradient is nonfinite", "path_color"
							);
						const Rgba rgb{colour->Red / 255., colour->Green / 255., colour->Blue / 255., 1};
						if (HasPrevious && !Raster.Line(Previous, point, Thickness, rgb, rgb, false))
							return false;
						HasPrevious = true;
						Previous = point;
					}
					return true;
				}
				const int first = (group.Orientation + 3 - int(group.Flip) * 2) % 4;
				const int last = (group.Orientation + 1 + int(group.Flip) * 2) % 4;
				const std::array<int, 4> orientations{first, group.Orientation, group.Orientation, last};
				for (size_t index = 0; index < 4; ++index) {
					const bool flip = index == 0 || index == 3 ? !group.Flip : group.Flip;
					if (!Visit(
							HilbertPoints(group.Points[index], halfWidth / 2, orientations[index], flip),
							halfWidth / 2,
							remaining - 1
						))
						return false;
				}
				return true;
			}
		};
		bool Hilbert(NodeContext &context) {
			if (context.Request.RequireSourceGpuRasterCoverage)
				return context.Fail(
					Status::UnsupportedExecution,
					"Hilbert builtin lines require captured source GPU coverage for this request"
				);
			const int64_t iteration = std::max(int64_t{1}, context.Integer("iteration", 2));
			constexpr int64_t MAXIMUM_ITERATION = 10; // At most 1,048,576 points and ten stack frames.
			if (iteration > MAXIMUM_ITERATION)
				return context.Fail(Status::LimitExceeded, "Hilbert exceeds its point budget", "iteration");
			const int64_t orientation = context.Integer("orientation", 1);
			if (orientation < 0 || orientation > 3)
				return context.Fail(Status::InvalidValue, "Hilbert orientation is invalid", "orientation");
			const double thickness = context.Scalar("thickness", 2), shift = context.Scalar("shift");
			if (!std::isfinite(thickness) || !std::isfinite(shift))
				return context.Fail(Status::InvalidValue, "Hilbert rendering controls must be finite");
			if (std::abs(thickness) > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "Hilbert thickness exceeds raster limits", "thickness"
				);
			uint32_t width = 0, height = 0;
			if (!ResolveDimension(context, "dimension", width, height)) return false;
			Vector2 dimension = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			const auto *value = context.Find("path_color");
			const auto *gradient = value ? std::get_if<Gradient>(value) : nullptr;
			if (!gradient)
				return context.Fail(Status::TypeMismatch, "Hilbert requires a path gradient", "path_color");
			Image *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			const Rgba background = source2d::InputColour(context, "bg_color", {0, 0, 0, 255});
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					if (!WritePixel(*output, x, y, background))
						return context.Fail(
							Status::InvalidValue, "Hilbert background exceeds surface range", "bg_color"
						);
			const auto initial = HilbertPoints(
				{dimension.X / 2, dimension.Y / 2}, dimension.X / 4, int(orientation), orientation % 2 == 0
			);
			// Depth-first emission preserves the source array expansion order without allocating intermediate
			// arrays.
			HilbertDraw draw{
				context,
				{context, *output},
				*gradient,
				shift,
				thickness,
				uint64_t{1} << (iteration * 2),
				0,
				initial.Points[0]
			};
			return draw.Visit(initial, dimension.X / 4, int(iteration - 1));
		}
	}
	std::span<const ExecutorEntry> SourceHilbertExecutors() {
		static const std::array entries{
			ExecutorEntry{"pc.hilbert", Hilbert, true},
			ExecutorEntry{"pc.kisrhombille", SourceKisrhombille, true}
		};
		return entries;
	}
}
