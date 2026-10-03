#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceProjectionMath.hpp"

namespace engine::imagegraph::detail {
	namespace {
		using CylinderPixel = std::array<float, 4>;
		CylinderPixel CylinderSample(const Image &image, float u, float v, bool filtered) {
			auto sample = Texture(image, u, v, filtered);
			return {float(sample[0]), float(sample[1]), float(sample[2]), float(sample[3])};
		}
		const Value *CylinderOriginal(const NodeContext &context, std::string_view id) {
			for (auto p = context.ProcessorOriginalValues.rbegin();
				 p != context.ProcessorOriginalValues.rend();
				 ++p)
				if (p->first == id) return p->second;
			for (const auto &[port, value] : context.Values)
				if (port == id) return &value;
			return context.Find(id);
		}
		template <class V>
		bool CylinderMaximum(
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
					Status::InvalidValue, "cylinder original control exceeds admitted payload", id
				);
			if (depth > Limits::MaximumArrayDepth)
				return context.Fail(Status::LimitExceeded, "cylinder control exceeds admitted depth", id);
			return std::visit(
				[&](const auto &data) -> bool {
					using T = std::decay_t<decltype(data)>;
					const auto number = [&](double v) {
						if (!std::isfinite(v))
							return context.Fail(
								Status::UnsupportedExecution, "cylinder control is nonfinite", id
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
							if (!CylinderMaximum(
									context, data.Elements[i], maximum, id, depth + 1, zero, tupleWidth
								))
								return false;
						for (const auto &row : data.Nested)
							for (size_t i = 0;
								 i < (tupleWidth ? std::min(tupleWidth, row.size()) : row.size());
								 ++i)
								if (!CylinderMaximum(
										context, row[i], maximum, id, depth + 2, zero, tupleWidth
									))
									return false;
						const auto item =
							[&](const auto &self, const SourceArrayItem &entry, size_t level) -> bool {
							if (level > Limits::MaximumArrayDepth)
								return context.Fail(
									Status::LimitExceeded, "cylinder control exceeds admitted depth", id
								);
							if (auto *leaf = std::get_if<ElementValue>(&entry.Data))
								return CylinderMaximum(context, *leaf, maximum, id, level, zero, tupleWidth);
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
							Status::UnsupportedExecution, "cylinder control needs numeric rows", id
						);
				},
				value
			);
		}
		bool CylinderBatchAdmission(NodeContext &context, uint32_t width, uint32_t height) {
			if (context.ProcessorRow != 0) return true;
			if (context.Request.MaximumImageDimension == 0 ||
				context.Request.MaximumImageDimension > Limits::MaximumDimension)
				return context.Fail(
					Status::InvalidValue,
					"request image dimension budget is outside the supported range",
					"dimension"
				);
			double extent = std::max(width, height), distance = 2;
			if (const auto *original = CylinderOriginal(context, "dimension")) {
				double control = 1;
				if (!CylinderMaximum(context, *original, control, "dimension", 0, nullptr, 2)) return false;
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
			if (const auto *original = CylinderOriginal(context, "distance")) {
				distance = 0;
				if (!CylinderMaximum(context, *original, distance, "distance")) return false;
			}
			if (!std::isfinite(extent) || extent > Limits::MaximumDimension ||
				extent > context.Request.MaximumImageDimension)
				return context.Fail(
					Status::LimitExceeded, "cylinder batch dimensions exceed native limits", "dimension"
				);
			bool perspective = false;
			if (const auto *original = CylinderOriginal(context, "projection")) {
				double ignored = 0;
				if (!CylinderMaximum(context, *original, ignored, "projection", 0, &perspective))
					return false;
			}
			const float stepsFloat =
				std::sqrt(3.f) * float(extent) * 2 * float(perspective ? std::max(1., distance) : 1.);
			const uint64_t count = std::max<size_t>(1, context.ProcessorCount),
						   workLimit = 64'000'000 / count;
			if (!std::isfinite(stepsFloat) || stepsFloat > float(workLimit / 24))
				return context.Fail(Status::LimitExceeded, "cylinder batch exceeds voxel work", "distance");
			const uint64_t visits = uint64_t(std::ceil(stepsFloat)),
						   pixels = uint64_t(extent) * uint64_t(extent);
			uint64_t validationWork = 0;
			for (const auto port : {"cylinder", "top"}) {
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
			if (validationWork > workLimit || pixels > (workLimit - validationWork) / (80 + 24 * visits))
				return context.Fail(Status::LimitExceeded, "cylinder batch exceeds voxel work", "dimension");
			// Three output attachments, with the widest supported format, are admitted
			// before the first row allocates. Later rows cannot hide larger dimensions.
			const uint64_t rowBytes =
				3 * (pixels * 16 + sizeof(std::pair<std::string, Image>) + std::string{}.capacity());
			if (pixels > Limits::MaximumOutputBytes / 16 || rowBytes > context.AvailableBytes() / count)
				return context.Fail(
					Status::LimitExceeded, "cylinder batch exceeds output storage", "surface_out"
				);
			return true;
		}
		bool CylinderVector(NodeContext &context, std::string_view id, Vector3 &result, size_t width = 3) {
			const auto *value = context.Find(id);
			if (!value) return true;
			if (std::holds_alternative<Path2D>(*value) || std::holds_alternative<PathValue3D>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "source cylinder uniform cannot consume a path object", id
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
						Status::UnsupportedExecution, "cylinder camera requires selected numeric rows", id
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
							"cylinder uniform needs a selected numeric tuple",
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
							Status::TypeMismatch, "cylinder camera coordinate must be numeric", id
						);
					*components[i] = *number;
				}
			} else {
				const auto number = SourceChoiceNumber(*value);
				if (!number)
					return context.Fail(
						Status::TypeMismatch, "cylinder camera requires numeric coordinates", id
					);
				result = {*number, *number, *number};
			}
			return true;
		}
		bool CylinderDimension(NodeContext &context, uint32_t &width, uint32_t &height) {
			Vector3 dimension{1, 1, 0};
			if (!CylinderVector(context, "dimension", dimension, 2)) return false;
			const int64_t unit = context.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(Status::InvalidValue, "cylinder dimension unit is invalid", "dimension");
			if (!context.IsLinked("dimension") && unit == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
			if (!std::isfinite(dimension.X) || !std::isfinite(dimension.Y))
				return context.Fail(Status::InvalidValue, "cylinder dimensions must be finite", "dimension");
			const auto roundEven = [](double v) {
				const double lower = std::floor(v), fraction = v - lower;
				return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0));
			};
			const double x = std::max(1., roundEven(dimension.X)), y = std::max(1., roundEven(dimension.Y));
			if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "cylinder dimensions exceed native limits", "dimension"
				);
			width = uint32_t(x);
			height = uint32_t(y);
			return true;
		}
		bool CylinderProjection(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.cylinder_projection");
			const Image *profile = context.Input("cylinder"), *top = context.Input("top");
			if (!profile)
				return context.Fail(
					Status::TypeMismatch, "cylinder projection requires its profile surface", "cylinder"
				);
			for (const Image *image : {profile, top})
				if (image &&
					(!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes)))
					return context.Fail(
						Status::InvalidValue,
						"cylinder sampler surface is invalid",
						image == profile ? "cylinder" : "top"
					);
			uint32_t width = 0, height = 0;
			if (!CylinderDimension(context, width, height) || !CylinderBatchAdmission(context, width, height))
				return false;
			for (const Image *image : {profile, top})
				if (image && !FiniteSurfaceSamples(*image))
					return context.Fail(
						Status::InvalidValue,
						"cylinder sampler is nonfinite",
						image == profile ? "cylinder" : "top"
					);
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			const auto sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			const bool filtered = Filtered(sampler), fromCenter = context.Boolean("from_center");
			const int64_t projection = context.Integer("projection", 1);
			if (projection != 0 && projection != 1)
				return context.Fail(
					Status::UnsupportedExecution,
					"source cylinder leaves camera ray uninitialized",
					"projection"
				);
			const float fov = float(context.Scalar("fov", 60)),
						distance = float(context.Scalar("distance", 2)),
						scale = float(context.Scalar("scale", 3.46));
			Vector3 anglePair{0, 360, 0}, depthPair{0, 1, 0};
			if (!CylinderVector(context, "angle_range", anglePair, 2) ||
				!CylinderVector(context, "depth_range", depthPair, 2))
				return false;
			const Vector2 angleRange{anglePair.X, anglePair.Y}, depthRange{depthPair.X, depthPair.Y};
			Vector3 angleValue{30, 45, 0}, positionValue{};
			if (!CylinderVector(context, "view_angle", angleValue) ||
				!CylinderVector(context, "position", positionValue))
				return false;
			ProjectionVector angle{float(angleValue.X), float(angleValue.Y), float(angleValue.Z)},
				position{float(positionValue.X), float(positionValue.Y), float(positionValue.Z)};
			for (float v :
				 {fov,
				  distance,
				  scale,
				  float(angleRange.X),
				  float(angleRange.Y),
				  float(depthRange.X),
				  float(depthRange.Y)})
				if (!std::isfinite(v))
					return context.Fail(
						Status::UnsupportedExecution,
						"cylinder controls exceed finite shader uniforms",
						"surface_out"
					);
			ProjectionMatrix inverse;
			if (!ProjectionFinite(angle) || !ProjectionFinite(position) ||
				!ProjectionInverseRotation(angle, inverse))
				return context.Fail(
					Status::UnsupportedExecution, "cylinder camera exceeds finite shader math", "view_angle"
				);
			ProjectionVector eye, direction;
			const float aspect = float(width) / height, size = float(std::max(width, height)),
						voxelSize = 2 / size;
			const float maxVoxels = std::sqrt(3.f) * size * 2 * (projection == 0 ? distance : 1);
			if (!std::isfinite(maxVoxels))
				return context.Fail(
					Status::UnsupportedExecution, "cylinder traversal is nonfinite", "distance"
				);
			if (!ProjectionRay(
					inverse, position, .5, .5, aspect, int(projection), fov, distance, scale, eye, direction
				))
				return context.Fail(Status::UnsupportedExecution, "source cylinder ray is undefined", "fov");
			if (context.FailureCode != Status::Ok) return false;
			const auto layout = CheckedSurfaceLayout(width, height, *format, Limits::MaximumOutputBytes);
			const uint64_t storage =
				layout ? 3 * (layout->Bytes + std::string{}.capacity()) : Limits::MaximumEvaluationBytes + 1;
			if (!layout || !context.ReserveOutput(storage, "surface_out")) return false;
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
			const float minimumAngle = float(angleRange.X) * (float(std::numbers::pi) / 180),
						maximumAngle = float(angleRange.Y) * (float(std::numbers::pi) / 180);
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
						return fail("source cylinder pixel ray is undefined", "view_angle");
					for (auto &v : direction)
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
						return fail("source cylinder voxel setup is nonfinite", "position");
					bool hit = false;
					float hitU = 0, hitV = 0;
					for (uint64_t i = 0; float(i) < maxVoxels; ++i) {
						ProjectionVector sample{};
						for (size_t c = 0; c < 3; ++c)
							sample[c] = (cell[c] + .5f) * voxelSize * .5f + .5f;
						const float radialX = sample[0] - .5f, radialZ = sample[2] - .5f;
						const float azimuth = std::atan2(radialX, radialZ) + float(std::numbers::pi);
						float radius = std::sqrt(radialX * radialX + radialZ * radialZ);
						radius = fromCenter ? radius + .5f : radius * 2;
						if (azimuth >= minimumAngle && azimuth <= maximumAngle && radius >= 0 && radius < 1 &&
							sample[1] >= 0 && sample[1] < 1) {
							const auto p = CylinderSample(*profile, radius, sample[1], filtered);
							hit = p[3] != 0;
							if (hit && top)
								hit = CylinderSample(*top, sample[0], sample[2], filtered)[3] != 0;
							if (hit) {
								hitU = radius;
								hitV = sample[1];
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
							return fail("source cylinder traversal is nonfinite", "position");
					}
					if (!hit) continue;
					ProjectionVector near{};
					for (size_t c = 0; c < 3; ++c)
						near[c] = (cell[c] - origin[c] + .5f - .5f * sign[c]) * reciprocal[c];
					const float t = std::max(near[0], std::max(near[1], near[2]));
					ProjectionVector hitPosition{};
					float squared = 0;
					for (size_t c = 0; c < 3; ++c) {
						hitPosition[c] = (origin[c] + direction[c] * t) * voxelSize;
						const float d = eye[c] - hitPosition[c];
						squared += d * d;
					}
					float z = std::sqrt(squared) / scale;
					z = (z - float(depthRange.X)) / (float(depthRange.Y) - float(depthRange.X));
					if (!ProjectionFinite(hitPosition) || !std::isfinite(z))
						return fail(
							"source cylinder hit divides by zero or exceeds finite depth", "depth_range"
						);
					auto fragment = CylinderSample(*profile, hitU, hitV, filtered);
					if (top) {
						const auto tint = CylinderSample(
							*top, hitPosition[0] * .5f + .5f, hitPosition[2] * .5f + .5f, filtered
						);
						if (tint[3] > 0)
							for (size_t c = 0; c < 4; ++c)
								fragment[c] *= tint[c];
					}
					const bool fixed = *format == SurfaceFormat::RGBA8Unorm ||
									   *format == SurfaceFormat::RGBA4Unorm ||
									   *format == SurfaceFormat::R8Unorm;
					Rgba colourValue{}, depthValue{z, z, z, 1}, normalValue{mask[0], mask[1], mask[2], 1};
					for (size_t c = 0; c < 4; ++c)
						colourValue[c] = fixed ? std::clamp(double(fragment[c]), 0., 1.) : fragment[c];
					const double alpha = colourValue[3];
					for (double &v : colourValue)
						v *= alpha;
					if (!WritePixel(*colour, x, y, colourValue) || !WritePixel(*depth, x, y, depthValue) ||
						!WritePixel(*normal, x, y, normalValue))
						return fail("source cylinder sample exceeds output storage", "surface_out");
				}
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceCylinderProjectionExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.surface_project_cylinder_3_d", CylinderProjection, true}
		};
		return entries;
	}
}
