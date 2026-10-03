#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceMappedInputs.hpp"
#include "SourceNormalShaderCurve.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		using OcclusionPixel = std::array<float, 4>;
		OcclusionPixel OcclusionSample(const Image &image, float u, float v, bool filtered) {
			const auto sample = Texture(image, u, v, filtered);
			return {float(sample[0]), float(sample[1]), float(sample[2]), float(sample[3])};
		}
		float OcclusionHeight(const OcclusionPixel &sample) {
			return (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
		}
		// The source divides by zero at every radius-zero sample. This ordered
		// comparison is the deterministic native profile, not a guarantee about a
		// driver's undefined NaN reduction.
		float OcclusionMaximum(float left, float right) {
			return left < right ? right : left;
		}
		double OcclusionReferenceWidth(const NodeContext &context, const Image &source) {
			for (const auto &[port, images] : context.ImageArrays)
				if (port == "height_map" && images) {
					if (!images->Items.empty()) {
						const auto *index = std::get_if<size_t>(&images->Items.front().Data);
						if (index && *index < images->Images.size()) return images->Images[*index].Width;
					}
					// getDimension(0) returns PROJ_SURF for a nonsurface first array item.
					return context.Project.SurfaceWidth;
				}
			return source.Width;
		}
		bool OcclusionRange(NodeContext &context, std::string_view port, double fallback, Vector2 &range) {
			const Value *input = context.Find(port);
			const bool originalPair =
				!context.Boolean("attribute_process", true) && input &&
				(std::holds_alternative<ArrayValue>(*input) || std::holds_alternative<Vector2>(*input));
			if (context.Boolean(std::string(port) + "_mapped") || originalPair)
				return ReadSourceMappedRange(context, port, range);
			const auto number = input ? SourceChoiceNumber(*input) : std::optional<double>{fallback};
			if (!number)
				return context.Fail(
					Status::UnsupportedExecution, "AO scalar getter needs an observed numeric value", port
				);
			const double value = *number;
			range = {value, value};
			return true;
		}
		const Value *OcclusionOriginal(const NodeContext &context, std::string_view port) {
			for (auto value = context.ProcessorOriginalValues.rbegin();
				 value != context.ProcessorOriginalValues.rend();
				 ++value)
				if (value->first == port) return value->second;
			for (const auto &[id, value] : context.Values)
				if (id == port) return &value;
			return context.Find(port);
		}
		template <typename T>
		bool OcclusionNumericMaximum(
			NodeContext &context,
			const T &value,
			double &maximum,
			std::string_view port,
			size_t depth = 0,
			bool *nonzero = nullptr
		) {
			if (depth > Limits::MaximumArrayDepth)
				return context.Fail(Status::LimitExceeded, "AO control exceeds admitted depth", port);
			return std::visit(
				[&](const auto &data) -> bool {
					using Data = std::decay_t<decltype(data)>;
					if constexpr (std::is_same_v<Data, double> || std::is_same_v<Data, int64_t> ||
								  std::is_same_v<Data, bool>) {
						if (!std::isfinite(double(data)))
							return context.Fail(
								Status::UnsupportedExecution, "AO control is nonfinite", port
							);
						if (nonzero) *nonzero = *nonzero || data != 0;
						maximum = std::max(maximum, double(data));
						return true;
					} else if constexpr (std::is_same_v<Data, EnumValue>) {
						if (nonzero) *nonzero = *nonzero || data.Value != 0;
						maximum = std::max(maximum, double(data.Value));
						return true;
					} else if constexpr (std::is_same_v<Data, Vector2>) {
						if (!std::isfinite(data.X) || !std::isfinite(data.Y))
							return context.Fail(
								Status::UnsupportedExecution, "AO control is nonfinite", port
							);
						if (nonzero) *nonzero = *nonzero || data.X != 0 || data.Y != 0;
						maximum = std::max({maximum, data.X, data.Y});
						return true;
					} else if constexpr (std::is_same_v<Data, ArrayValue>) {
						for (const auto &element : data.Elements)
							if (!OcclusionNumericMaximum(context, element, maximum, port, depth + 1, nonzero))
								return false;
						for (const auto &row : data.Nested)
							for (const auto &element : row)
								if (!OcclusionNumericMaximum(
										context, element, maximum, port, depth + 2, nonzero
									))
									return false;
						const auto items =
							[&](const auto &self, const SourceArrayItem &item, size_t level) -> bool {
							if (level > Limits::MaximumArrayDepth)
								return context.Fail(
									Status::LimitExceeded, "AO control exceeds admitted depth", port
								);
							if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								return OcclusionNumericMaximum(context, *leaf, maximum, port, level, nonzero);
							for (const auto &child : std::get<std::vector<SourceArrayItem>>(item.Data))
								if (!self(self, child, level + 1)) return false;
							return true;
						};
						for (const auto &item : data.Items)
							if (!items(items, item, depth + 1)) return false;
						return true;
					} else
						return context.Fail(
							Status::UnsupportedExecution, "AO control needs numeric leaves", port
						);
				},
				value
			);
		}
		bool OcclusionBatchAdmission(NodeContext &context) {
			if (context.ProcessorRow != 0) return true;
			const Image *source = context.Input("height_map");
			if (!source)
				return context.Fail(Status::TypeMismatch, "AO requires its height surface", "height_map");
			uint64_t pixels = uint64_t(source->Width) * source->Height;
			for (const auto &[port, images] : context.ImageArrays) {
				if (port != "height_map" || !images) continue;
				// Backing images are already bounded, including those retained beneath nested item rows.
				for (const auto &image : images->Images)
					pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
			}
			const auto toggle = [&](std::string_view port, bool &enabled) {
				double maximum = 0;
				const Value *original = OcclusionOriginal(context, port);
				return !original || OcclusionNumericMaximum(context, *original, maximum, port, 0, &enabled);
			};
			bool mapped = false, curved = false;
			if (!toggle("height_mapped", mapped) || !toggle("intensity_curved", curved)) return false;
			const Value *height = OcclusionOriginal(context, "height");
			double maximum = -std::numeric_limits<double>::infinity();
			if (!height || !OcclusionNumericMaximum(context, *height, maximum, "height")) return false;
			// Include both sources conservatively when any original toggle enables mapping.
			if (mapped) {
				const Value *range = OcclusionOriginal(context, "height_map_range");
				if (range && !OcclusionNumericMaximum(context, *range, maximum, "height")) return false;
			}
			double unit = 0;
			if (const Value *units = OcclusionOriginal(context, "height_unit");
				units && !OcclusionNumericMaximum(context, *units, unit, "height_unit"))
				return false;
			const auto domain = context.InputDomain("height");
			if (unit > 0 && !(domain && domain->Kind == SourceSocketKind::Surface))
				maximum *= OcclusionReferenceWidth(context, *source);
			const float shaderMaximum = float(maximum);
			if (!std::isfinite(shaderMaximum))
				return context.Fail(
					Status::UnsupportedExecution, "AO batch exceeds finite height uniforms", "height"
				);
			const uint64_t limit = 64'000'000 / std::max<size_t>(1, context.ProcessorCount);
			const double steps = shaderMaximum < 0 ? 0 : std::floor(double(shaderMaximum)) + 1;
			const uint64_t curveWork = curved ? 73 : 1;
			// Eight curve segments and eight Newton iterations are conservatively charged per sample.
			const uint64_t directionWork = 65 * curveWork;
			if (steps > double(limit / directionWork) ||
				pixels > limit / (3 + directionWork * uint64_t(steps)))
				return context.Fail(
					Status::LimitExceeded, "AO original batch exceeds aggregate sample work", "height"
				);
			return true;
		}
		bool SourceAmbientOcclusion(NodeContext &context) {
			if (!OcclusionBatchAdmission(context)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("height_map");
			if (!source)
				return context.Fail(
					Status::TypeMismatch, "ambient occlusion requires its height surface", "height_map"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			const auto sampler = ReadSampler(context);
			// sh_sao uses sampler_simple rather than reconstructed interpolation kernels.
			// Pixel and CleanEdge disable hardware filtering; other choices are bilinear.
			const bool filtered = Filtered(sampler), pixelSweep = context.Boolean("pixel_sweep", true),
					   blend = context.Boolean("blend_original");
			Vector2 heightRange, intensityRange;
			if (!OcclusionRange(context, "height", .25, heightRange) ||
				!OcclusionRange(context, "intensity", 4, intensityRange))
				return false;
			const int64_t unit = context.Integer("height_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(
					Status::InvalidValue, "ambient occlusion height unit is invalid", "height_unit"
				);
			const auto heightDomain = context.InputDomain("height");
			const bool surfaceGetter = heightDomain && heightDomain->Kind == SourceSocketKind::Surface;
			const double width = unit && !surfaceGetter ? OcclusionReferenceWidth(context, *source) : 1;
			const std::array<float, 2> heights{float(heightRange.X * width), float(heightRange.Y * width)},
				intensities{float(intensityRange.X), float(intensityRange.Y)};
			const float strength = float(context.Scalar("blend_strength", 1));
			for (float value : {heights[0], heights[1], intensities[0], intensities[1], strength})
				if (!std::isfinite(value))
					return context.Fail(
						Status::UnsupportedExecution,
						"ambient occlusion exceeds finite shader uniforms",
						"height"
					);
			const float maximum = OcclusionMaximum(heights[0], heights[1]);
			const double steps = maximum < 0 ? 0 : std::floor(double(maximum)) + 1;
			const uint64_t workLimit = 64'000'000 / std::max<size_t>(1, context.ProcessorCount);
			const uint64_t pixels = uint64_t(source->Width) * source->Height;
			if (steps > double(workLimit / 65) || pixels > workLimit / (3 + 65 * uint64_t(steps)))
				return context.Fail(
					Status::LimitExceeded, "ambient occlusion exceeds aggregate sample work", "height"
				);
			const Image *heightMap =
							context.Boolean("height_mapped") ? context.Input("height_map_2") : nullptr,
						*intensityMap =
							context.Boolean("intensity_mapped") ? context.Input("intensity_map") : nullptr;
			const bool curved = context.Boolean("intensity_curved");
			const Curve *curve = context.Find("intensity_curve")
									 ? std::get_if<Curve>(context.Find("intensity_curve"))
									 : nullptr;
			if (curved && (!curve || curve->Anchors.empty() || curve->Anchors.size() > 9 ||
						   !std::isfinite(float(curve->Header[1])) || float(curve->Header[1]) == 0))
				return context.Fail(
					Status::UnsupportedExecution,
					"ambient occlusion curve exceeds the portable shader profile",
					"intensity_curve"
				);
			if (curved) {
				const auto finite = [](double value) { return std::isfinite(float(value)); };
				if (!std::all_of(curve->Header.begin(), curve->Header.end(), finite))
					return context.Fail(
						Status::UnsupportedExecution,
						"AO curve exceeds finite shader uniforms",
						"intensity_curve"
					);
				for (const auto &anchor : curve->Anchors)
					if (!std::all_of(anchor.begin(), anchor.end(), finite))
						return context.Fail(
							Status::UnsupportedExecution,
							"AO curve exceeds finite shader uniforms",
							"intensity_curve"
						);
			}
			const int64_t mode = context.Integer("blendmode");
			if (blend && (mode < 0 || mode > 1))
				return context.Fail(
					Status::InvalidValue, "ambient occlusion blend mode is invalid", "blendmode"
				);
			Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			const auto mapped = [](const Image *map, const std::array<float, 2> &range, float u, float v) {
				if (!map) return range[0];
				const auto sample = OcclusionSample(*map, u, v, false);
				const float amount = (sample[0] + sample[1] + sample[2]) / 3;
				return range[0] + (range[1] - range[0]) * amount;
			};
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const float u = (float(x) + .5f) / float(source->Width),
								v = (float(y) + .5f) / float(source->Height);
					const auto background = OcclusionSample(*source, u, v, filtered);
					const float center = OcclusionHeight(background),
								height = mapped(heightMap, heights, u, v),
								intensity = mapped(intensityMap, intensities, u, v);
					if (!std::isfinite(center) || !std::isfinite(height) || !std::isfinite(intensity))
						return context.Fail(
							Status::UnsupportedExecution,
							"ambient occlusion sampled nonfinite shader values",
							"height_map"
						);
					float occlusion = 0, top = 0, base = 1;
					for (unsigned direction = 0; direction <= 64; direction++) {
						const float angle = (pixelSweep ? top / base : float(direction) / 64) *
											(2 * std::numbers::pi_v<float>);
						top += 2;
						if (top >= base) {
							top = 1;
							base *= 2;
						}
						float directional = 0;
						for (float radius = 0; radius <= maximum; radius++) {
							if (radius > height) break;
							const auto sample = SampleTextureSimple(
								*source,
								u + std::cos(angle) * radius / float(source->Width),
								v + std::sin(angle) * radius / float(source->Height),
								sampler.Oversample,
								filtered
							);
							const float sampled = (float(sample[0]) + float(sample[1]) + float(sample[2])) /
												  3 * float(sample[3]);
							if (!std::isfinite(sampled))
								return context.Fail(
									Status::UnsupportedExecution,
									"ambient occlusion sampled nonfinite height",
									"height_map"
								);
							const float delta = (sampled - center) * height;
							const float difference = (delta - radius) / delta;
							float amount = OcclusionMaximum(0, sampled - center) * difference * intensity;
							if (curved) amount *= NormalEvalShaderCurve(*curve, radius / height);
							directional = OcclusionMaximum(directional, amount);
						}
						occlusion += directional / 64;
					}
					const float ambient = OcclusionMaximum(0, 1 - occlusion);
					OcclusionPixel result{ambient, ambient, ambient, background[3]};
					if (blend) {
						for (size_t channel = 0; channel < 3; channel++) {
							const float edited =
								mode == 0 ? background[channel] * ambient : background[channel] - ambient;
							result[channel] = background[channel] * (1 - strength) + edited * strength;
						}
					}
					if (!WritePixel(*output, x, y, {result[0], result[1], result[2], result[3]}))
						return context.Fail(
							Status::UnsupportedExecution,
							"ambient occlusion produced nonfinite output",
							"surface_out"
						);
				}
			return context.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceAmbientOcclusionExecutors() {
		static const ExecutorEntry entries[] = {{"pc.ambient_occlusion", SourceAmbientOcclusion, true}};
		return entries;
	}
} // namespace engine::imagegraph::detail
