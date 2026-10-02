#include "Gradient.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"

namespace engine::imagegraph::detail {
	bool SourceGrid(NodeContext &context) {
		const int64_t mode = context.Integer("render_type"), axis = context.Integer("shift_axis");
		const Image *texture = context.Input("texture"), *mask = context.Input("mask");
		if ((mode == 3 || mode == 4) && !texture)
			return context.Fail(Status::InvalidValue, "Grid texture mode requires a texture", "texture");
		uint32_t width = 0, height = 0;
		if ((mode == 3 || mode == 4) && context.Boolean("use_texture_dimension")) {
			width = texture->Width;
			height = texture->Height;
		} else if (context.Integer("dimension_unit", 1) == 2) {
			if (!mask)
				return context.Fail(Status::InvalidValue, "Grid Mask dimensions require a mask", "mask");
			width = mask->Width;
			height = mask->Height;
		} else if (!ResolveDimension(context, "dimension", width, height))
			return false;
		Vector2 dimension{double(width), double(height)};
		if (!((mode == 3 || mode == 4) && context.Boolean("use_texture_dimension")) &&
			context.Integer("dimension_unit", 1) != 2) {
			dimension = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
		}
		if (dimension.X == 0 || dimension.Y == 0)
			return context.Fail(
				Status::UnsupportedExecution, "Grid source dimension division is undefined", "dimension"
			);
		const auto format = ResolveProcessorSurfaceFormat(context, texture);
		if (!format) return false;
		const bool safeGray = texture && (texture->Format == SurfaceFormat::R8Unorm ||
										  texture->Format == SurfaceFormat::R16Float ||
										  texture->Format == SurfaceFormat::R32Float);
		// __channel_pre replaces sh_grid; only hardware filtering and the later main-output mask survive.
		if (safeGray) {
			if (!context.SetOutputDiagnostic(
					"heightmap",
					Status::UnsupportedExecution,
					"Grid single-channel safe draw replaces its MRT shader and leaves Heightmap unwritten"
				))
				return false;
			Image *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			const auto sampler = ReadSampler(context);
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const double u = (x + .5) / width, v = (y + .5) / height;
					const double red =
						(Filtered(sampler) ? BilinearClamp(*texture, u, v)
										   : SampleNearest(*texture, u, v))[0];
					Rgba colour{red, red, red, 1};
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(Status::InvalidValue, "Grid safe draw exceeds surface range");
					if (mask) {
						colour = ReadPixel(*output, x, y);
						const Rgba sample = SampleNearest(*mask, u, v);
						colour[3] *= (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
						for (double &channel : colour)
							channel = Quantize(channel) / 255.;
						if (!WritePixel(*output, x, y, colour))
							return context.Fail(Status::InvalidValue, "Grid safe mask exceeds surface range");
					}
				}
			return context.FailureCode == Status::Ok;
		}
		if (!context.Find("seed"))
			return context.Fail(Status::InvalidValue, "Grid requires its resolved source seed", "seed");
		const auto *keysValue = context.Find("tile_color");
		const auto *keys = keysValue ? std::get_if<Gradient>(keysValue) : nullptr;
		if (!keys || keys->Keys.empty() || keys->Keys.size() > GRADIENT_KEY_SLOTS)
			return context.Fail(Status::InvalidValue, "Grid requires a bounded tile gradient", "tile_color");
		const auto gradient = ReadGradient(context, "tile_color", *keys);
		if ((context.Scalar("random_shift") != 0 && !context.Find("shift_seed")) ||
			(context.Scalar("random_scale") != 0 && !context.Find("scale_seed")) ||
			(mode == 3 && context.Boolean("truchet") && !context.Find("texture_seed")) ||
			(mode != 1 && mode != 2 && (keys->Keys.size() > 1 || gradient.Map) &&
			 !context.Find("color_seed")))
			return context.Fail(
				Status::InvalidValue, "Grid requires resolved source seeds for its enabled random controls"
			);

		const double seed = context.Scalar("seed");
		const auto random = [&](Vector2 point) {
			const double amplitude = 43758.5453123 + seed;
			return ShaderFract(
				std::sin((point.X + 85.456034) * 12.9898 + (point.Y + 64.54065) * 78.233) *
				(amplitude - std::floor(amplitude / 100000) * 100000) / 10
			);
		};
		const auto mix = [](double first, double last, double amount) {
			return first + (last - first) * amount;
		};
		const Vector2 levelIn = context.Vec2("level_in", {0, 1}),
					  levelOut = context.Vec2("level_out", {0, 1});
		const bool undefinedHeight = mode != 1 && levelIn.X == levelIn.Y;
		if (undefinedHeight && mode == 2)
			return context.SetOutputDiagnostic(
					   "surface_out",
					   Status::UnsupportedExecution,
					   "Grid source Height Map level division is undefined"
				   ) &&
				   context.SetOutputDiagnostic(
					   "heightmap",
					   Status::UnsupportedExecution,
					   "Grid source Heightmap level division is undefined"
				   );
		Vector2 scaleValue = context.Vec2("grid_size", {.25, .25});
		if (!context.IsLinked("grid_size") && context.Integer("grid_size_unit", 1) == 1) {
			scaleValue.X *= dimension.X;
			scaleValue.Y *= dimension.Y;
		}
		Vector2 position = context.Vec2("position");
		if (!context.IsLinked("position") && context.Integer("position_unit", 1) == 1) {
			position.X *= dimension.X;
			position.Y *= dimension.Y;
		}
		position = {position.X / dimension.X, position.Y / dimension.Y};
		const Vector4 texturePosition = context.Get<Vector4>("random_position", {}),
					  textureScale = context.Get<Vector4>("random_scale_2", {1, 1, 1, 1});
		const Vector2 textureAngle = context.Vec2("random_angle");
		const auto sampler = ReadSampler(context);
		const Rgba gapColour = source2d::InputColour(context, "gap_color", {0, 0, 0, 255});
		Image *output = context.NewImage("surface_out", width, height, *format);
		Image *heightmap =
			mode == 1 || undefinedHeight ? nullptr : context.NewImage("heightmap", width, height, *format);
		if (mode == 1 &&
			!context.SetOutputDiagnostic(
				"heightmap",
				Status::UnsupportedExecution,
				"Accurate Grid source leaves Heightmap MRT unwritten; source renderer observation is required"
			))
			return false;
		if (undefinedHeight &&
			!context.SetOutputDiagnostic(
				"heightmap", Status::UnsupportedExecution, "Grid source Heightmap level division is undefined"
			))
			return false;
		if (!output || (mode != 1 && !undefinedHeight && !heightmap)) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha, Filtered(sampler));
				Vector2 scale = scaleValue;
				if (context.Input("grid_size_map") && context.Boolean("grid_size_mapped"))
					return context.Fail(
						Status::UnsupportedExecution,
						"Grid vec2 mapping needs its source uniform upload observation",
						"grid_size_map"
					);
				if (context.Boolean("invert_size")) {
					scale.X = dimension.X / scale.X;
					scale.Y = dimension.Y / scale.Y;
				}
				const double aspect = dimension.X / dimension.Y;
				scale.X *= aspect;
				if (scale.X == 0 || scale.Y == 0)
					return context.Fail(
						Status::UnsupportedExecution, "Grid source scale division is undefined", "grid_size"
					);
				if (mode == 1) {
					scale = {std::floor(scale.X), std::floor(scale.Y)};
					if (scale.X == 0 || scale.Y == 0)
						return context.Fail(
							Status::UnsupportedExecution,
							"Accurate Grid source divides by a zero rounded cell size",
							"grid_size"
						);
					const Vector2 pixel{
						std::floor((uv.X - position.X) * aspect * dimension.X),
						std::floor((uv.Y - position.Y) * dimension.Y)
					};
					Vector2 coordinate =
						context.Boolean("diagonal") ? Vector2{pixel.X + pixel.Y, -pixel.X + pixel.Y} : pixel;
					const Vector2 remainder{
						coordinate.X - std::floor(coordinate.X / scale.X) * scale.X,
						coordinate.Y - std::floor(coordinate.Y / scale.Y) * scale.Y
					};
					const double gap = context.Scalar("gap_width", 1) + 1;
					Rgba colour =
						remainder.X > scale.X - gap || remainder.Y > scale.Y - gap
							? gapColour
							: GradientEval(
								  gradient, random({coordinate.X - remainder.X, coordinate.Y - remainder.Y})
							  );
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(Status::InvalidValue, "Accurate Grid colour is nonfinite");
					if (mask) {
						colour = ReadPixel(*output, x, y);
						const Rgba sample = SampleNearest(*mask, u, v);
						colour[3] *= ((sample[0] + sample[1] + sample[2]) / 3) * sample[3];
						for (double &channel : colour)
							channel = Quantize(channel) / 255.;
					}
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(Status::InvalidValue, "Accurate Grid colour is nonfinite");
					continue;
				}
				scale = {dimension.X / scale.X, dimension.Y / scale.Y};
				Vector2 point = source2d::Rotate(
					{(uv.X - position.X) * aspect, uv.Y - position.Y},
					MappedScalar(context, "angle", u, v, Filtered(sampler)) * std::acos(-1.0) / 180
				);
				const double shift =
					MappedScalar(context, "shift", u, v, Filtered(sampler)) / (axis == 0 ? scale.X : scale.Y);
				const double cell = std::floor(axis == 0 ? point.Y * scale.Y : point.X * scale.X),
							 secondary = cell - std::floor(cell / 2) * 2;
				const double shiftCoordinate = context.Scalar("shift_seed") / 1000 + cell / dimension.X;
				const double scaleCoordinate = context.Scalar("scale_seed") / 1000 + cell / dimension.X;
				const double offset =
					secondary * context.Scalar("secondary_shift") + cell * shift +
					context.Scalar("random_shift") * (random({shiftCoordinate, shiftCoordinate}) * 2 - 1);
				const double factor =
					secondary * context.Scalar("secondary_scale") + 1 +
					context.Scalar("random_scale") * (random({scaleCoordinate, scaleCoordinate}) * 2 - 1);
				if (axis == 0) {
					point.X += offset;
					scale.X *= factor;
				} else {
					point.Y += offset;
					scale.Y *= factor;
				}
				if (scale.X == 0 || scale.Y == 0)
					return context.Fail(
						Status::UnsupportedExecution,
						"Grid secondary scale division is undefined",
						"secondary_scale"
					);
				const double ratio = context.Boolean("uniform_gap", true)
										 ? (axis == 0 ? scale.Y / scale.X : scale.X / scale.Y)
										 : 1;
				const Vector2 cellOrigin{
					std::floor(point.X * scale.X) / scale.X, std::floor(point.Y * scale.Y) / scale.Y
				};
				const Vector2 distance{
					std::abs((point.X - cellOrigin.X) * scale.X - .5) * 2,
					std::abs((point.Y - cellOrigin.Y) * scale.Y - .5) * 2
				};
				const double depth = 1 - (axis == 0 ? std::max((distance.X - 1) * ratio + 1, distance.Y)
													: std::max((distance.Y - 1) * ratio + 1, distance.X));
				const double heightValue =
					undefinedHeight
						? 0
						: mix(levelOut.X, levelOut.Y, (depth - levelIn.X) / (levelIn.Y - levelIn.X));
				if (heightmap && !WritePixel(*heightmap, x, y, {heightValue, heightValue, heightValue, 1}))
					return context.Fail(Status::InvalidValue, "Grid height sample is nonfinite");
				Rgba colour{heightValue, heightValue, heightValue, alpha};
				if (mode != 2) {
					const double colourSeed = context.Scalar("color_seed") / 1000;
					const Rgba base = GradientEval(
						gradient,
						ShaderFract(
							ShaderFract(
								random({cellOrigin.X + colourSeed, cellOrigin.Y + colourSeed}) +
								context.Scalar("shift_2")
							) +
							1
						)
					);
					colour = base;
					if (mode == 3 || mode == 4) {
						Vector2 textureUv =
							mode == 3
								? Vector2{ShaderFract(point.X * scale.X), ShaderFract(point.Y * scale.Y)}
								: Vector2{ShaderFract(cellOrigin.X), ShaderFract(cellOrigin.Y)};
						if (mode == 3 && context.Boolean("truchet")) {
							const double textureSeed = context.Scalar("texture_seed") / 100;
							const Vector2 tile{
								std::floor(point.X * scale.X) + textureSeed,
								std::floor(point.Y * scale.Y) + textureSeed
							};
							if (random(tile) >= context.Scalar("flip_horizontal", .5))
								textureUv.X = 1 - textureUv.X;
							if (random({tile.X + .4864, tile.Y + .6879}) >=
								context.Scalar("flip_vertical", .5))
								textureUv.Y = 1 - textureUv.Y;
							const double localSeed = random({tile.X + .9843, tile.Y + .1636});
							const auto localRandom = [&](double increment) {
								return random({localSeed + increment, localSeed + increment});
							};
							const Vector2 scaleTexture{
								mix(textureScale.X, textureScale.Y, localRandom(3)),
								mix(textureScale.Z, textureScale.W, localRandom(4))
							};
							if (scaleTexture.X == 0 || scaleTexture.Y == 0)
								return context.Fail(
									Status::UnsupportedExecution,
									"Grid source texture scale division is undefined",
									"random_scale_2"
								);
							textureUv = source2d::Rotate(
								{textureUv.X - .5, textureUv.Y - .5},
								mix(textureAngle.X, textureAngle.Y, localRandom(0)) * std::acos(-1.0) / 180
							);
							textureUv = {
								textureUv.X / scaleTexture.X + .5 -
									mix(texturePosition.X, texturePosition.Z, localRandom(1)),
								textureUv.Y / scaleTexture.Y + .5 -
									mix(texturePosition.Y, texturePosition.W, localRandom(2))
							};
						}
						const Rgba sampled =
							SampleTextureUv(*texture, textureUv.X, textureUv.Y, 0, UvMap{}, sampler);
						for (size_t channel = 0; channel < 4; ++channel)
							colour[channel] = sampled[channel] * base[channel];
					}
					const double gap = MappedScalar(context, "gap", u, v, Filtered(sampler)),
								 antialias = 4.0 / std::max(dimension.X, dimension.Y);
					const double transition =
						context.Boolean("anti_aliasing")
							? std::clamp((depth - gap + antialias) / antialias, 0.0, 1.0)
							: (depth >= gap ? 1.0 : 0.0);
					const double gapBlend =
						1 - (context.Boolean("anti_aliasing") ? transition * transition * (3 - 2 * transition)
															  : transition);
					for (size_t channel = 0; channel < 4; ++channel)
						colour[channel] = mix(colour[channel], gapColour[channel], gapBlend);
					colour[3] *= alpha;
				}
				if (!WritePixel(*output, x, y, colour))
					return context.Fail(Status::InvalidValue, "Grid colour sample is nonfinite");
				if (mask) {
					colour = ReadPixel(*output, x, y);
					const Rgba sample = SampleNearest(*mask, u, v);
					colour[3] *= ((sample[0] + sample[1] + sample[2]) / 3) * sample[3];
					for (double &channel : colour)
						channel = Quantize(channel) / 255.;
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(Status::InvalidValue, "Grid mask sample is nonfinite");
				}
			}
		return context.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
