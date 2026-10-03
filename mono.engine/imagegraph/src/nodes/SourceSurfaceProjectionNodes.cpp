#include "Families.hpp"
#include "Source2DGenerator.hpp"
#include "SourceInterpret.hpp"
#include "SourceSurfaceProjectionKernel.hpp"

namespace engine::imagegraph::detail {
	namespace {
		const Value *SurfaceProjectOriginal(const NodeContext &context, std::string_view id) {
			for (auto p = context.ProcessorOriginalValues.rbegin();
				 p != context.ProcessorOriginalValues.rend();
				 ++p)
				if (p->first == id) return p->second;
			for (const auto &[port, value] : context.Values)
				if (port == id) return &value;
			return context.Find(id);
		}
		template <class V>
		bool SurfaceProjectMaximum(
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
					Status::InvalidValue, "surface projection original control exceeds admitted payload", id
				);
			if (depth > Limits::MaximumArrayDepth)
				return context.Fail(
					Status::LimitExceeded, "surface projection control exceeds admitted depth", id
				);
			return std::visit(
				[&](const auto &data) -> bool {
					using T = std::decay_t<decltype(data)>;
					const auto number = [&](double v) {
						if (!std::isfinite(v))
							return context.Fail(
								Status::UnsupportedExecution, "surface projection control is nonfinite", id
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
							if (!SurfaceProjectMaximum(
									context, data.Elements[i], maximum, id, depth + 1, zero, tupleWidth
								))
								return false;
						for (const auto &row : data.Nested)
							for (size_t i = 0;
								 i < (tupleWidth ? std::min(tupleWidth, row.size()) : row.size());
								 ++i)
								if (!SurfaceProjectMaximum(
										context, row[i], maximum, id, depth + 2, zero, tupleWidth
									))
									return false;
						const auto item =
							[&](const auto &self, const SourceArrayItem &entry, size_t level) -> bool {
							if (level > Limits::MaximumArrayDepth)
								return context.Fail(
									Status::LimitExceeded,
									"surface projection control exceeds admitted depth",
									id
								);
							if (auto *leaf = std::get_if<ElementValue>(&entry.Data))
								return SurfaceProjectMaximum(
									context, *leaf, maximum, id, level, zero, tupleWidth
								);
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
							Status::UnsupportedExecution, "surface projection control needs numeric rows", id
						);
				},
				value
			);
		}
		bool
		SurfaceProjectVector(NodeContext &context, std::string_view id, Vector3 &result, size_t width = 3) {
			const auto *value = context.Find(id);
			if (!value) return true;
			if (std::holds_alternative<Path2D>(*value) || std::holds_alternative<PathValue3D>(*value))
				return context.Fail(
					Status::UnsupportedExecution,
					"source surface projection uniform cannot consume a path object",
					id
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
						Status::UnsupportedExecution,
						"surface projection camera requires selected numeric rows",
						id
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
							"surface projection uniform needs a selected numeric tuple",
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
							Status::TypeMismatch, "surface projection camera coordinate must be numeric", id
						);
					*components[i] = *number;
				}
			} else {
				const auto number = SourceChoiceNumber(*value);
				if (!number)
					return context.Fail(
						Status::TypeMismatch, "surface projection camera requires numeric coordinates", id
					);
				result = {*number, *number, *number};
			}
			return true;
		}
		bool SurfaceProjectDimension(NodeContext &context, uint32_t &width, uint32_t &height) {
			Vector3 dimension{1, 1, 0};
			if (!SurfaceProjectVector(context, "dimension", dimension, 2)) return false;
			const int64_t unit = context.IsLinked("dimension") ? 0 : context.Integer("dimension_unit", 1);
			if (unit == 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"source surface projection Mask dimension has no configured mask input",
					"dimension_unit"
				);
			if (unit < 0 || unit > 2)
				return context.Fail(
					Status::InvalidValue, "surface projection dimension unit is invalid", "dimension"
				);
			if (!context.IsLinked("dimension") && unit == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
			if (!std::isfinite(dimension.X) || !std::isfinite(dimension.Y))
				return context.Fail(
					Status::InvalidValue, "surface projection dimensions must be finite", "dimension"
				);
			const auto roundEven = [](double v) {
				const double lower = std::floor(v), fraction = v - lower;
				return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0));
			};
			const double x = std::max(1., roundEven(dimension.X)), y = std::max(1., roundEven(dimension.Y));
			if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "surface projection dimensions exceed native limits", "dimension"
				);
			width = uint32_t(x);
			height = uint32_t(y);
			return true;
		}
		bool SurfaceProjectAdmission(
			NodeContext &context, uint32_t width, uint32_t height, uint32_t atlasWidth, uint32_t atlasHeight
		) {
			if (context.ProcessorRow != 0) return true;
			source2d::GeneratorDimensionBounds units;
			if (!context.IsLinked("dimension")) {
				if (const Value *original = SurfaceProjectOriginal(context, "dimension_unit");
					original && !ValidRuntimeValue(*original))
					return context.Fail(
						Status::InvalidValue,
						"surface projection unit rows exceed admitted payload",
						"dimension_unit"
					);
				if (!source2d::GeneratorDimensionBoundsFor(context, "dimension_unit", units)) return false;
				if (!units.Units) {
					const int64_t unit = context.Integer("dimension_unit", 1);
					if (unit < 0 || unit > 2)
						return context.Fail(
							Status::InvalidValue,
							"surface projection dimension unit is invalid",
							"dimension_unit"
						);
					units.Units = uint8_t(1u << unit);
				}
				if (units.Units & 4u)
					return context.Fail(
						Status::UnsupportedExecution,
						"source surface projection Mask dimension has no configured mask input",
						"dimension_unit"
					);
			}
			double extent = std::max(width, height), distance = 2;
			if (const auto *v = SurfaceProjectOriginal(context, "dimension")) {
				double original = 1;
				if (!SurfaceProjectMaximum(context, *v, original, "dimension", 0, nullptr, 2)) return false;
				// Original unit choices bound every scheduled pairing before row zero allocates.
				if (units.Units & 2u)
					original = std::max(
						original,
						original * std::max(context.Project.SurfaceWidth, context.Project.SurfaceHeight)
					);
				const double lower = std::floor(original), fraction = original - lower;
				extent = std::max(
					extent,
					std::max(1., lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0)))
				);
			}
			if (const auto *v = SurfaceProjectOriginal(context, "distance"))
				if (!SurfaceProjectMaximum(context, *v, distance, "distance")) return false;
			bool perspective = false;
			if (const auto *v = SurfaceProjectOriginal(context, "projection")) {
				double ignored = 0;
				if (!SurfaceProjectMaximum(context, *v, ignored, "projection", 0, &perspective)) return false;
			}
			uint64_t inputWork = 0;
			const auto observe = [&](std::string_view port, const Image &image) {
				if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
					return context.Fail(
						Status::InvalidValue, "surface projection face has invalid owned layout", port
					);
				if (image.Pixels.size() > 64'000'000 - std::min<uint64_t>(inputWork, 64'000'000))
					return context.Fail(
						Status::LimitExceeded,
						"surface projection input validation exceeds bounded work",
						port
					);
				inputWork += image.Pixels.size();
				if (port == "top" || port == "front" || port == "right") {
					atlasWidth = std::max(atlasWidth, image.Width);
					atlasHeight = std::max(atlasHeight, image.Height);
				}
				return true;
			};
			for (std::string_view port : {"top", "front", "right", "bottom", "back", "left"}) {
				bool originalArray = false;
				for (const auto &[id, arrays] : context.ImageArrays)
					if (id == port && arrays) {
						originalArray = true;
						for (const auto &image : arrays->Images)
							if (!observe(port, image)) return false;
					}
				if (!originalArray)
					if (const auto *image = context.Input(port); image && !observe(port, *image))
						return false;
			}
			for (const auto &[port, arrays] : context.ImageArrays)
				if (port == "dimension" && arrays)
					for (const auto &image : arrays->Images)
						extent = std::max(extent, double(std::max(image.Width, image.Height)));
			const uint64_t rows = std::max<size_t>(1, context.ProcessorCount), work = 64'000'000 / rows;
			const float steps =
				std::sqrt(3.f) * float(extent) * 2 * float(perspective ? std::max(1., distance) : 1.);
			if (!std::isfinite(extent) || extent > context.Request.MaximumImageDimension ||
				extent > Limits::MaximumDimension || !std::isfinite(steps) || steps > float(work / 48) ||
				inputWork > work)
				return context.Fail(
					Status::LimitExceeded,
					"surface projection batch exceeds bounded dimensions or traversal",
					"dimension"
				);
			const uint64_t pixels = uint64_t(extent) * uint64_t(extent),
						   visits = uint64_t(std::ceil(std::max(0.f, steps)));
			const uint64_t atlasPixels = uint64_t(atlasWidth) * atlasHeight * 9;
			if (atlasWidth > Limits::MaximumDimension / 3 || atlasHeight > Limits::MaximumDimension / 3 ||
				atlasPixels > (work - inputWork) / 64 ||
				pixels > (work - inputWork - atlasPixels * 64) / (400 + visits * 48))
				return context.Fail(
					Status::LimitExceeded,
					"surface projection batch exceeds atlas and voxel work",
					"dimension"
				);
			const uint64_t bytes =
				pixels * 3 * 16 + atlasPixels * 4 + 3 * (sizeof(Image) + sizeof(std::string));
			if (pixels > Limits::MaximumOutputBytes / 16 || bytes > context.AvailableBytes() / rows)
				return context.Fail(
					Status::LimitExceeded,
					"surface projection batch exceeds owned output and atlas bytes",
					"surface_out"
				);
			// Validate every borrowed original before the first temporary or output allocation.
			for (std::string_view port : {"top", "front", "right", "bottom", "back", "left"}) {
				bool originalArray = false;
				for (const auto &[id, arrays] : context.ImageArrays)
					if (id == port && arrays) {
						originalArray = true;
						for (const auto &image : arrays->Images)
							if (!FiniteSurfaceSamples(image))
								return context.Fail(
									Status::InvalidValue,
									"surface projection face has nonfinite samples",
									port
								);
					}
				if (!originalArray)
					if (const auto *image = context.Input(port); image && !FiniteSurfaceSamples(*image))
						return context.Fail(
							Status::InvalidValue, "surface projection face has nonfinite samples", port
						);
			}
			return true;
		}
		bool SurfaceProject(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.surface_project_3d");
			uint32_t width = 0, height = 0;
			if (!SurfaceProjectDimension(context, width, height)) return false;
			const Image *top = context.Input("top"), *front = context.Input("front"),
						*right = context.Input("right");
			const bool hasTop = top, hasFront = front, hasRight = right;
			if (!hasTop && !hasFront && !hasRight)
				return context.Fail(
					Status::UnsupportedExecution,
					"source empty projection retains ambient output attachments",
					"top"
				);
			if (!hasFront) front = hasTop ? top : right;
			if (!hasRight) right = hasFront ? front : top;
			if (!hasTop) top = hasRight ? right : front;
			const Image *bottom = context.Input("bottom"), *back = context.Input("back"),
						*left = context.Input("left");
			const std::array<const Image *, 6> faces{front, right, top, back, left, bottom};
			const uint32_t cellWidth = std::max({front->Width, right->Width, top->Width}),
						   cellHeight = std::max({front->Height, right->Height, top->Height});
			if (!SurfaceProjectAdmission(context, width, height, cellWidth, cellHeight)) return false;
			SurfaceProjectionControls c;
			Vector3 angle{30, 45, 0}, position{}, depth{0, 1, 0};
			if (!SurfaceProjectVector(context, "view_angle", angle) ||
				!SurfaceProjectVector(context, "position", position) ||
				!SurfaceProjectVector(context, "depth_range", depth, 2))
				return false;
			c.DepthRange = {depth.X, depth.Y};
			c.Position = {float(position.X), float(position.Y), float(position.Z)};
			if (!ProjectionInverseRotation({float(angle.X), float(angle.Y), float(angle.Z)}, c.Inverse))
				return context.Fail(
					Status::UnsupportedExecution,
					"surface projection camera transform is nonfinite",
					"view_angle"
				);
			const double projection = context.SourceChoice("projection", 1),
						 blend = context.SourceChoice("voxel_color", 0),
						 except = context.SourceChoice("except", 0);
			if (!std::isfinite(projection) || projection < 0 || projection > 1 ||
				std::trunc(projection) != projection || !std::isfinite(blend) || blend < 0 || blend > 2 ||
				std::trunc(blend) != blend || !std::isfinite(except) || except < 0 || except > 2 ||
				std::trunc(except) != except)
				return context.Fail(
					Status::UnsupportedExecution,
					"surface projection selector has no defined shader case",
					"projection"
				);
			c.Projection = int(projection);
			c.BlendType = int(blend);
			c.Except = int(except);
			c.Fov = float(context.Scalar("fov", 60));
			c.Distance = float(context.Scalar("distance", 2));
			c.Scale = float(context.Scalar("scale", 3.46));
			c.BothSides = context.Boolean("extrude_both_side", true);
			c.Noise = context.Boolean("use_noise", false);
			if (c.Noise && !context.Find("noise_seed"))
				return context.Fail(
					Status::UnsupportedExecution,
					"source noise Seed constructor default is unresolved",
					"noise_seed"
				);
			c.NoiseSeed = c.Noise ? float(context.Scalar("noise_seed", 0)) : 0;
			c.NoiseThreshold = float(context.Scalar("threshold", .5));
			c.Back = {bool(back), bool(left), bool(bottom)};
			if (context.FailureCode != Status::Ok) return false;
			if (c.Projection < 0 || c.Projection > 1 || c.BlendType < 0 || c.BlendType > 2 || c.Except < 0 ||
				c.Except > 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"surface projection source selector has no defined shader case",
					"projection"
				);
			for (double v :
				 {double(c.Fov),
				  double(c.Distance),
				  double(c.Scale),
				  double(c.NoiseSeed),
				  double(c.NoiseThreshold),
				  depth.X,
				  depth.Y})
				if (!std::isfinite(v))
					return context.Fail(
						Status::UnsupportedExecution,
						"surface projection uniform exceeds finite shader precision",
						"scale"
					);
			if (!ProjectionFinite(c.Position) || c.Scale == 0 || !std::isfinite(float(depth.X)) ||
				!std::isfinite(float(depth.Y)) || float(depth.X) == float(depth.Y))
				return context.Fail(
					Status::UnsupportedExecution,
					"surface projection depth divides by zero or camera is nonfinite",
					"depth_range"
				);
			const auto *paletteValue = context.Find("face_blending");
			const auto *palette = paletteValue ? std::get_if<ArrayValue>(paletteValue) : nullptr;
			const auto singleColour =
				paletteValue && !palette
					? std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, *paletteValue)
					: std::optional<Colour>{};
			const size_t paletteSize =
				palette		   ? (palette->Items.empty() ? palette->Elements.size() : palette->Items.size())
				: singleColour ? 1
							   : 0;
			if ((!palette && !singleColour) || !ValidRuntimeValue(*paletteValue) || paletteSize == 0 ||
				paletteSize > 256 || (palette && !palette->Nested.empty()))
				return context.Fail(
					Status::UnsupportedExecution,
					"surface projection palette requires a bounded selected Colour row",
					"face_blending"
				);
			std::array<Colour, 256> colours;
			for (size_t i = 0; i < paletteSize; ++i) {
				const auto *leaf = singleColour ? nullptr
								   : palette->Items.empty()
									   ? &palette->Elements[i]
									   : std::get_if<ElementValue>(&palette->Items[i].Data);
				const auto colour =
					singleColour ? singleColour
					: leaf ? std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, *leaf)
						   : std::optional<Colour>{};
				if (!colour)
					return context.Fail(
						Status::TypeMismatch,
						"surface projection palette needs resolved packed Colours",
						"face_blending"
					);
				colours[i] = *colour;
			}
			c.Palette = {colours.data(), paletteSize};
			const auto sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			c.Filtered = Filtered(sampler);
			const auto atlasLayout = CheckedSurfaceLayout(
				cellWidth * 3, cellHeight * 3, SurfaceFormat::RGBA8Unorm, Limits::MaximumOutputBytes
			);
			if (!atlasLayout)
				return context.Fail(
					Status::LimitExceeded, "surface projection atlas dimensions exceed native limits", "top"
				);
			auto reservation = context.ReserveWorkspace(atlasLayout->Bytes, "surface_projection_atlas");
			if (!reservation) return false;
			Image atlas{cellWidth * 3, cellHeight * 3, std::vector<uint8_t>(atlasLayout->Bytes, 0)};
			if (atlas.Pixels.capacity() > atlasLayout->Bytes) {
				auto excess = context.ReserveWorkspace(
					atlas.Pixels.capacity() - atlasLayout->Bytes, "surface_projection_atlas"
				);
				if (!excess || !reservation->Merge(std::move(*excess))) return false;
			}
			for (size_t face = 0; face < faces.size(); ++face) {
				const auto *image = faces[face];
				if (!image) continue;
				if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
					!FiniteSurfaceSamples(*image))
					return context.Fail(
						Status::InvalidValue, "surface projection face has invalid owned samples", "top"
					);
				for (uint32_t y = 0;
					 y < std::min(atlas.Height - uint32_t(face / 3) * cellHeight, image->Height);
					 ++y)
					for (uint32_t x = 0;
						 x < std::min(atlas.Width - uint32_t(face % 3) * cellWidth, image->Width);
						 ++x) {
						auto pixel = ReadPixel(*image, x, y);
						if (DescribeSurfaceFormat(image->Format)->Channels == 1)
							pixel = {pixel[0], pixel[0], pixel[0], 1};
						const uint32_t targetX = uint32_t(face % 3) * cellWidth + x;
						const uint32_t targetY = uint32_t(face / 3) * cellHeight + y;
						const auto previous = ReadPixel(atlas, targetX, targetY);
						// BLEND_ALPHA uses ONE for source RGB and additive alpha on this atlas.
						for (size_t k = 0; k < 3; ++k)
							pixel[k] = float(pixel[k]) + float(previous[k]) * (1.f - float(pixel[3]));
						pixel[3] = float(pixel[3]) + float(previous[3]);
						if (!WritePixel(
								atlas,
								uint32_t(face % 3) * cellWidth + x,
								uint32_t(face / 3) * cellHeight + y,
								pixel
							))
							return context.Fail(
								Status::InvalidValue, "surface projection atlas conversion failed", "top"
							);
					}
			}
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			Image *colour = context.NewImage("surface_out", width, height, *format),
				  *depthImage = context.NewImage("depth_pass", width, height, *format),
				  *normal = context.NewImage("normal_pass", width, height, *format);
			if (!colour || !depthImage || !normal) {
				context.ClearOutputs();
				return false;
			}
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					SurfaceProjectionFragments fragments;
					if (!SurfaceProjectionPixel(
							c,
							atlas,
							width,
							height,
							(float(x) + .5f) / width,
							(float(y) + .5f) / height,
							fragments
						) ||
						!WritePixel(*colour, x, y, fragments[0]) ||
						!WritePixel(*depthImage, x, y, fragments[1]) ||
						!WritePixel(*normal, x, y, fragments[2])) {
						context.ClearOutputs();
						return context.Fail(
							Status::UnsupportedExecution,
							"surface projection pixel has undefined shader arithmetic",
							"surface_out"
						);
					}
				}
			return true;
		}
	}
	std::span<const ExecutorEntry> SourceSurfaceProjectionExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.surface_project_3_d", SurfaceProject, true}};
		return entries;
	}
}
