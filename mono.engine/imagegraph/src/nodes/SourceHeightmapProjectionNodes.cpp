#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceProjectionGradient.hpp"
#include "SourceProjectionMath.hpp"

namespace engine::imagegraph::detail {
	namespace {
		using HeightmapPixel = std::array<float, 4>;
		HeightmapPixel HeightmapSample(const Image &image, float u, float v, bool filtered) {
			auto sample = Texture(image, u, v, filtered);
			return {float(sample[0]), float(sample[1]), float(sample[2]), float(sample[3])};
		}
		const Value *HeightmapOriginal(const NodeContext &context, std::string_view id) {
			for (auto p = context.ProcessorOriginalValues.rbegin();
				 p != context.ProcessorOriginalValues.rend();
				 ++p)
				if (p->first == id) return p->second;
			for (const auto &[port, value] : context.Values)
				if (port == id) return &value;
			return context.Find(id);
		}
		template <class V>
		bool HeightmapMaximum(
			NodeContext &context,
			const V &value,
			double &maximum,
			std::string_view id,
			size_t depth = 0,
			bool *zero = nullptr,
			size_t tupleWidth = 0
		) {
			if (depth == 0 && !std::visit([](const auto &data) { return ValidPayload(data, true); }, value))
				return context.Fail(
					Status::InvalidValue, "heightmap original control exceeds admitted payload", id
				);
			if (depth > Limits::MaximumArrayDepth)
				return context.Fail(Status::LimitExceeded, "heightmap control exceeds admitted depth", id);
			return std::visit(
				[&](const auto &data) -> bool {
					using T = std::decay_t<decltype(data)>;
					const auto number = [&](double v) {
						if (!std::isfinite(v))
							return context.Fail(
								Status::UnsupportedExecution, "heightmap control is nonfinite", id
							);
						if (zero) *zero = *zero || v == 0;
						maximum = std::max(maximum, v);
						return true;
					};
					if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
								  std::is_same_v<T, bool>)
						return number(double(data));
					else if constexpr (std::is_same_v<T, EnumValue>)
						return number(double(data.Value));
					else if constexpr (std::is_same_v<T, Vector2>)
						return number(data.X) && number(data.Y);
					else if constexpr (std::is_same_v<T, Vector3>)
						return number(data.X) && number(data.Y) && (tupleWidth == 2 || number(data.Z));
					else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
						return number(data.X) && number(data.Y) &&
							   (tupleWidth == 2 || (number(data.Z) && number(data.W)));
					else if constexpr (std::is_same_v<T, ArrayValue>) {
						const bool coordinateLeaves = data.ElementType != ValueType::Vector2 &&
													  data.ElementType != ValueType::Vector3 &&
													  data.ElementType != ValueType::Vector4 &&
													  data.ElementType != ValueType::Quaternion;
						const size_t elements = tupleWidth && coordinateLeaves
													? std::min(tupleWidth, data.Elements.size())
													: data.Elements.size();
						for (size_t i = 0; i < elements; ++i)
							if (!HeightmapMaximum(
									context, data.Elements[i], maximum, id, depth + 1, zero, tupleWidth
								))
								return false;
						for (const auto &row : data.Nested)
							for (size_t i = 0;
								 i < (tupleWidth ? std::min(tupleWidth, row.size()) : row.size());
								 ++i)
								if (!HeightmapMaximum(
										context, row[i], maximum, id, depth + 2, zero, tupleWidth
									))
									return false;
						const auto item =
							[&](const auto &self, const SourceArrayItem &entry, size_t level) -> bool {
							if (level > Limits::MaximumArrayDepth)
								return context.Fail(
									Status::LimitExceeded, "heightmap control exceeds admitted depth", id
								);
							if (auto *leaf = std::get_if<ElementValue>(&entry.Data))
								return HeightmapMaximum(context, *leaf, maximum, id, level, zero, tupleWidth);
							const auto &children = std::get<std::vector<SourceArrayItem>>(entry.Data);
							const bool flat =
								std::all_of(children.begin(), children.end(), [](const auto &child) {
									const auto *leaf = std::get_if<ElementValue>(&child.Data);
									return leaf && !std::holds_alternative<Vector2>(*leaf) &&
										   !std::holds_alternative<Vector3>(*leaf) &&
										   !std::holds_alternative<Vector4>(*leaf) &&
										   !std::holds_alternative<Quaternion>(*leaf);
								});
							const size_t n =
								tupleWidth && flat ? std::min(tupleWidth, children.size()) : children.size();
							for (size_t i = 0; i < n; ++i)
								if (!self(self, children[i], level + 1)) return false;
							return true;
						};
						const bool flat =
							std::all_of(data.Items.begin(), data.Items.end(), [](const auto &entry) {
								const auto *leaf = std::get_if<ElementValue>(&entry.Data);
								return leaf && !std::holds_alternative<Vector2>(*leaf) &&
									   !std::holds_alternative<Vector3>(*leaf) &&
									   !std::holds_alternative<Vector4>(*leaf) &&
									   !std::holds_alternative<Quaternion>(*leaf);
							});
						const size_t n =
							tupleWidth && flat ? std::min(tupleWidth, data.Items.size()) : data.Items.size();
						for (size_t i = 0; i < n; ++i)
							if (!item(item, data.Items[i], depth + 1)) return false;
						return true;
					} else
						return context.Fail(
							Status::UnsupportedExecution, "heightmap control needs numeric rows", id
						);
				},
				value
			);
		}
		bool HeightmapBatchAdmission(NodeContext &context, uint32_t width, uint32_t height) {
			if (context.ProcessorRow != 0) return true;
			if (context.Request.MaximumImageDimension == 0 ||
				context.Request.MaximumImageDimension > Limits::MaximumDimension)
				return context.Fail(
					Status::InvalidValue,
					"request image dimension budget is outside the supported range",
					"dimension"
				);
			double extent = std::max(width, height), distance = 1;
			if (const auto *original = HeightmapOriginal(context, "dimension")) {
				double control = 1;
				if (!HeightmapMaximum(context, *original, control, "dimension", 0, nullptr, 2)) return false;
				// An unlinked Dimension's Project unit scales before round-to-even.
				if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 1)
					control *= std::max(context.Project.SurfaceWidth, context.Project.SurfaceHeight);
				const double lower = std::floor(control), fraction = control - lower;
				const double rounded =
					lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0));
				extent = std::max(extent, std::max(1., rounded));
			}
			for (const auto &[port, images] : context.ImageArrays)
				if (port == "dimension" && images)
					for (const auto &image : images->Images)
						extent = std::max(extent, double(std::max(image.Width, image.Height)));
			if (const auto *original = HeightmapOriginal(context, "distance")) {
				distance = 0;
				if (!HeightmapMaximum(context, *original, distance, "distance")) return false;
			}
			if (!std::isfinite(extent) || extent > Limits::MaximumDimension ||
				extent > context.Request.MaximumImageDimension)
				return context.Fail(
					Status::LimitExceeded, "heightmap batch dimensions exceed native limits", "dimension"
				);
			bool perspective = false;
			if (const auto *original = HeightmapOriginal(context, "projection")) {
				double ignored = 0;
				if (!HeightmapMaximum(context, *original, ignored, "projection", 0, &perspective))
					return false;
			}
			const float stepsFloat =
				std::sqrt(3.f) * float(extent) * 2 * float(perspective ? std::max(1., distance) : 1.);
			const uint64_t count = std::max<size_t>(1, context.ProcessorCount),
						   workLimit = 64'000'000 / count;
			if (!std::isfinite(stepsFloat) || stepsFloat > float(workLimit / 24))
				return context.Fail(Status::LimitExceeded, "heightmap batch exceeds voxel work", "distance");
			const uint64_t visits = uint64_t(std::ceil(stepsFloat)),
						   pixels = uint64_t(extent) * uint64_t(extent);
			uint64_t validationWork = 0;
			for (const auto port : {"heightmap", "texture", "texture_side", "texture_front"}) {
				uint64_t maximum = 0;
				const auto observe = [&](const Image &image) {
					const auto info = DescribeSurfaceFormat(image.Format);
					if (info && info->FloatingPoint)
						maximum = std::max(maximum, uint64_t(image.Pixels.size() / 2));
				};
				if (const auto *image = context.Input(port)) observe(*image);
				for (const auto &[id, images] : context.ImageArrays)
					if (id == port && images)
						for (const auto &image : images->Images)
							observe(image);
				validationWork += maximum;
			}
			if (validationWork > workLimit || pixels > (workLimit - validationWork) / (400 + 24 * visits))
				return context.Fail(Status::LimitExceeded, "heightmap batch exceeds voxel work", "dimension");
			// Three output attachments, with the widest supported format, are admitted
			// before the first row allocates. Later rows cannot hide larger dimensions.
			const uint64_t rowBytes =
				3 * (pixels * 16 + sizeof(std::pair<std::string, Image>) + std::string{}.capacity());
			if (pixels > Limits::MaximumOutputBytes / 16 || rowBytes > context.AvailableBytes() / count)
				return context.Fail(
					Status::LimitExceeded, "heightmap batch exceeds output storage", "surface_out"
				);
			return true;
		}
		bool HeightmapVector(NodeContext &context, std::string_view id, Vector3 &result, size_t width = 3) {
			const auto *value = context.Find(id);
			if (!value) return true;
			if (std::holds_alternative<Path2D>(*value) || std::holds_alternative<PathValue3D>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "source heightmap uniform cannot consume a path object", id
				);
			if (auto *v = std::get_if<Vector3>(value))
				result = *v;
			else if (auto *v = std::get_if<Vector2>(value))
				result = {v->X, v->Y, 0};
			else if (auto *v = std::get_if<Vector4>(value))
				result = {v->X, v->Y, v->Z};
			else if (auto *v = std::get_if<Quaternion>(value))
				result = {v->X, v->Y, v->Z};
			else if (auto *array = std::get_if<ArrayValue>(value)) {
				if (!ValidRuntimeValue(*value) || !array->Nested.empty())
					return context.Fail(
						Status::UnsupportedExecution, "heightmap camera requires selected numeric rows", id
					);
				result = {};
				double *components[] = {&result.X, &result.Y, &result.Z};
				for (size_t i = 0;
					 i < std::min<size_t>(
							 width, array->Items.empty() ? array->Elements.size() : array->Items.size()
						 );
					 ++i) {
					const auto *leaf = array->Items.empty()
										   ? &array->Elements[i]
										   : std::get_if<ElementValue>(&array->Items[i].Data);
					if (!leaf)
						return context.Fail(
							Status::UnsupportedExecution,
							"heightmap uniform needs a selected numeric tuple",
							id
						);
					const auto number = std::visit(
						[](const auto &leaf) -> std::optional<double> {
							using T = std::decay_t<decltype(leaf)>;
							if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
										  std::is_same_v<T, bool>)
								return double(leaf);
							else if constexpr (std::is_same_v<T, EnumValue>)
								return double(leaf.Value);
							else
								return std::nullopt;
						},
						*leaf
					);
					if (!number)
						return context.Fail(
							Status::TypeMismatch, "heightmap camera coordinate must be numeric", id
						);
					*components[i] = *number;
				}
			} else {
				const auto number = SourceChoiceNumber(*value);
				if (!number)
					return context.Fail(
						Status::TypeMismatch, "heightmap camera requires numeric coordinates", id
					);
				result = {*number, *number, *number};
			}
			return true;
		}
		bool HeightmapDimension(NodeContext &context, uint32_t &width, uint32_t &height) {
			Vector3 dimension{1, 1, 0};
			if (!HeightmapVector(context, "dimension", dimension, 2)) return false;
			const int64_t unit = context.IsLinked("dimension") ? 0 : context.Integer("dimension_unit", 1);
			if (unit == 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"source heightmap Mask dimension has no configured mask input",
					"dimension_unit"
				);
			if (unit < 0 || unit > 2)
				return context.Fail(Status::InvalidValue, "heightmap dimension unit is invalid", "dimension");
			if (!context.IsLinked("dimension") && unit == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
			if (!std::isfinite(dimension.X) || !std::isfinite(dimension.Y))
				return context.Fail(Status::InvalidValue, "heightmap dimensions must be finite", "dimension");
			const auto roundEven = [](double v) {
				const double lower = std::floor(v), fraction = v - lower;
				return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0));
			};
			const double x = std::max(1., roundEven(dimension.X)), y = std::max(1., roundEven(dimension.Y));
			if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "heightmap dimensions exceed native limits", "dimension"
				);
			width = uint32_t(x);
			height = uint32_t(y);
			return true;
		}
		Rgba HeightmapTexture(
			const Image &image, float u, float v, const SamplerSettings &sampler, Vector2 dimension
		) {
			const bool inside = u >= 0 && u <= 1 && v >= 0 && v <= 1;
			if (!inside) {
				switch (sampler.Oversample) {
				case 2:
					return {0, 0, 0, 1};
				case 3:
					u = std::clamp(u, 0.f, 1.f);
					v = std::clamp(v, 0.f, 1.f);
					break;
				case 4:
					u = ProjectionFract(u);
					v = ProjectionFract(v);
					break;
				case 6:
				case 7:
					if (v < 0 || v > 1) return sampler.Oversample == 7 ? Rgba{0, 0, 0, 1} : Rgba{};
					u = ProjectionFract(u);
					break;
				case 8:
					u = ProjectionFract(u);
					v = std::clamp(v, 0.f, 1.f);
					break;
				case 10:
				case 11:
					if (u < 0 || u > 1) return sampler.Oversample == 11 ? Rgba{0, 0, 0, 1} : Rgba{};
					v = ProjectionFract(v);
					break;
				case 12:
					u = std::clamp(u, 0.f, 1.f);
					v = ProjectionFract(v);
					break;
				default:
					return {};
				}
			}
			if (sampler.Interpolation == 4) {
				const double centreU = u - (Fract(u * dimension.X) - .5) / dimension.X;
				const double centreV = v - (Fract(v * dimension.Y) - .5) / dimension.Y;
				const double dx = (u - centreU) * dimension.X, dy = (v - centreV) * dimension.Y;
				// Check the shader's weight divisions before the common sampler narrows texel coordinates.
				for (int x = -1; x <= 1; ++x)
					for (int y = -1; y <= 1; ++y) {
						const double wx = LanczosWeight(x * 2 - 1 - dx, 3.) + LanczosWeight(x * 2 - dx, 3.);
						const double wy = LanczosWeight(y * 2 - 1 - dy, 3.) + LanczosWeight(y * 2 - dy, 3.);
						if (wx == 0 || wy == 0 || !std::isfinite(wx) || !std::isfinite(wy)) {
							const double invalid = std::numeric_limits<double>::quiet_NaN();
							return {invalid, invalid, invalid, invalid};
						}
					}
			}
			return TextureInterpolated(image, u, v, sampler, dimension);
		}
		bool HeightmapProjection(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.heightmap_projection");
			const Image *heightmap = context.Input("heightmap"), *texture = context.Input("texture"),
						*side = context.Input("texture_side"), *front = context.Input("texture_front");
			if (!heightmap)
				return context.Fail(
					Status::TypeMismatch, "heightmap projection requires a height surface", "heightmap"
				);
			if (!texture) texture = heightmap;
			for (const Image *image : {heightmap, texture, side, front})
				if (image &&
					!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
					return context.Fail(
						Status::InvalidValue, "heightmap sampler layout is invalid", "heightmap"
					);
			uint32_t width = 0, height = 0;
			if (!HeightmapDimension(context, width, height) ||
				!HeightmapBatchAdmission(context, width, height))
				return false;
			for (const Image *image : {heightmap, texture, side, front})
				if (image && !FiniteSurfaceSamples(*image))
					return context.Fail(Status::InvalidValue, "heightmap sampler is nonfinite", "heightmap");
			if (context.Input("height_color"))
				return context.Fail(
					Status::UnsupportedExecution,
					"source heightmap Surface color leaves gradient uniforms unobserved",
					"height_color"
				);
			const Value *colourInput = context.Find("height_color");
			const auto *gradient = colourInput ? std::get_if<Gradient>(colourInput) : nullptr;
			if ((colourInput && !gradient) ||
				(gradient && (gradient->Keys.empty() || gradient->Keys.size() > 64 || gradient->Mode > 6 ||
							  !ValidPayload(*gradient, false))))
				return context.Fail(
					Status::UnsupportedExecution,
					"heightmap color needs a defined gradient of at most 64 keys",
					"height_color"
				);
			if (gradient)
				for (const auto &key : gradient->Keys)
					if (!std::isfinite(float(key.Time)))
						return context.Fail(
							Status::UnsupportedExecution,
							"heightmap gradient time exceeds shader precision",
							"height_color"
						);
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			const auto sampler = ReadSampler(context);
			// This shader uses the ordinary sampler region, where CleanEdge falls back to plain nearest
			// reads.
			const bool filtered = Filtered(sampler), tiled = context.Boolean("tiled"),
					   normalize = context.Boolean("normalize_height");
			const int64_t projection = context.Integer("projection", 1);
			if (projection != 0 && projection != 1)
				return context.Fail(
					Status::UnsupportedExecution, "source heightmap ray is uninitialized", "projection"
				);
			const float fov = float(context.Scalar("fov", 60)),
						distance = float(context.Scalar("distance", 1)),
						scale = float(context.Scalar("scale", 3.46)),
						shift = float(context.Scalar("shift", 0));
			Vector3 heightPair{0, 1, 0}, depthPair{0, 1, 0}, angleValue{30, 45, 0}, positionValue{};
			if (!HeightmapVector(context, "height_range", heightPair, 2) ||
				!HeightmapVector(context, "depth_range", depthPair, 2) ||
				!HeightmapVector(context, "view_angle", angleValue) ||
				!HeightmapVector(context, "position", positionValue))
				return false;
			ProjectionVector angle{float(angleValue.X), float(angleValue.Y), float(angleValue.Z)},
				position{float(positionValue.X), float(positionValue.Y), float(positionValue.Z)};
			for (float value :
				 {fov,
				  distance,
				  scale,
				  shift,
				  float(heightPair.X),
				  float(heightPair.Y),
				  float(depthPair.X),
				  float(depthPair.Y)})
				if (!std::isfinite(value))
					return context.Fail(
						Status::UnsupportedExecution,
						"heightmap uniforms exceed finite shader precision",
						"surface_out"
					);
			ProjectionMatrix inverse;
			if (!ProjectionFinite(angle) || !ProjectionFinite(position) ||
				!ProjectionInverseRotation(angle, inverse))
				return context.Fail(
					Status::UnsupportedExecution, "heightmap camera exceeds finite math", "view_angle"
				);
			const float aspect = float(width) / height, size = float(std::max(width, height)),
						voxelSize = 2 / size,
						maxVoxels = std::sqrt(3.f) * size * 2 * (projection == 0 ? distance : 1);
			ProjectionVector eye{}, direction{};
			if (!std::isfinite(maxVoxels) ||
				!ProjectionRay(
					inverse, position, .5, .5, aspect, int(projection), fov, distance, scale, eye, direction
				))
				return context.Fail(Status::UnsupportedExecution, "heightmap camera ray is undefined", "fov");
			if (context.FailureCode != Status::Ok) return false;
			const auto layout = CheckedSurfaceLayout(width, height, *format, Limits::MaximumOutputBytes);
			if (!layout ||
				!context.ReserveOutput(3 * (layout->Bytes + std::string{}.capacity()), "surface_out"))
				return false;
			Image *colour = context.NewImage("surface_out", width, height, *format),
				  *depth = context.NewImage("depth_pass", width, height, *format),
				  *normal = context.NewImage("normal_pass", width, height, *format);
			if (!colour || !depth || !normal) {
				context.ClearOutputs();
				return false;
			}
			const auto fail = [&](std::string_view message, std::string_view port) {
				context.ClearOutputs();
				return context.Fail(Status::UnsupportedExecution, std::string(message), port);
			};
			const Vector2 sampleDimension{double(heightmap->Width), double(heightmap->Height)};
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					if (!ProjectionRay(
							inverse,
							position,
							(float(x) + .5f) / width,
							(float(y) + .5f) / height,
							aspect,
							int(projection),
							fov,
							distance,
							scale,
							eye,
							direction
						))
						return fail("heightmap pixel ray is undefined", "view_angle");
					for (float &v : direction)
						if (std::abs(v) < .001f) v = .001f;
					ProjectionVector origin{}, cell{}, reciprocal{}, sign{}, dis{}, mask{};
					for (size_t c = 0; c < 3; ++c) {
						origin[c] = eye[c] / voxelSize;
						cell[c] = std::floor(origin[c]);
						reciprocal[c] = 1 / direction[c];
						sign[c] = direction[c] > 0 ? 1 : -1;
						dis[c] = (cell[c] - origin[c] + .5f + sign[c] * .5f) * reciprocal[c];
					}
					if (!ProjectionFinite(origin) || !ProjectionFinite(dis))
						return fail("heightmap voxel setup is nonfinite", "position");
					bool hit = false;
					float sampledHeight = 0;
					for (uint64_t i = 0; float(i) < maxVoxels; ++i) {
						ProjectionVector sample{};
						for (size_t c = 0; c < 3; ++c)
							sample[c] = (cell[c] + .5f) * voxelSize * .5f + .5f;
						if (tiled) {
							sample[0] = ProjectionFract(sample[0]);
							sample[2] = ProjectionFract(sample[2]);
						}
						if (sample[0] >= 0 && sample[0] < 1 && sample[1] >= 0 && sample[1] < 1 &&
							sample[2] >= 0 && sample[2] < 1) {
							const auto heightSample =
								HeightmapSample(*heightmap, sample[0], sample[2], filtered);
							sampledHeight = ((1 - heightSample[0]) - float(heightPair.X)) /
											(float(heightPair.Y) - float(heightPair.X));
							if (!std::isfinite(sampledHeight))
								return fail(
									"heightmap occupancy divides by zero or is nonfinite", "height_range"
								);
							if (sample[1] > sampledHeight) {
								hit = true;
								break;
							}
						}
						for (size_t c = 0; c < 3; ++c)
							mask[c] = dis[c] <= dis[(c + 1) % 3] && dis[c] <= dis[(c + 2) % 3] ? 1 : 0;
						for (size_t c = 0; c < 3; ++c) {
							dis[c] += mask[c] * sign[c] * reciprocal[c];
							cell[c] += mask[c] * sign[c];
						}
						if (!ProjectionFinite(cell) || !ProjectionFinite(dis))
							return fail("heightmap traversal is nonfinite", "position");
					}
					if (!hit) continue;
					ProjectionVector near{}, hitPosition{}, samplePosition{};
					for (size_t c = 0; c < 3; ++c)
						near[c] = (cell[c] - origin[c] + .5f - .5f * sign[c]) * reciprocal[c];
					const float t = std::max(near[0], std::max(near[1], near[2]));
					float squared = 0;
					for (size_t c = 0; c < 3; ++c) {
						hitPosition[c] = (origin[c] + direction[c] * t) * voxelSize;
						samplePosition[c] = hitPosition[c] * .5f + .5f;
						const float d = eye[c] - hitPosition[c];
						squared += d * d;
					}
					if (!ProjectionFinite(samplePosition))
						return fail("heightmap entry position is nonfinite", "position");
					auto fragment = HeightmapTexture(
						*texture, samplePosition[0], samplePosition[2], sampler, sampleDimension
					);
					if (side && mask[2] > .5f &&
						(samplePosition[2] <= voxelSize / 2 || samplePosition[2] >= 1 - voxelSize / 2))
						fragment = HeightmapTexture(
							*side, samplePosition[0], samplePosition[1], sampler, sampleDimension
						);
					else if (front && mask[0] > .5f &&
							 (samplePosition[0] <= voxelSize / 2 || samplePosition[0] >= 1 - voxelSize / 2))
						fragment = HeightmapTexture(
							*front, 1 - samplePosition[2], samplePosition[1], sampler, sampleDimension
						);
					float progress =
						normalize ? (1 - samplePosition[1]) / (1 - sampledHeight) : 1 - samplePosition[1];
					progress = ProjectionFract(ProjectionFract(progress + shift) + 1);
					if (!std::isfinite(progress))
						return fail(
							"heightmap normalized gradient coordinate is undefined", "normalize_height"
						);
					const auto tint =
						gradient ? ProjectionGradient(*gradient, progress) : ProjectionColour{1, 1, 1, 1};
					for (size_t c = 0; c < 4; ++c)
						fragment[c] *= tint[c];
					float z = std::sqrt(squared) / scale;
					z = (z - float(depthPair.X)) / (float(depthPair.Y) - float(depthPair.X));
					if (!std::isfinite(z))
						return fail("heightmap depth divides by zero or is nonfinite", "depth_range");
					const bool fixed = *format == SurfaceFormat::RGBA4Unorm ||
									   *format == SurfaceFormat::RGBA8Unorm ||
									   *format == SurfaceFormat::R8Unorm;
					for (double &c : fragment) {
						if (!std::isfinite(c))
							return fail("heightmap sample or gradient is undefined", "height_color");
						if (fixed) c = std::clamp(c, 0., 1.);
					}
					const double alpha = fragment[3];
					for (double &c : fragment)
						c *= alpha;
					if (!WritePixel(*colour, x, y, fragment) || !WritePixel(*depth, x, y, {z, z, z, 1}) ||
						!WritePixel(*normal, x, y, {mask[0], mask[1], mask[2], 1}))
						return fail("heightmap sample exceeds output storage", "surface_out");
				}
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceHeightmapProjectionExecutors() {
		static const ExecutorEntry entries[] = {{"pc.heightmap_project_3_d", HeightmapProjection, true}};
		return entries;
	}
}
