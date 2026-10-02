#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceProjectionGradient.hpp"
#include "SourceProjectionMath.hpp"

namespace engine::imagegraph::detail {
	namespace {
		using VolumePixel = std::array<float, 4>;
		VolumePixel VolumeSample(const Image &image, float u, float v, bool filtered) {
			const auto c = Texture(image, u, v, filtered);
			return {float(c[0]), float(c[1]), float(c[2]), float(c[3])};
		}
		float VolumeMean(const VolumePixel &c) {
			return (c[0] + c[1] + c[2]) / 3 * c[3];
		}
		bool VolumeProjection(NodeContext &context) {
			const Image *top = context.Input("top"), *front = context.Input("front"),
						*right = context.Input("right");
			const bool hasTop = top, hasFront = front, hasRight = right;
			uint32_t width = 0, height = 0;
			if (!ResolveDimension(context, "dimension", width, height)) return false;
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			if (!hasTop && !hasFront && !hasRight) {
				return context.Fail(
					Status::UnsupportedExecution,
					"source empty volume retains ambient output surface",
					"front"
				);
			}
			// Source fallback tests the original flags, then deliberately permutes sampler bindings.
			if (!hasFront) front = hasTop ? top : right;
			if (!hasRight) right = hasFront ? front : top;
			if (!hasTop) top = hasRight ? right : front;
			const float size = float(std::max(front->Width, front->Height)), voxelSize = 2 / size;
			const int64_t projection = context.Integer("projection", 1);
			if (projection != 0 && projection != 1)
				return context.Fail(
					Status::UnsupportedExecution,
					"source projection leaves camera ray uninitialized",
					"projection"
				);
			const float distance = float(context.Scalar("distance", 1)),
						scale = float(context.Scalar("scale", 3.46)), fov = float(context.Scalar("fov", 60)),
						density = float(context.Scalar("density", 10)),
						exponent = float(context.Scalar("exponent", 2.5)),
						threshold = float(context.Scalar("color_threshold", .5));
			const auto level = context.Vec2("level", {0, 1});
			const Vector3 sourceAngle = context.Get<Vector3>("view_angle", {30, 45, 0}),
						  sourcePosition = context.Get<Vector3>("position", {});
			const ProjectionVector angle{float(sourceAngle.X), float(sourceAngle.Y), float(sourceAngle.Z)},
				position{float(sourcePosition.X), float(sourcePosition.Y), float(sourcePosition.Z)};
			for (float value :
				 {distance, scale, fov, density, exponent, threshold, float(level.X), float(level.Y)})
				if (!std::isfinite(value))
					return context.Fail(
						Status::UnsupportedExecution,
						"volume controls exceed finite shader uniforms",
						"surface_out"
					);
			if (!ProjectionFinite(angle) || !ProjectionFinite(position))
				return context.Fail(
					Status::UnsupportedExecution, "volume camera exceeds finite shader uniforms", "position"
				);
			if (float(level.X) == float(level.Y))
				return context.Fail(
					Status::UnsupportedExecution, "source volume divides by zero level range", "level"
				);
			ProjectionMatrix inverse;
			if (!ProjectionInverseRotation(angle, inverse))
				return context.Fail(
					Status::UnsupportedExecution, "source volume camera rotation is singular", "view_angle"
				);
			const auto *gradient = context.Find("density_color")
									   ? std::get_if<Gradient>(context.Find("density_color"))
									   : nullptr;
			if (!gradient || gradient->Keys.empty() || gradient->Keys.size() > 64 || gradient->Mode > 6)
				return context.Fail(
					Status::UnsupportedExecution,
					"volume gradient exceeds defined GLSL upload",
					"density_color"
				);
			for (const auto &key : gradient->Keys)
				if (!std::isfinite(float(key.Time)))
					return context.Fail(
						Status::UnsupportedExecution,
						"volume gradient time exceeds finite shader uniform",
						"density_color"
					);
			const float maxVoxels = std::sqrt(3.f) * size * 2 * (projection == 0 ? distance : 1);
			const uint64_t pixels = uint64_t(width) * height,
						   limit = 64'000'000 / std::max<size_t>(1, context.ProcessorCount);
			const double visits = std::ceil(std::max(0.f, maxVoxels));
			if (!std::isfinite(visits) || visits > double(limit / 3) ||
				pixels > limit / (72 + 12 * uint64_t(visits)))
				return context.Fail(
					Status::LimitExceeded,
					"volume traversal exceeds bounded aggregate sample work",
					"dimension"
				);
			const auto sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			const bool filtered = Filtered(sampler);
			const Image *texture = context.Input("texture_side");
			const Colour base = context.Get<Colour>("base_color", {255, 255, 255, 255});
			const VolumePixel baseColour{
				base.Red / 255.f, base.Green / 255.f, base.Blue / 255.f, base.Alpha / 255.f
			};
			// A float ray is validated before the first allocation, and again for every view sample.
			ProjectionVector testEye, testDirection;
			if (!ProjectionRay(
					inverse,
					position,
					.5,
					.5,
					float(width) / height,
					int(projection),
					fov,
					distance,
					scale,
					testEye,
					testDirection
				))
				return context.Fail(
					Status::UnsupportedExecution, "source volume camera has undefined ray", "fov"
				);
			Image *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = 0; x < width; x++) {
					ProjectionVector eye, dir;
					if (!ProjectionRay(
							inverse,
							position,
							float((x + .5) / width),
							float((y + .5) / height),
							float(width) / height,
							int(projection),
							fov,
							distance,
							scale,
							eye,
							dir
						))
						return context.Fail(
							Status::UnsupportedExecution, "source volume camera has undefined ray", "fov"
						);
					for (float &value : dir)
						if (std::abs(value) < .001f) value = .001f;
					ProjectionVector ro{}, pos{}, ri{}, rs{}, dis{}, mm{}, colour{}, impact{};
					for (size_t c = 0; c < 3; c++) {
						ro[c] = eye[c] / voxelSize;
						pos[c] = std::floor(ro[c]);
						ri[c] = 1 / dir[c];
						rs[c] = dir[c] > 0 ? 1 : -1;
						dis[c] = (pos[c] - ro[c] + .5f + rs[c] * .5f) * ri[c];
					}
					if (!ProjectionFinite(ro) || !ProjectionFinite(dis))
						return context.Fail(
							Status::UnsupportedExecution,
							"source volume ray exceeds finite voxel coordinates",
							"position"
						);
					float volume = 0, transmit = 1;
					for (uint64_t i = 0; i < uint64_t(visits); i++) {
						ProjectionVector wc{}, sc{};
						for (size_t c = 0; c < 3; c++) {
							wc[c] = (pos[c] + .5f) * voxelSize;
							sc[c] = wc[c] * .5f + .5f;
						}
						if (sc[0] >= 0 && sc[0] < 1 && sc[1] >= 0 && sc[1] < 1 && sc[2] >= 0 && sc[2] < 1) {
							const auto a = VolumeSample(*front, sc[0], sc[1], filtered),
									   b = VolumeSample(*right, 1 - sc[2], sc[1], filtered),
									   c = VolumeSample(*top, sc[0], sc[2], filtered);
							float product = VolumeMean(a);
							product *= b[0] + b[1] + b[2];
							product /= 3;
							product *= b[3];
							product *= c[0] + c[1] + c[2];
							product /= 3;
							product *= c[3];
							if (product < 0 || (product == 0 && exponent <= 0))
								return context.Fail(
									Status::UnsupportedExecution,
									"source volume pow requires a positive density base",
									"exponent"
								);
							const float dens = std::pow(product, exponent) * density,
										stepAlpha = 1 - std::exp(-dens);
							const auto &face = mm[2] > .5f ? a : (mm[0] > .5f ? b : c);
							volume += dens + transmit * stepAlpha;
							for (size_t channel = 0; channel < 3; channel++)
								colour[channel] += transmit * stepAlpha * face[channel];
							transmit *= 1 - stepAlpha;
							if (!std::isfinite(volume) || !std::isfinite(transmit) ||
								!ProjectionFinite(colour))
								return context.Fail(
									Status::UnsupportedExecution,
									"source volume accumulation produced nonfinite math",
									"density"
								);
							if (transmit > threshold) impact = sc;
							if (transmit < .001f) break;
						}
						for (size_t c = 0; c < 3; c++)
							mm[c] = (dis[c] <= dis[(c + 1) % 3] && dis[c] <= dis[(c + 2) % 3]) ? 1 : 0;
						for (size_t c = 0; c < 3; c++) {
							dis[c] += mm[c] * rs[c] * ri[c];
							pos[c] += mm[c] * rs[c];
						}
					}
					volume = (volume - float(level.X)) / (float(level.Y) - float(level.X));
					const auto denColour = ProjectionGradient(*gradient, volume);
					VolumePixel tint = baseColour;
					if (texture) {
						const auto texel = VolumeSample(*texture, impact[0], impact[1], filtered);
						for (size_t c = 0; c < 4; c++)
							tint[c] *= texel[c];
					}
					Rgba result{};
					for (size_t c = 0; c < 4; c++)
						result[c] = (c < 3 ? colour[c] : volume) * tint[c] * float(denColour[c]);
					for (double &c : result) {
						if (!std::isfinite(c))
							return context.Fail(
								Status::UnsupportedExecution,
								"source volume gradient produced nonfinite math",
								"density_color"
							);
						if (*format == SurfaceFormat::RGBA8Unorm || *format == SurfaceFormat::RGBA4Unorm ||
							*format == SurfaceFormat::R8Unorm)
							c = std::clamp(c, 0., 1.);
					}
					// The source targets and clears the output directly, retaining ordinary alpha blending.
					const double alpha = result[3];
					for (double &c : result)
						c *= alpha;
					if (!WritePixel(*output, x, y, result))
						return context.Fail(
							Status::UnsupportedExecution,
							"source volume exceeds finite surface storage",
							"surface_out"
						);
				}
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceVolumeProjectionExecutors() {
		static const ExecutorEntry entries[] = {{"pc.surface_project_volume_3_d", VolumeProjection, true}};
		return entries;
	}
}
