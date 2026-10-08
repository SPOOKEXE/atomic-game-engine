#include "Families.hpp"
#include "Processor.hpp"
#include "Source2DMath.hpp"
#include "SourceCrossSection.hpp"
#include "SourceMarkovGradient.hpp"

#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	bool SourceHerringboneTile(NodeContext &);
	bool SourceHoneycombNoise(NodeContext &);
	bool SourceMirrorPolar(NodeContext &);
	bool SourceSmear(NodeContext &);
	bool SourceCropContent(NodeContext &);
	bool SourceGradientGrid(NodeContext &);
	bool SourceGradientPoints(NodeContext &);
	bool SourceGradientPointsN(NodeContext &);
	bool SourceDrawGradient(NodeContext &);
	bool SourcePadding(NodeContext &context);
	bool SourceNineSlice(NodeContext &context);
	bool PixelDrawSurface(NodeContext &context);
	bool PixelBoxPolar(NodeContext &context);
	bool PixelDrawShape(NodeContext &context);
	bool PixelBuilder(NodeContext &context);
	bool PixelBuilderOutput(NodeContext &context);
	bool PixelExtrude(NodeContext &context);
	bool PixelHighlight(NodeContext &context);
	bool PixelShine(NodeContext &context);
	bool MatrixColorApply(NodeContext &context);
	bool BlendEdge(NodeContext &context);
	bool AnisotropicNoise(NodeContext &context);
	bool DeStray(NodeContext &context);
	bool ColorSelect(NodeContext &context);
	bool RoundCorner(NodeContext &context);
	bool PixelBoxCrop(NodeContext &context);
	bool PixelBoxSurfaceMirror(NodeContext &context);
	bool PixelBox(NodeContext &context);
	bool PixelBoxConvert(NodeContext &context);
	bool PixelBoxMirror(NodeContext &context);
	bool PixelBoxSplit(NodeContext &context);
	bool PixelBoxPoint(NodeContext &context);
	bool PixelBuilderDimension(NodeContext &context);
	bool Scale(NodeContext &context);
	bool Crop(NodeContext &context);
	bool MultiplyAlpha(NodeContext &context);
	bool ColorBlind(NodeContext &context);
	bool ColorRemove(NodeContext &context);
	bool NormalAdjust(NodeContext &context);
	bool HighPass(NodeContext &context);
	bool ExtractChannels(NodeContext &context);
	bool Curvature(NodeContext &context);
	bool Emboss(NodeContext &context);
	bool BitReduce(NodeContext &context);
	bool Checker(NodeContext &context);
	bool InterpretMatrix(NodeContext &context);
	bool BoxPattern(NodeContext &context);
	bool Zigzag(NodeContext &context);
	bool GaussianNoise(NodeContext &context);
	bool FoldNoise(NodeContext &context);
	bool SourceJuliaSet(NodeContext &c);
	bool SourceGaborNoise(NodeContext &c);
	bool SourceFlowNoise(NodeContext &c);
	bool SourceBubbleNoise(NodeContext &c);
	bool SourceCristalNoise(NodeContext &c);
	bool SourceRefract(NodeContext &c);
	bool SourceGradientCube(NodeContext &c);
	bool SourceSimplexNoise(NodeContext &context);
	bool SourceRidgeNoise(NodeContext &context);
	bool DeCorner(NodeContext &context);
	bool ShapeBlur(NodeContext &context);
	bool LinearBrush(NodeContext &context);
	bool Deblur(NodeContext &context);
	bool HeightBlend(NodeContext &context);
	bool WaveInterference(NodeContext &context);
	bool SlopeBlur(NodeContext &context);
	bool MorphSurface(NodeContext &context);
	bool Quasicrystal(NodeContext &context);
	bool Vignette(NodeContext &context);
	bool SymmetricNearest(NodeContext &context);
	using source2d::PixelPosition;
	using source2d::Rotate;
	namespace {
		double UvFract(double coordinate) {
			return coordinate - std::floor(coordinate);
		}

		Vector2 MapUv(const NodeContext &context, Vector2 point, double &alpha) {
			alpha = 1.0;
			const Image *map = context.Input("uv_map");
			if (!map) return point;
			const Rgba sample = SampleNearest(*map, point.X, point.Y);
			const double amount = context.Scalar("uv_mix", 1.0);
			alpha = sample[3];
			return {point.X + (sample[0] - point.X) * amount, point.Y + (1.0 - sample[1] - point.Y) * amount};
		}

		bool RepeatUv(Vector2 &point, int64_t repeat) {
			if (repeat == 0) return point.X >= 0 && point.Y >= 0 && point.X < 1 && point.Y < 1;
			const auto repeatCoordinate = [repeat](double coordinate) {
				if (repeat == 1) return UvFract(coordinate);
				if (repeat == 2) return std::clamp(coordinate, 0.0, 1.0);
				return 1.0 - std::abs(UvFract(coordinate / 2.0) - 0.5) * 2.0;
			};
			point = {repeatCoordinate(point.X), repeatCoordinate(point.Y)};
			return true;
		}

		bool UvGenerator(NodeContext &context) {
			uint32_t width = 0, height = 0;
			const int64_t dimensionUnit = context.Integer("dimension_unit", 1);
			const Image *mask = context.Input("mask");
			if (dimensionUnit == 2) {
				if (!mask)
					return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
				width = mask->Width;
				height = mask->Height;
			} else if (!ResolveDimension(context, "dimension", width, height))
				return false;
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			Image *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			const std::string_view type = context.Authored.Type;
			const bool polar = type == "pc.uv_polar", areaNode = type == "pc.uv_area";
			const bool heightNode = type == "pc.uv_height", isometric = type == "pc.uv_isometric";
			const bool perspective = type == "pc.uv_perspective";
			const Image *heightMap = context.Input("height_map");
			if (heightNode && !heightMap)
				return context.Fail(Status::InvalidValue, "Height Map is required", "height_map");
			const int64_t repeat = context.Integer("repeat", 1), side = context.Integer("direction");
			if (!polar && !heightNode && (repeat < 0 || repeat > 3))
				return context.Fail(Status::InvalidValue, "Repeat choice is invalid", "repeat");
			if ((isometric && (side < 0 || side > 2)) || (perspective && (side < 0 || side > 3)))
				return context.Fail(Status::InvalidValue, "Direction choice is invalid", "direction");
			const Vector2 position = PixelPosition(context, "position", width, height);
			const Vector2 offset = PixelPosition(context, "offset", width, height);
			const Vector2 anchor = context.Vec2("anchor");
			const Vector2 scale = context.Vec2(
				polar					   ? "tiling"
				: isometric || perspective ? "scale"
										   : "tile_scale",
				{1, 1}
			);
			const Vector2 xRange = context.Vec2("x", {0, 1}), yRange = context.Vec2("y", {1, 0});
			const double rotation = context.Scalar("rotation") * std::acos(-1.0) / 180.0;
			const double blue = context.Scalar("blue");
			Area area = context.Get<Area>("area", {});
			if (!context.IsLinked("area") && context.Integer("area_unit", 1) == 1) {
				area.CenterX *= width;
				area.HalfWidth *= width;
				area.CenterY *= height;
				area.HalfHeight *= height;
			}
			for (uint32_t y = 0; y < height; ++y) {
				for (uint32_t x = 0; x < width; ++x) {
					const Vector2 texel{(x + 0.5) / width, (y + 0.5) / height};
					double alpha = 1;
					Vector2 point = MapUv(context, texel, alpha);
					Rgba colour{};
					if (heightNode) {
						const auto sample = [&](double dx, double dy) {
							double ignoredAlpha = 0;
							const Vector2 coordinate = MapUv(
								context,
								{UvFract(texel.X - position.X + dx), UvFract(texel.Y - position.Y + dy)},
								ignoredAlpha
							);
							return SampleNearest(*heightMap, coordinate.X, coordinate.Y);
						};
						const double strength = context.Scalar("strength", 1);
						const double dx = sample(1.0 / width, 0)[0] - sample(-1.0 / width, 0)[0];
						const double dy = sample(0, 1.0 / height)[0] - sample(0, -1.0 / height)[0];
						colour = {
							texel.X + dx * strength, 1.0 - texel.Y + dy * strength, blue, sample(0, 0)[3]
						};
					} else {
						if (areaNode) {
							const Vector2 origin{
								(area.CenterX - area.HalfWidth) / width,
								(area.CenterY - area.HalfHeight) / height
							};
							const Vector2 extent{
								area.HalfWidth * 2.0 / width, area.HalfHeight * 2.0 / height
							};
							if (context.Boolean("invert"))
								point = {point.X * extent.X + origin.X, point.Y * extent.Y + origin.Y};
							else
								point = {(point.X - origin.X) / extent.X, (point.Y - origin.Y) / extent.Y};
						} else if (isometric) {
							point = {point.X - offset.X - anchor.X, point.Y - offset.Y - anchor.Y};
							const double a = 2.0 / std::sqrt(5.0), b = 1.0 / std::sqrt(5.0);
							const double c = side == 0 ? a : 0.0, d = side == 0 ? -b : 1.0;
							const double firstY = side == 2 ? -b : b;
							if (context.Boolean("inverted"))
								point = {point.X * a + point.Y * c, point.X * firstY + point.Y * d};
							else {
								const double determinant = a * d - firstY * c;
								point = {
									(point.X * d - point.Y * c) / determinant,
									(-point.X * firstY + point.Y * a) / determinant
								};
							}
						} else if (perspective)
							point = {point.X - offset.X - anchor.X, point.Y - offset.Y - anchor.Y};
						if (!areaNode && !perspective) point = {point.X - position.X, point.Y - position.Y};
						point = Rotate(point, rotation);
						if (polar) {
							const double angle =
								(std::atan2(point.Y, -point.X) + std::acos(-1.0)) / (2.0 * std::acos(-1.0));
							point = {
								UvFract(angle * scale.X),
								UvFract(std::hypot(point.X, point.Y) / (std::sqrt(2.0) * 0.5) * scale.Y)
							};
						} else {
							if (perspective) {
								const double distance = MappedScalar(context, "distance", texel.X, texel.Y);
								const double depth = side == 0	 ? point.Y * distance
													 : side == 1 ? point.X * distance
													 : side == 2 ? 1.0 - point.Y * distance
																 : 1.0 - point.X * distance;
								const Vector2 vanishing =
									side == 0 || side == 2 ? Vector2{0.5, 0} : Vector2{0, 0.5};
								const Vector2 lens = context.Vec2("perspective", {1, 1});
								point = {
									vanishing.X + (point.X - vanishing.X) * 2.0 / (1.0 + depth * lens.X),
									vanishing.Y + (point.Y - vanishing.Y) * 2.0 / (1.0 + depth * lens.Y)
								};
								point = {point.X - position.X, point.Y - position.Y};
							}
							point = {point.X * scale.X, point.Y * scale.Y};
							if (isometric || perspective)
								point = {point.X + anchor.X, point.Y + anchor.Y};
							else if (!areaNode)
								point = {point.X - anchor.X, point.Y - anchor.Y};
							if (!std::isfinite(point.X) || !std::isfinite(point.Y))
								return context.Fail(Status::InvalidValue, "UV coordinates must be finite");
							if (!RepeatUv(point, repeat)) continue;
						}
						colour = {
							xRange.X + (xRange.Y - xRange.X) * point.X,
							yRange.X + (yRange.Y - yRange.X) * point.Y,
							blue,
							alpha
						};
						if (polar && context.Boolean("invert")) std::swap(colour[0], colour[1]);
					}
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "UV sample exceeds surface range", "surface_out"
						);
					if (mask) {
						colour = ReadPixel(*output, x, y);
						const Rgba maskSample = SampleNearest(*mask, texel.X, texel.Y);
						colour[3] *= (maskSample[0] + maskSample[1] + maskSample[2]) / 3.0 * maskSample[3];
						// Pinned mask_apply_empty stages the masked result through an RGBA8 scratch surface.
						for (double &channel : colour)
							channel = Quantize(channel) / 255.0;
						if (!WritePixel(*output, x, y, colour)) return false;
					}
				}
			}
			return true;
		}

		bool UvBlend(NodeContext &context) {
			const Image *background = context.Input("uv_bg"), *foreground = context.Input("uv_fg"),
						*mask = context.Input("mask");
			if (!background) return context.Fail(Status::InvalidValue, "UV background is required", "uv_bg");
			if (!foreground) return context.Fail(Status::InvalidValue, "UV foreground is required", "uv_fg");
			Image *output = context.NewImage(
				"surface_out", background->Width, background->Height, SurfaceFormat::RGBA8Unorm
			);
			if (!output) return false;
			for (uint32_t y = 0; y < output->Height; ++y) {
				for (uint32_t x = 0; x < output->Width; ++x) {
					const double u = (x + 0.5) / output->Width, v = (y + 0.5) / output->Height;
					const Rgba back = SampleNearest(*background, u, v),
							   front = SampleNearest(*foreground, u, v);
					const double amount =
						context.Scalar("amount", 0.5) * (mask ? SampleNearest(*mask, u, v)[0] : 1.0);
					if (!WritePixel(
							*output,
							x,
							y,
							Rgba{
								back[0] + (front[0] - back[0]) * amount,
								back[1] + (front[1] - back[1]) * amount,
								0,
								1
							}
						))
						return context.Fail(
							Status::InvalidValue, "UV blend exceeds surface range", "surface_out"
						);
				}
			}
			return true;
		}
	}

	bool MkSparkle(NodeContext &context);
	bool SourceNormalize(NodeContext &context);
	bool SourceGrid(NodeContext &context);
	bool SourceGrain(NodeContext &context);
	bool SourceContrastBlur(NodeContext &context);
	bool SourcePathShape(NodeContext &context);
	bool SourceCosGradient(NodeContext &context);
	bool SourceRadialBlur(NodeContext &context);
	bool SourceEdgeDetect(NodeContext &context);
	bool SourceBokehBlur(NodeContext &context);
	bool SourcePathBlur(NodeContext &context);
	bool SourceSimpleBlur(NodeContext &context);
	bool SourceComposeBlend(NodeContext &context);
	bool SourceGapContract(NodeContext &context);
	bool SourceAlignContent(NodeContext &context);
	bool SourceDitherDiffuse(NodeContext &context);
	bool SourceStripe(NodeContext &context);
	bool SourceDotted(NodeContext &context);
	bool SourceOrderedDither(NodeContext &context);
	bool SourceSurfaceReplace(NodeContext &context);
	bool SourceShapeMap(NodeContext &context);
	bool SourceXDoG(NodeContext &c);
	bool SourceKuwahara(NodeContext &c);
	bool SourceBlobify(NodeContext &c);
	std::span<const ExecutorEntry> Source2DExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.xdo_g_threshold", SourceXDoG, true},
			ExecutorEntry{"pc.point_sdf", SourcePointSdf, true},
			ExecutorEntry{"pc.cross_section", DrawSourceCrossSection, true},
			ExecutorEntry{"pc.markov_gradient", DrawSourceMarkovGradient, true},
			ExecutorEntry{"pc.kuwahara", SourceKuwahara, true},
			ExecutorEntry{"pc.blobify", SourceBlobify, true},
			ExecutorEntry{"pc.mirror_polar", SourceMirrorPolar, true},
			ExecutorEntry{"pc.shape_map", SourceShapeMap, true},
			ExecutorEntry{"pc.smear", SourceSmear, true},
			ExecutorEntry{"pc.crop_content", SourceCropContent, true},
			ExecutorEntry{"pc.gradient_grid", SourceGradientGrid, true},
			ExecutorEntry{"pc.gradient_points", SourceGradientPoints, true},
			ExecutorEntry{"pc.gradient_points_n", SourceGradientPointsN, true},
			ExecutorEntry{"pc.gradient", SourceDrawGradient, true},
			ExecutorEntry{"pc.pb_draw_surface", PixelDrawSurface, true},
			ExecutorEntry{"pc.pb_draw_rectangle", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_diamond", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_triangle", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_trapezoid", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_quadrilateral", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_polygon", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_star", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_ellipse", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_pie", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_round_rectangle", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_line", PixelDrawShape, true},
			ExecutorEntry{"pc.pb_draw_curve", PixelDrawShape, true},

			ExecutorEntry{"pc.pixel_builder", PixelBuilder, true},
			ExecutorEntry{"pc.pb_output", PixelBuilderOutput, true},
			ExecutorEntry{"pc.pb_fx_extrude", PixelExtrude, true},
			ExecutorEntry{"pc.pb_fx_highlight", PixelHighlight, true},
			ExecutorEntry{"pc.pb_fx_shine", PixelShine, true},
			ExecutorEntry{"pc.matrix_color_apply", MatrixColorApply, true},
			ExecutorEntry{"pc.blend_edge", BlendEdge, true},
			ExecutorEntry{"pc.noise_aniso", AnisotropicNoise, true},
			ExecutorEntry{"pc.mk_sparkle", MkSparkle, true},
			ExecutorEntry{"pc.normalize", SourceNormalize, true},
			ExecutorEntry{"pc.grid", SourceGrid, true},
			ExecutorEntry{"pc.grain", SourceGrain, true},
			ExecutorEntry{"pc.blur_contrast", SourceContrastBlur, true},
			ExecutorEntry{"pc.path_shape", SourcePathShape, true},
			ExecutorEntry{"pc.gradient_cos", SourceCosGradient, true},
			ExecutorEntry{"pc.blur_radial", SourceRadialBlur, true},
			ExecutorEntry{"pc.edge_detect", SourceEdgeDetect, true},
			ExecutorEntry{"pc.blur_bokeh", SourceBokehBlur, true},
			ExecutorEntry{"pc.blur_path", SourcePathBlur, true},
			ExecutorEntry{"pc.blur_simple", SourceSimpleBlur, true},
			ExecutorEntry{"pc.blend", SourceComposeBlend, true},
			ExecutorEntry{"pc.gap_contract", SourceGapContract, true},
			ExecutorEntry{"pc.align_content", SourceAlignContent, true},
			ExecutorEntry{"pc.dither_diffuse", SourceDitherDiffuse, true},
			ExecutorEntry{"pc.dither", SourceOrderedDither, true},
			ExecutorEntry{"pc.stripe", SourceStripe, true},
			ExecutorEntry{"pc.dotted", SourceDotted, true},
			ExecutorEntry{"pc.surface_replace", SourceSurfaceReplace, true},
			ExecutorEntry{"pc.padding", SourcePadding, true},
			ExecutorEntry{"pc.9_slice", SourceNineSlice, true},
			ExecutorEntry{"pc.de_stray", DeStray, true},
			ExecutorEntry{"pc.color_select", ColorSelect, true},
			ExecutorEntry{"pc.corner", RoundCorner, true},
			ExecutorEntry{"pc.pb_crop_pbbox", PixelBoxCrop, true},
			ExecutorEntry{"pc.pb_filter_mirror", PixelBoxSurfaceMirror, true},
			ExecutorEntry{"pc.pb_filter_polar", PixelBoxPolar, true},
			ExecutorEntry{"pc.pb_box", PixelBox, true},
			ExecutorEntry{"pc.pb_box_bbox", PixelBoxConvert, true},
			ExecutorEntry{"pc.pb_box_mirror", PixelBoxMirror, true},
			ExecutorEntry{"pc.pb_box_split", PixelBoxSplit, true},
			ExecutorEntry{"pc.pb_box_point", PixelBoxPoint, true},
			ExecutorEntry{"pc.pb_dimension", PixelBuilderDimension, true},
			ExecutorEntry{"pc.uv_area", UvGenerator, true},
			ExecutorEntry{"pc.uv_cartesian", UvGenerator, true},
			ExecutorEntry{"pc.uv_height", UvGenerator, true},
			ExecutorEntry{"pc.uv_isometric", UvGenerator, true},
			ExecutorEntry{"pc.uv_perspective", UvGenerator, true},
			ExecutorEntry{"pc.uv_polar", UvGenerator, true},
			ExecutorEntry{"pc.uv_blend", UvBlend, true},
			ExecutorEntry{"pc.color_remove", ColorRemove, true},
			ExecutorEntry{"pc.normal_adjust", NormalAdjust, true},
			ExecutorEntry{"pc.high_pass", HighPass, true},
			ExecutorEntry{"pc.crop", Crop, true},
			ExecutorEntry{"pc.scale", Scale, true},
			ExecutorEntry{"pc.multiply_alpha", MultiplyAlpha, true},
			ExecutorEntry{"pc.color_blind", ColorBlind, true},
			ExecutorEntry{"pc.rgb_channel", ExtractChannels, true},
			ExecutorEntry{"pc.hsv_channel", ExtractChannels, true},
			ExecutorEntry{"pc.curvature", Curvature, true},
			ExecutorEntry{"pc.emboss", Emboss, true},
			ExecutorEntry{"pc.bit_reduce", BitReduce, true},
			ExecutorEntry{"pc.checker", Checker, true},
			ExecutorEntry{"pc.interpret_matrix", InterpretMatrix, true},
			ExecutorEntry{"pc.box_pattern", BoxPattern, true},
			ExecutorEntry{"pc.zigzag", Zigzag, true},
			ExecutorEntry{"pc.noise_gaussian", GaussianNoise, true},
			ExecutorEntry{"pc.fold_noise", FoldNoise, true},
			ExecutorEntry{"pc.julia_set", SourceJuliaSet, true},
			ExecutorEntry{"pc.gabor_noise", SourceGaborNoise, true},
			ExecutorEntry{"pc.flow_noise", SourceFlowNoise, true},
			ExecutorEntry{"pc.noise_bubble", SourceBubbleNoise, true},
			ExecutorEntry{"pc.noise_cristal", SourceCristalNoise, true},
			ExecutorEntry{"pc.refract", SourceRefract, true},
			ExecutorEntry{"pc.gradient_cube", SourceGradientCube, true},
			ExecutorEntry{"pc.noise_simplex", SourceSimplexNoise, true},
			ExecutorEntry{"pc.ridge_noise", SourceRidgeNoise, true},
			ExecutorEntry{"pc.de_corner", DeCorner, true},
			ExecutorEntry{"pc.blur_shape", ShapeBlur, true},
			ExecutorEntry{"pc.brush_linear", LinearBrush, true},
			ExecutorEntry{"pc.deblur", Deblur, true},
			ExecutorEntry{"pc.blend_height", HeightBlend, true},
			ExecutorEntry{"pc.wave_interfere", WaveInterference, true},
			ExecutorEntry{"pc.blur_slope", SlopeBlur, true},
			ExecutorEntry{"pc.morph_surface", MorphSurface, true},
			ExecutorEntry{"pc.quasicrystal", Quasicrystal, true},
			ExecutorEntry{"pc.vignette", Vignette, true},
			ExecutorEntry{"pc.symmetric_nn", SymmetricNearest, true},
			ExecutorEntry{"pc.herringbone_tile", SourceHerringboneTile, true},
			ExecutorEntry{"pc.honeycomb_noise", SourceHoneycombNoise, true},
		};
		return ENTRIES;
	}
}
