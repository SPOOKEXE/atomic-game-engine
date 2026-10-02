#include "../SourceFlipDomain.hpp"
#include "../SourceGradient.hpp"
#include "../SourceRandom.hpp"
#include "FlipNodes.hpp"
#include "Sampler.hpp"
#include "SourceFlipLines.hpp"
#include "SourceFlipObstacle.hpp"
#include "SourceFlipSprite.hpp"
#include "SourceFlipSpriteFrames.hpp"

#include <engine/imagegraph/FlipReplay.hpp>

namespace engine::imagegraph::detail {
	namespace {
		struct DropVertex {
			Vector2 Position;
			double Alpha;
		};
		double DropCross(Vector2 a, Vector2 b, Vector2 c) {
			return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
		}
		bool DropTopLeft(Vector2 a, Vector2 b) {
			return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
		}
		bool DropBlend(Image &image, uint32_t x, uint32_t y, Rgba source, bool additive) {
			const auto destination = ReadPixel(image, x, y);
			Rgba result;
			for (size_t channel = 0; channel < 3; ++channel)
				result[channel] =
					source[channel] * source[3] + destination[channel] * (additive ? 1 : 1 - source[3]);
			result[3] = source[3] * (additive ? source[3] : 1) + destination[3];
			return WritePixel(image, x, y, result);
		}
		bool DropTriangle(
			NodeContext &context,
			Image &image,
			DropVertex a,
			DropVertex b,
			DropVertex c,
			Colour color,
			bool additive,
			uint64_t &work
		) {
			double area = DropCross(a.Position, b.Position, c.Position);
			if (!std::isfinite(area))
				return context.Fail(Status::InvalidValue, "FLIP droplet triangle is nonfinite");
			if (area == 0) return true;
			if (area < 0) {
				std::swap(b, c);
				area = -area;
			}
			const auto lowX = uint32_t(
				std::clamp(
					std::ceil(std::min({a.Position.X, b.Position.X, c.Position.X}) - .5),
					0.,
					double(image.Width)
				)
			);
			const auto lowY = uint32_t(
				std::clamp(
					std::ceil(std::min({a.Position.Y, b.Position.Y, c.Position.Y}) - .5),
					0.,
					double(image.Height)
				)
			);
			const auto highX = uint32_t(
				std::clamp(
					std::floor(std::max({a.Position.X, b.Position.X, c.Position.X}) - .5) + 1,
					0.,
					double(image.Width)
				)
			);
			const auto highY = uint32_t(
				std::clamp(
					std::floor(std::max({a.Position.Y, b.Position.Y, c.Position.Y}) - .5) + 1,
					0.,
					double(image.Height)
				)
			);
			const uint64_t samples = uint64_t(highX - lowX) * (highY - lowY);
			if (samples > FluidDomainLimits::MaximumWork - work)
				return context.Fail(
					Status::LimitExceeded, "FLIP droplet reference raster exceeds bounded sample work"
				);
			work += samples;
			for (uint32_t y = lowY; y < highY; ++y)
				for (uint32_t x = lowX; x < highX; ++x) {
					const Vector2 point{double(x) + .5, double(y) + .5};
					const double ca = DropCross(b.Position, c.Position, point),
								 cb = DropCross(c.Position, a.Position, point),
								 cc = DropCross(a.Position, b.Position, point);
					if (ca < 0 || (ca == 0 && !DropTopLeft(b.Position, c.Position)) || cb < 0 ||
						(cb == 0 && !DropTopLeft(c.Position, a.Position)) || cc < 0 ||
						(cc == 0 && !DropTopLeft(a.Position, b.Position)))
						continue;
					const double alpha = std::pow((ca * a.Alpha + cb * b.Alpha + cc * c.Alpha) / area, 5);
					if (!DropBlend(
							image,
							x,
							y,
							{color.Red / 255., color.Green / 255., color.Blue / 255., alpha},
							additive
						))
						return context.Fail(Status::InvalidValue, "FLIP droplet blend is nonfinite");
				}
			return true;
		}
		bool DropCircle(
			NodeContext &context,
			Image &image,
			Vector2 position,
			double radius,
			double alpha,
			Colour color,
			bool additive,
			uint64_t &work
		) {
			std::array<DropVertex, 99> vertices;
			for (size_t index = 0; index <= 32; ++index) {
				const double angle = double(index) * std::numbers::pi / 16,
							 next = angle + std::numbers::pi / 16;
				vertices[index * 3] = {position, alpha};
				vertices[index * 3 + 1] = {
					{position.X + std::cos(angle) * radius, position.Y - std::sin(angle) * radius}, 0
				};
				vertices[index * 3 + 2] = {
					{position.X + std::cos(next) * radius, position.Y - std::sin(next) * radius}, 0
				};
			}
			// This is the source triangle strip, including its duplicated fan sectors and
			// final repeated sector.
			for (size_t index = 2; index < vertices.size(); ++index)
				if (!DropTriangle(
						context,
						image,
						vertices[index - 2],
						vertices[index - 1],
						vertices[index],
						color,
						additive,
						work
					))
					return false;
			return true;
		}
		bool DropLine(
			NodeContext &context,
			Image &image,
			Vector2 from,
			Vector2 to,
			double width,
			Colour color,
			bool additive,
			uint64_t &work
		) {
			// Official HTML5 applies this offscreen-target Y offset before constructing its quad.
			from.Y -= .01;
			to.Y -= .01;
			const double dx = to.X - from.X, dy = to.Y - from.Y, squared = dx * dx + dy * dy;
			if (!std::isfinite(squared))
				return context.Fail(Status::InvalidValue, "FLIP line geometry is undefined");
			if (squared < .0001) return true;
			const double length = std::sqrt(squared);
			if (length < .0001) return true;
			const double xx = .5 * width * dx / length, yy = .5 * width * dy / length;
			const auto vertex = [](double x, double y) {
				return DropVertex{{double(float(x)), double(float(y))}, 1};
			};
			const auto a = vertex(from.X - yy, from.Y + xx), b = vertex(to.X - yy, to.Y + xx),
					   c = vertex(to.X + yy, to.Y - xx), d = vertex(from.X + yy, from.Y - xx);
			return DropTriangle(context, image, a, b, c, color, additive, work) &&
				   DropTriangle(context, image, c, d, a, color, additive, work);
		}
	} // namespace
	bool FlipRender(NodeContext &context) {
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP CPU reference raster requires captured GameMaker "
				"GPU coverage for exact target coverage",
				"domain"
			);
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) return true;
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP render domain is invalid", "domain");
		if (context.Request.ReuseSimulationFrame) {
			input = CapturedFlipFrame(context, input->Data->OriginNodeId, input->Data->OriginProcessorRow);
			if (!input) return false;
		}
		const auto mode = context.Integer("render_type", 0),
				   requestedSteps = context.Integer("update_step", 1);
		const auto steps = context.Request.ReuseSimulationFrame ? int64_t{0} : requestedSteps;
		if (mode < 0 || mode > 1)
			return context.Fail(Status::InvalidValue, "FLIP source render type is invalid", "render_type");
		if (requestedSteps < 0)
			return context.Fail(
				Status::InvalidValue, "FLIP render update step must be nonnegative", "update_step"
			);
		if (requestedSteps > FluidDomainLimits::MaximumIterations)
			return context.Fail(
				Status::LimitExceeded, "FLIP render steps exceed native bound", "update_step"
			);
		// Native physics replay uses integer samples; zero-step rendering remains continuous.
		if (steps > 0 && context.Request.Subframe != 0)
			return context.Fail(
				Status::InvalidValue, "FLIP render advance requires an integer frame", "update_step"
			);
		const auto &settings = input->Data->Settings;
		const double width = settings.Width - settings.Spacing * 2,
					 height = settings.Height - settings.Spacing * 2;
		if (width < 1 || height < 1 || width != std::trunc(width) || height != std::trunc(height) ||
			width > Limits::MaximumDimension || height > Limits::MaximumDimension)
			return context.Fail(
				Status::InvalidValue,
				"FLIP reference raster requires bounded integral output dimension",
				"domain"
			);
		auto scratch = context.ReserveWorkspace(
			FluidStorageBytes<true>(*input) * 2 +
				uint64_t(steps) *
					(sizeof(FluidDomainData::HistoryFrame) * (input->Data->History.size() + steps) +
					 uint64_t(input->Data->ParticleCount) * 9 * sizeof(double)) +
				input->Data->Obstacles.size() * sizeof(source_flip::SourceObstacle),
			"domain"
		);
		if (!scratch) return false;
		FluidDomainValue stepped = *input;
		Diagnostic diagnostic;
		for (int64_t iteration = 0; iteration < steps; ++iteration) {
			const auto target = context.Request.Tick == 0 ? 1 : context.Request.Tick;
			stepped.Data->Tick = target - 1;
			const auto status = StepFlipReplay(
				stepped,
				target,
				context.Request.SimulationAuthoringRevision,
				Limits::MaximumEvaluationBytes,
				FluidDomainLimits::MaximumWork / uint64_t(steps),
				stepped,
				diagnostic
			);
			if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "domain");
			NormalizeFlipFrameTick(*stepped.Data, context.Request.Tick);
		}
		stepped.Data->Tick = context.Request.Tick;
		if (steps > 0 && !PublishSimulationFluidUpdate(context, stepped)) return false;
		auto *output = context.NewImage("rendered", uint32_t(width), uint32_t(height));
		if (!output) return false;
		auto temporaryCharge =
			context.ReserveWorkspace(uint64_t(output->Width) * output->Height * 4, "rendered");
		if (!temporaryCharge) return false;
		Image temporary{
			output->Width, output->Height, std::vector<uint8_t>(size_t(output->Width) * output->Height * 4)
		};
		const Vector2 alpha = context.Vec2("alpha", {1, 1}), lifespan = context.Vec2("lifespan", {}),
					  map = context.Vec2("velocity_map", {0, 10});
		const double size = context.Scalar("particle_size", 20), shift = context.Scalar("shift", 0),
					 threshold = 1 - context.Scalar("merge_threshold", .75);
		if (!MeshFinite(alpha) || !MeshFinite(lifespan) || !MeshFinite(map) || !std::isfinite(size) ||
			!std::isfinite(shift) || !std::isfinite(threshold))
			return context.Fail(Status::InvalidValue, "FLIP render controls must be finite");
		const auto *value = context.Find("color_over_velocity");
		const auto *gradient = value ? std::get_if<Gradient>(value) : nullptr;
		if (gradient && gradient->Keys.empty())
			return context.Fail(
				Status::UnsupportedExecution,
				"source FLIP render reads an undefined empty gradient key",
				"color_over_velocity"
			);
		Colour color = gradient ? gradient->Keys[0].Color : Colour{255, 255, 255, 255};
		const auto &data = *stepped.Data;
		const auto &positions = data.ReadbackPositions, &velocities = data.ReadbackVelocities,
				   &life = data.ReadbackLife;
		const double radius = settings.Spacing + FluidDomainLayout(settings)->ParticleRadius * size;
		if (!std::isfinite(radius))
			return context.Fail(Status::InvalidValue, "FLIP render radius is nonfinite", "particle_size");
		SourceFlipSpriteFrames sprites;
		if (mode == 0 && !ResolveSourceFlipSpriteFrames(context, sprites)) return false;
		const Image *firstSprite = sprites.Count ? sprites.At(0) : nullptr;
		SourceRandom random{uint32_t(context.Request.Seed)};
		uint64_t work = 0;
		const size_t count =
			std::min(size_t(settings.MaximumParticles - 1), size_t(SourceFluidParticleCount(data)));
		if (mode == 1) {
			if (!VisitSourceFlipLines(
					context,
					data,
					gradient,
					lifespan,
					map,
					shift,
					context.Integer("segments", 1),
					context.Scalar("thickness", 1),
					[&](Vector2 from, Vector2 to, double thickness, Colour lineColor) {
						return DropLine(
							context,
							temporary,
							from,
							to,
							thickness,
							lineColor,
							context.Boolean("additive", true),
							work
						);
					}
				))
				return false;
		} else {
			for (size_t index = 0; index < count; ++index) {
				if (index >= life.size()) continue;
				if (positions[index * 2] == 0 && positions[index * 2 + 1] == 0) continue;
				const double opacity = random.Range(alpha.X, alpha.Y),
							 duration = double(random.IntRange(lifespan.X, lifespan.Y));
				const double ratio = duration ? (duration - life[index]) / duration : 1;
				if (duration && ratio * radius < .5) continue;
				if (gradient && gradient->Keys.size() > 1) {
					const double velocity = std::hypot(velocities[index * 2], velocities[index * 2 + 1]);
					const double normalized = (velocity - map.X) / (map.Y - map.X);
					if (!std::isfinite(normalized))
						return context.Fail(
							Status::InvalidValue,
							"FLIP velocity gradient range produced nonfinite ratio",
							"velocity_map"
						);
					const auto mapped = SourceCachedGradient(
						*gradient, Fract(std::pow(std::clamp(normalized, 0., 1.), 5) + shift)
					);
					if (!mapped)
						return context.Fail(
							Status::InvalidValue,
							"FLIP velocity gradient produced nonfinite color",
							"color_over_velocity"
						);
					color = *mapped;
				}
				if (const Image *sprite = sprites.At(index)) {
					if (!DrawSourceFlipSprite(
							context,
							temporary,
							*sprite,
							{double(firstSprite->Width), double(firstSprite->Height)},
							{positions[index * 2] - settings.Spacing,
							 positions[index * 2 + 1] - settings.Spacing},
							ratio,
							color,
							opacity,
							Filtered(ReadSampler(context)),
							work,
							[&](uint32_t x, uint32_t y, Rgba pixel) {
								return DropBlend(temporary, x, y, pixel, context.Boolean("additive", true));
							}
						))
						return false;
				} else {
					if (!DropCircle(
							context,
							temporary,
							{positions[index * 2] - settings.Spacing,
							 positions[index * 2 + 1] - settings.Spacing},
							radius,
							opacity * ratio,
							color,
							context.Boolean("additive", true),
							work
						))
						return false;
				}
			}
		}
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				Rgba sample = ReadPixel(temporary, x, y);
				if (context.Boolean("threshold", true)) {
					if (sample[3] > threshold)
						sample[3] = 1;
					else
						sample = {};
				}
				for (size_t channel = 0; channel < 3; ++channel)
					sample[channel] *= sample[3];
				sample[3] *= sample[3];
				if (!WritePixel(*output, x, y, sample))
					return context.Fail(Status::InvalidValue, "FLIP threshold output is nonfinite");
			}
		if (context.Boolean("draw_obstracles", true)) {
			for (const auto &visual : stepped.Data->ObstacleVisuals) {
				const auto control = std::lower_bound(
					stepped.Data->ObstacleControls.begin(),
					stepped.Data->ObstacleControls.end(),
					std::tie(visual.NodeId, visual.ProcessorRow),
					[](const auto &item, const auto &key) {
						return std::tie(item.NodeId, item.ProcessorRow) < key;
					}
				);
				if (control == stepped.Data->ObstacleControls.end() || !control->Texture) continue;
				const auto &texture = *control->Texture;
				const double left = control->X - texture.Width / 2., top = control->Y - texture.Height / 2.;
				const auto x0 = uint32_t(std::clamp(std::ceil(left - .5), 0., double(output->Width))),
						   x1 = uint32_t(
							   std::clamp(std::ceil(left + texture.Width - .5), 0., double(output->Width))
						   ),
						   y0 = uint32_t(std::clamp(std::ceil(top - .5), 0., double(output->Height))),
						   y1 = uint32_t(
							   std::clamp(std::ceil(top + texture.Height - .5), 0., double(output->Height))
						   );
				const uint64_t samples = uint64_t(x1 - x0) * (y1 - y0);
				if (samples > FluidDomainLimits::MaximumWork - work)
					return context.Fail(
						Status::LimitExceeded,
						"FLIP obstacle texture raster exceeds bounded work",
						"draw_obstracles"
					);
				work += samples;
				for (uint32_t y = y0; y < y1; ++y)
					for (uint32_t x = x0; x < x1; ++x) {
						const auto source = Texture(
							texture, (x + .5 - left) / texture.Width, (y + .5 - top) / texture.Height, false
						);
						const auto destination = ReadPixel(*output, x, y);
						Rgba blended;
						for (size_t channel = 0; channel < 4; ++channel)
							blended[channel] =
								source[channel] * source[3] + destination[channel] * (1 - source[3]);
						if (!WritePixel(*output, x, y, blended))
							return context.Fail(
								Status::InvalidValue,
								"FLIP obstacle texture blend is nonfinite",
								"draw_obstracles"
							);
					}
			}
		}
		return true;
	}
} // namespace engine::imagegraph::detail
