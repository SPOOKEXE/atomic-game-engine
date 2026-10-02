// Source: Pixel Composer b69eca232217360cf1502ef0223523d818606652 node_array_*
// and array_functions.gml.
#include "../SourceBuiltinRandomContext.hpp"
#include "../SourceRandom.hpp"
#include "../TimelineDrivers.hpp"
#include "ArraySource.hpp"

#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		using namespace source_array;
		struct Input {
			ValueType EmptyType = ValueType::Integer;
			std::optional<AllocationReservation> Charge;
			SourceArrayItem Root{ElementValue{double{0}}};
			const Items *Array() const {
				return std::get_if<Items>(&Root.Data);
			}
		};
		bool Read(NodeContext &context, std::string_view port, Input &out) {
			const Value *value = context.Find(port);
			const Image *image = context.Input(port);
			const ImageArray *images = nullptr;
			for (const auto &[id, candidate] : context.ImageArrays)
				if (id == port) images = candidate;
			TreeCost cost;
			const ArrayValue *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (const auto *selector = value ? std::get_if<ArraySelectorValue>(value) : nullptr) {
				if (context.Authored.Type != "pc.array_get" && context.Authored.Type != "pc.array_randomizer")
					return context.Fail(
						Status::UnsupportedExecution, "source node requires an ordinary array", port
					);
				if (!selector->Data)
					return context.Fail(Status::InvalidValue, "array selector payload is missing", port);
				array = &selector->Data->Values;
			}
			if (value && !ValidRuntimeValue(*value))
				return context.Fail(Status::InvalidValue, "source input is invalid", port);
			if (array) {
				out.EmptyType =
					array->ElementType == ValueType::Any ? ValueType::Integer : array->ElementType;
				cost.Bytes = RetainedPayloadBytes(*array);
				if (!array->Items.empty()) {
					TreeCost shape;
					if (!MeasureSource(array->Items, shape))
						return context.Fail(Status::LimitExceeded, "source input exceeds bounds", port);
					cost.Nodes = shape.Nodes;
					cost.Bytes += shape.Bytes;
				} else {
					const auto count = [&](const auto &row) {
						for (const auto &leaf : row)
							cost.Nodes += 1 + PackedCount(leaf);
					};
					cost.Nodes = array->Nested.size();
					count(array->Elements);
					for (const auto &row : array->Nested)
						count(row);
					cost.Bytes += cost.Nodes * sizeof(SourceArrayItem);
				}
			} else if (images) {
				if (!ImageCost(*images, images->Items, cost, 1))
					return context.Fail(Status::InvalidValue, "source image shape is invalid", port);
			} else if (image) {
				if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumArrayBytes))
					return context.Fail(Status::InvalidValue, "source image is invalid", port);
				cost.Bytes = image->Pixels.capacity();
			} else if (value) {
				cost.Nodes = PackedCount(*value);
				cost.Bytes = RetainedPayloadBytes(*value) + (cost.Nodes + 1) * sizeof(SourceArrayItem);
			}
			if (cost.Nodes > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "source input exceeds bounds", port);
			out.Charge = context.ReserveWorkspace(cost.Bytes, port);
			if (!out.Charge) return false;
			if (array)
				out.Root = {FromValues(*array)};
			else if (images)
				out.Root = {FromImages(*images, images->Items)};
			else if (image)
				out.Root = {*image};
			else if (value)
				out.Root = SourceLeaf(*value);
			return true;
		}
		bool Index(const SourceArrayItem &item, int64_t &result) {
			const auto *leaf = std::get_if<ElementValue>(&item.Data);
			if (!leaf) return false;
			if (const auto *integer = std::get_if<int64_t>(leaf)) {
				result = *integer;
				return true;
			}
			if (const auto *number = std::get_if<double>(leaf)) {
				if (!std::isfinite(*number) || *number < double(INT64_MIN) ||
					*number >= 9223372036854775808.0)
					return false;
				result = int64_t(*number);
				return true;
			}
			return false;
		}
		const SourceArrayItem Zero{ElementValue{double{0}}};
		using PlanItems = std::vector<const SourceArrayItem *>;
		bool Finish(
			NodeContext &context,
			const PlanItems &plan,
			std::string_view port,
			ValueType emptyType = ValueType::Integer
		) {
			TreeCost cost;
			for (const auto *item : plan)
				if (!MeasureItem(*item, cost))
					return context.Fail(Status::LimitExceeded, "source array output exceeds bounds", port);
			auto charge = context.ReserveWorkspace(cost.Bytes, port);
			if (!charge) return false;
			Items output;
			output.reserve(plan.size());
			for (const auto *item : plan)
				output.push_back(*item);
			return Publish(context, std::move(output), port, emptyType);
		}
		bool Append(PlanItems &plan, const SourceArrayItem &item, bool spread) {
			const auto *children = std::get_if<Items>(&item.Data);
			const size_t count = spread && children ? children->size() : 1;
			if (count > Limits::MaximumArrayElements - plan.size()) return false;
			if (spread && children)
				for (const auto &child : *children)
					plan.push_back(&child);
			else
				plan.push_back(&item);
			return true;
		}
		bool Get(NodeContext &context, const Items &input, const Input &indices, ValueType emptyType) {
			const double mode = context.SourceChoice("mode");
			if (mode != 0 && mode != 1) {
				context.SetValue("value", 0.0);
				return context.FailureCode == Status::Ok;
			}
			const bool randomMode = mode == 1;
			SourceRandom random(uint32_t(context.Integer("seed")));
			const int64_t overflow = context.Integer("overflow");
			if (!randomMode && (overflow < 0 || overflow > 2))
				return context.Fail(Status::InvalidValue, "get overflow is invalid", "overflow");
			const auto select = [&](const SourceArrayItem &index) -> const SourceArrayItem * {
				int64_t at;
				if (!Index(index, at) || input.empty()) return &Zero;
				const int64_t length = int64_t(input.size());
				if (overflow == 0) {
					if (at < 0) at += length;
					at = std::clamp<int64_t>(at, 0, length - 1);
				} else if (overflow == 1) {
					at %= length;
					if (at < 0) at += length;
				} else {
					const uint64_t magnitude = at < 0 ? uint64_t(-(at + 1)) + 1 : uint64_t(at);
					const uint64_t period = uint64_t(length - 1) * 2;
					at = period ? int64_t(magnitude % period) : 0;
					if (at >= length) at = int64_t(period) - at;
				}
				return &input[size_t(at)];
			};
			auto planCharge = context.ReserveWorkspace(
				Limits::MaximumArrayElements * sizeof(const SourceArrayItem *), "index"
			);
			if (!planCharge) return false;
			PlanItems plan;
			if (const auto *array = indices.Array(); array && !randomMode) {
				plan.reserve(array->size());
				for (const auto &index : *array)
					plan.push_back(select(index));
				return Finish(context, plan, "value", emptyType);
			}
			size_t randomIndex = randomMode && !input.empty() ? random.Index(uint32_t(input.size())) : 0;
			if (randomMode) {
				const Value *raw = context.Find("array");
				if (const auto *selector = raw ? std::get_if<ArraySelectorValue>(raw) : nullptr;
					selector && selector->Data) {
					if (input.empty()) {
						context.SetValue("value", UndefinedValue{});
						return context.FailureCode == Status::Ok;
					}
					SourceRandom weighted(uint32_t(context.Integer("seed")));
					const double target = weighted.Unit() * selector->Data->TotalWeight;
					const auto &weights = selector->Data->CumulativeWeights;
					randomIndex = 0;
					if (weights.size() > 2) {
						// Source permits signed weights and checks the first endpoint before the last.
						if (target <= weights.front())
							randomIndex = 0;
						else if (target >= weights.back())
							randomIndex = weights.size() - 1;
						else if (target > weights.front()) {
							int64_t low = 0, high = int64_t(weights.size()) - 1;
							while (low <= high) {
								const size_t mid = size_t((low + high) / 2);
								if (mid + 1 >= weights.size()) break;
								if (weights[mid] < target && weights[mid + 1] >= target) {
									randomIndex = mid;
									break;
								}
								if (weights[mid] < target)
									low = int64_t(mid) + 1;
								else
									high = int64_t(mid) - 1;
							}
						}
					}
				}
			}

			if (randomMode && !input.empty() && randomIndex >= input.size())
				return context.Fail(
					Status::InvalidValue, "source random selection index exceeds input", "array"
				);
			const SourceArrayItem &selected =
				randomMode ? input.empty() ? Zero : input[randomIndex] : *select(indices.Root);
			if (const auto *array = std::get_if<Items>(&selected.Data)) {
				plan.reserve(array->size());
				for (const auto &item : *array)
					plan.push_back(&item);
				return Finish(context, plan, "value", emptyType);
			}
			TreeCost cost;
			if (!MeasureItem(selected, cost))
				return context.Fail(Status::LimitExceeded, "source get output exceeds bounds", "value");
			if (!context.ReserveOutput(cost.Bytes, "value")) return false;
			if (const auto *leaf = std::get_if<ElementValue>(&selected.Data))
				context.SetValue("value", std::visit([](const auto &data) -> Value { return data; }, *leaf));
			else
				context.OutputImages.emplace_back("value", std::get<Image>(selected.Data));
			return context.FailureCode == Status::Ok;
		}

		std::optional<bool>
		Equal(const SourceArrayItem &left, const SourceArrayItem &right, size_t depth = 0) {
			if (depth > 8) return false;
			const auto *a = std::get_if<Items>(&left.Data), *b = std::get_if<Items>(&right.Data);
			if (a || b) {
				if (!a || !b || a->size() != b->size()) return false;
				for (size_t i = 0; i < a->size(); ++i) {
					const auto equal = Equal((*a)[i], (*b)[i], depth + 1);
					if (!equal || !*equal) return equal;
				}
				return true;
			}
			const auto *x = std::get_if<ElementValue>(&left.Data),
					   *y = std::get_if<ElementValue>(&right.Data);
			if (!x || !y) return std::nullopt;
			using Number = std::variant<int64_t, double>;
			const auto number = [](const ElementValue &item) -> std::optional<Number> {
				return std::visit(
					[](const auto &value) -> std::optional<Number> {
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, double>)
							return Number{value};
						else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t>)
							return Number{int64_t(value)};
						else if constexpr (std::is_same_v<T, EnumValue>)
							return Number{value.Value};
						else
							return std::nullopt;
					},
					item
				);
			};
			const auto numericA = number(*x), numericB = number(*y);
			if (numericA || numericB) {
				if (!numericA || !numericB) return false;
				const auto *integerA = std::get_if<int64_t>(&*numericA),
						   *integerB = std::get_if<int64_t>(&*numericB);
				if (integerA && integerB) return *integerA == *integerB;
				if (!integerA && !integerB) return std::get<double>(*numericA) == std::get<double>(*numericB);
				const int64_t integer = integerA ? *integerA : *integerB;
				const double floating = integerA ? std::get<double>(*numericB) : std::get<double>(*numericA);
				// Native mixed comparisons preserve integer precision on every supported
				// compiler.
				return std::isfinite(floating) && floating >= -9223372036854775808.0 &&
					   floating < 9223372036854775808.0 && std::trunc(floating) == floating &&
					   int64_t(floating) == integer;
			}
			if (const auto *text = std::get_if<std::string>(x))
				return std::holds_alternative<std::string>(*y) && *text == std::get<std::string>(*y);
			// Source structs and surfaces compare identity, which owned native values
			// cannot reconstruct.
			return std::nullopt;
		}
		// Borrow first: repetition can exceed the source-shape budget before one
		// normalized copy would be affordable. No intermediate input tree is needed.
		struct UniformCost {
			TreeCost Shape;
			uint64_t StorageBytes = 0;
		};
		bool UniformWrapperCost(UniformCost &cost) {
			if (!MeasureWrapper(cost.Shape)) return false;
			cost.StorageBytes += sizeof(SourceArrayItem);
			return true;
		}
		template <class T> uint64_t UniformCopiedPayloadBytes(const T &data) {
			if constexpr (std::is_same_v<T, std::string>)
				return std::max(data.size(), std::string{}.capacity());
			else
				return PayloadOwnedBytes(data);
		}
		template <class Variant> bool UniformLeafCost(const Variant &leaf, UniformCost &cost, size_t depth) {
			if (depth > Limits::MaximumArrayDepth || !UniformWrapperCost(cost)) return false;
			const size_t components = PackedCount(leaf);
			if (components) {
				if (depth == Limits::MaximumArrayDepth ||
					components > Limits::MaximumArrayElements - cost.Shape.Nodes)
					return false;
				cost.Shape.Nodes += components;
				cost.Shape.Bytes += components * sizeof(SourceArrayItem);
				cost.StorageBytes += components * sizeof(SourceArrayItem);
			} else {
				cost.Shape.Bytes +=
					std::visit([](const auto &data) { return PayloadOwnedBytes(data); }, leaf);
				cost.StorageBytes +=
					std::visit([](const auto &data) { return UniformCopiedPayloadBytes(data); }, leaf);
			}
			return cost.Shape.Bytes <= Limits::MaximumArrayBytes;
		}
		bool UniformItemsCost(const Items &items, UniformCost &cost, size_t depth, bool &hasImages) {
			if (depth > Limits::MaximumArrayDepth) return false;
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!UniformLeafCost(*leaf, cost, depth)) return false;
				} else {
					if (!UniformWrapperCost(cost)) return false;
					if (const auto *children = std::get_if<Items>(&item.Data)) {
						if (!UniformItemsCost(*children, cost, depth + 1, hasImages)) return false;
					} else {
						hasImages = true;
						cost.Shape.Bytes += std::get<Image>(item.Data).Pixels.size();
						cost.StorageBytes += std::get<Image>(item.Data).Pixels.size();
						if (cost.Shape.Bytes > Limits::MaximumArrayBytes) return false;
					}
				}
			}
			return true;
		}
		bool UniformImageShapeValid(
			const ImageArray &array, const std::vector<ImageArrayItem> &items, size_t &count, size_t depth
		) {
			if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - count)
				return false;
			count += items.size();
			for (const auto &item : items) {
				if (const auto *index = std::get_if<size_t>(&item.Data)) {
					if (*index >= array.Images.size()) return false;
				} else if (!UniformImageShapeValid(
							   array, std::get<std::vector<ImageArrayItem>>(item.Data), count, depth + 1
						   ))
					return false;
			}
			return true;
		}
		bool UniformImageCost(
			const ImageArray &array, const std::vector<ImageArrayItem> &items, UniformCost &cost, size_t depth
		) {
			if (depth > Limits::MaximumArrayDepth) return false;
			for (const auto &item : items) {
				if (!UniformWrapperCost(cost)) return false;
				if (const auto *index = std::get_if<size_t>(&item.Data)) {
					cost.Shape.Bytes += array.Images[*index].Pixels.size();
					cost.StorageBytes += array.Images[*index].Pixels.size();
					if (cost.Shape.Bytes > Limits::MaximumArrayBytes) return false;
				} else if (!UniformImageCost(
							   array, std::get<std::vector<ImageArrayItem>>(item.Data), cost, depth + 1
						   ))
					return false;
			}
			return true;
		}
		// Native Uniform admits finite floating-point images, including nested
		// leaves and unused image-pool entries. Validate before the zero-length exit.
		bool UniformItemsFinite(const Items &items) {
			for (const auto &item : items) {
				if (const auto *image = std::get_if<Image>(&item.Data)) {
					if (!FiniteSurfaceSamples(*image)) return false;
				} else if (const auto *children = std::get_if<Items>(&item.Data)) {
					if (!UniformItemsFinite(*children)) return false;
				}
			}
			return true;
		}
		bool Uniform(NodeContext &context) {
			const Value *lengthValue = context.Find("length");
			const int64_t *resolvedLength = lengthValue ? std::get_if<int64_t>(lengthValue) : nullptr;
			bool surfaceLength = context.Input("length") != nullptr;
			for (const auto &[port, images] : context.ImageArrays)
				if (port == "length" && images) surfaceLength = true;
			if ((lengthValue && !resolvedLength) || surfaceLength)
				return context.Fail(
					Status::UnsupportedExecution,
					"source uniform length requires one resolved integer",
					"length"
				);
			const int64_t requested = resolvedLength ? *resolvedLength : 1;
			if (requested < 0)
				return context.Fail(Status::InvalidValue, "source uniform length is negative", "length");
			if (requested > int64_t(Limits::MaximumArrayElements))
				return context.Fail(Status::LimitExceeded, "source uniform length exceeds bounds", "length");
			const size_t length = size_t(requested);
			const Value *value = context.Find("data");
			const Image *image = context.Input("data");
			const ImageArray *images = nullptr;
			for (const auto &[port, candidate] : context.ImageArrays)
				if (port == "data") images = candidate;
			if (value && !ValidRuntimeValue(*value))
				return context.Fail(Status::InvalidValue, "source uniform data is invalid", "data");
			if (image && !ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumArrayBytes))
				return context.Fail(Status::InvalidValue, "source uniform image is invalid", "data");
			if (image && !FiniteSurfaceSamples(*image))
				return context.Fail(
					Status::InvalidValue, "source uniform image samples are nonfinite", "data"
				);
			if (images) {
				size_t count = 0;
				if (images->Images.size() > Limits::MaximumArrayElements ||
					!UniformImageShapeValid(*images, images->Items, count, 1))
					return context.Fail(
						Status::InvalidValue, "source uniform image shape is invalid", "data"
					);
				for (const auto &member : images->Images) {
					if (!ValidSurfaceLayout(member, Limits::MaximumDimension, Limits::MaximumArrayBytes))
						return context.Fail(Status::InvalidValue, "source uniform image is invalid", "data");
					if (!FiniteSurfaceSamples(member))
						return context.Fail(
							Status::InvalidValue, "source uniform image samples are nonfinite", "data"
						);
				}
			}
			const ArrayValue *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (array && !UniformItemsFinite(array->Items))
				return context.Fail(
					Status::InvalidValue, "source uniform image samples are nonfinite", "data"
				);
			const ValueType emptyType = array	? array->ElementType
										: value ? PayloadType(*value)
												: ValueType::Any;
			const bool imageSource = !array && (images || image);
			if (length == 0) {
				if (imageSource) {
					if (!context.ReserveOutput(std::string{}.capacity(), "array_out")) return false;
					context.OutputImageArrays.emplace_back("array_out", ImageArray{});
				} else
					context.SetValue("array_out", ArrayValue{emptyType, {}});
				return context.FailureCode == Status::Ok;
			}
			if (!value && !image && !images)
				return context.Fail(
					Status::UnsupportedExecution, "source uniform noone default is unavailable", "data"
				);
			UniformCost memberCost;
			bool hasImages = imageSource;
			bool measured = true;
			if (array) {
				measured = UniformWrapperCost(memberCost);
				if (!array->Items.empty())
					measured = measured && UniformItemsCost(array->Items, memberCost, 2, hasImages);
				else if (!array->Nested.empty()) {
					for (const auto &row : array->Nested) {
						if (!measured || !UniformWrapperCost(memberCost)) {
							measured = false;
							break;
						}
						for (const auto &leaf : row)
							if (!UniformLeafCost(leaf, memberCost, 3)) {
								measured = false;
								break;
							}
					}
				} else
					for (const auto &leaf : array->Elements)
						if (!measured || !UniformLeafCost(leaf, memberCost, 2)) {
							measured = false;
							break;
						}
			} else if (images)
				measured =
					UniformWrapperCost(memberCost) && UniformImageCost(*images, images->Items, memberCost, 2);
			else if (image) {
				measured = UniformWrapperCost(memberCost);
				memberCost.Shape.Bytes += image->Pixels.size();
				memberCost.StorageBytes += image->Pixels.size();
			} else
				measured = UniformLeafCost(*value, memberCost, 1);
			if (!measured || memberCost.Shape.Nodes > Limits::MaximumArrayElements / length ||
				memberCost.Shape.Bytes > Limits::MaximumArrayBytes / length)
				return context.Fail(
					Status::LimitExceeded, "source uniform output exceeds bounds", "array_out"
				);
			// One admitted candidate is built directly from borrowed input. Publication
			// admits its separate normalization buffers while this charge remains live.
			auto candidateCharge = context.ReserveWorkspace(memberCost.StorageBytes * length, "array_out");
			if (!candidateCharge) return false;
			Items output;
			output.reserve(length);
			for (size_t index = 0; index < length; ++index) {
				if (array)
					output.push_back({FromValues(*array)});
				else if (images)
					output.push_back({FromImages(*images, images->Items)});
				else if (image)
					output.push_back({*image});
				else
					output.push_back(SourceLeaf(*value));
			}
			// Empty Any rows have no homogeneous legacy element type. Keep their
			// general carrier instead of creating invalid Any/Nested storage.
			if (array && array->ElementType == ValueType::Any && array->Items.empty() &&
				array->Elements.empty() && array->Nested.empty()) {
				ArrayValue result{ValueType::Any, {}};
				result.Items = std::move(output);
				context.SetValue("array_out", std::move(result));
				return context.FailureCode == Status::Ok;
			}
			return Publish(context, std::move(output), "array_out", emptyType, hasImages, true);
		}
		// Borrowed views keep unselected packed rows out of the output budget.
		struct RearrangeView {
			const ElementValue *Leaf = nullptr;
			const std::vector<ElementValue> *Row = nullptr;
			const SourceArrayItem *Item = nullptr;
			const ImageArray *Pool = nullptr;
			const ImageArrayItem *ImageItem = nullptr;
			double Number = 0;
		};
		template <class Variant> double RearrangeComponent(const Variant &value, size_t index) {
			return std::visit(
				[&](const auto &data) -> double {
					using T = std::decay_t<decltype(data)>;
					if constexpr (std::is_same_v<T, Vector2>)
						return index == 0 ? data.X : data.Y;
					else if constexpr (std::is_same_v<T, Vector3>)
						return index == 0 ? data.X : index == 1 ? data.Y : data.Z;
					else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
						return index == 0 ? data.X : index == 1 ? data.Y : index == 2 ? data.Z : data.W;
					else if constexpr (std::is_same_v<T, Area>) {
						const double fields[]{
							data.CenterX,
							data.CenterY,
							data.HalfWidth,
							data.HalfHeight,
							double(data.Shape),
							double(data.Mode)
						};
						return fields[index];
					} else if constexpr (std::is_same_v<T, Curve>)
						return index < 6 ? data.Header[index]
										 : data.Anchors[(index - 6) / 6][(index - 6) % 6];
					else
						return 0;
				},
				value
			);
		}
		struct RearrangeRoot {
			const Value *ValueData = nullptr;
			const ArrayValue *Array = nullptr;
			const ImageArray *Images = nullptr;
			const Image *Surface = nullptr;
			const MatrixValue *Matrix = nullptr;
			size_t Count = 0;
			bool IsArray = false;
			bool Dimensions = false;
		};
		RearrangeRoot RearrangeRead(NodeContext &context, std::string_view port, bool orders) {
			RearrangeRoot root;
			root.ValueData = context.Find(port);
			root.Surface = context.Input(port);
			for (const auto &[id, images] : context.ImageArrays)
				if (id == port) root.Images = images;
			if (root.ValueData) root.Array = std::get_if<ArrayValue>(root.ValueData);
			if (root.Array) {
				root.Count = !root.Array->Items.empty()	   ? root.Array->Items.size()
							 : !root.Array->Nested.empty() ? root.Array->Nested.size()
														   : root.Array->Elements.size();
				root.IsArray = true;
			} else if (orders && (root.Surface || root.Images)) {
				root.Dimensions = root.IsArray = true;
				root.Count = 2;
			} else if (root.Images) {
				root.IsArray = true;
				root.Count = root.Images->Items.size();
			} else if (root.ValueData) {
				if (orders) root.Matrix = std::get_if<MatrixValue>(root.ValueData);
				root.Count = root.Matrix ? root.Matrix->Values.size() : PackedCount(*root.ValueData);
				root.IsArray = root.Matrix || root.Count != 0;
			}
			return root;
		}
		RearrangeView RearrangeAt(const RearrangeRoot &root, size_t index) {
			RearrangeView view;
			if (root.Dimensions)
				view.Number = root.Surface ? (index == 0 ? root.Surface->Width : root.Surface->Height) : 1;
			else if (root.Array) {
				if (!root.Array->Items.empty())
					view.Item = &root.Array->Items[index];
				else if (!root.Array->Nested.empty())
					view.Row = &root.Array->Nested[index];
				else
					view.Leaf = &root.Array->Elements[index];
			} else if (root.Images) {
				view.Pool = root.Images;
				view.ImageItem = &root.Images->Items[index];
			} else
				view.Number =
					root.Matrix ? root.Matrix->Values[index] : RearrangeComponent(*root.ValueData, index);
			return view;
		}
		// Source getter depth follows only a nonempty first-child chain.
		bool RearrangeDeepOrders(const RearrangeRoot &root) {
			if (!root.Array || root.Count == 0) return false;
			const auto first = RearrangeAt(root, 0);
			if (first.Row) return !first.Row->empty();
			if (first.Leaf) return PackedCount(*first.Leaf) != 0;
			if (first.Item) {
				if (const auto *children = std::get_if<Items>(&first.Item->Data)) return !children->empty();
				if (const auto *leaf = std::get_if<ElementValue>(&first.Item->Data))
					return PackedCount(*leaf) != 0;
			}
			return false;
		}
		bool RearrangeValid(NodeContext &context, const RearrangeRoot &root, std::string_view port) {
			if (root.ValueData && !ValidRuntimeValue(*root.ValueData))
				return context.Fail(Status::InvalidValue, "source rearrange input is invalid", port);
			if (root.Array && !UniformItemsFinite(root.Array->Items))
				return context.Fail(
					Status::InvalidValue, "source rearrange image samples are nonfinite", port
				);
			if (root.Surface &&
				(!ValidSurfaceLayout(*root.Surface, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
				 !FiniteSurfaceSamples(*root.Surface)))
				return context.Fail(
					Status::InvalidValue, "source rearrange image is invalid or nonfinite", port
				);
			if (root.Images) {
				size_t count = 0;
				if (root.Images->Images.size() > Limits::MaximumArrayElements ||
					!UniformImageShapeValid(*root.Images, root.Images->Items, count, 1))
					return context.Fail(
						Status::InvalidValue, "source rearrange image shape is invalid", port
					);
				for (const auto &image : root.Images->Images)
					if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
						!FiniteSurfaceSamples(image))
						return context.Fail(
							Status::InvalidValue, "source rearrange image is invalid or nonfinite", port
						);
			}
			if (port == "array" && root.Count > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "source rearrange root exceeds bounds", port);
			return true;
		}
		bool RearrangeCost(const RearrangeView &view, UniformCost &cost, bool &images) {
			if (view.Leaf) return UniformLeafCost(*view.Leaf, cost, 1);
			if (view.Row) {
				if (!UniformWrapperCost(cost)) return false;
				for (const auto &leaf : *view.Row)
					if (!UniformLeafCost(leaf, cost, 2)) return false;
				return true;
			}
			if (view.Item) {
				if (const auto *leaf = std::get_if<ElementValue>(&view.Item->Data))
					return UniformLeafCost(*leaf, cost, 1);
				if (!UniformWrapperCost(cost)) return false;
				if (const auto *children = std::get_if<Items>(&view.Item->Data))
					return UniformItemsCost(*children, cost, 2, images);
				images = true;
				cost.Shape.Bytes += std::get<Image>(view.Item->Data).Pixels.size();
				cost.StorageBytes += std::get<Image>(view.Item->Data).Pixels.size();
				return cost.Shape.Bytes <= Limits::MaximumArrayBytes;
			}
			if (view.ImageItem) {
				if (!UniformWrapperCost(cost)) return false;
				if (const auto *index = std::get_if<size_t>(&view.ImageItem->Data)) {
					images = true;
					cost.Shape.Bytes += view.Pool->Images[*index].Pixels.size();
					cost.StorageBytes += view.Pool->Images[*index].Pixels.size();
					return cost.Shape.Bytes <= Limits::MaximumArrayBytes;
				}
				return UniformImageCost(
					*view.Pool, std::get<std::vector<ImageArrayItem>>(view.ImageItem->Data), cost, 2
				);
			}
			return UniformWrapperCost(cost);
		}
		SourceArrayItem RearrangeClone(const RearrangeView &view) {
			if (view.Leaf) return SourceLeaf(*view.Leaf);
			if (view.Row) {
				Items row;
				row.reserve(view.Row->size());
				for (const auto &leaf : *view.Row)
					row.push_back(SourceLeaf(leaf));
				return {std::move(row)};
			}
			if (view.Item) {
				if (const auto *leaf = std::get_if<ElementValue>(&view.Item->Data)) return SourceLeaf(*leaf);
				if (const auto *children = std::get_if<Items>(&view.Item->Data))
					return {SourceItems(*children)};
				return *view.Item;
			}
			if (view.ImageItem) {
				if (const auto *index = std::get_if<size_t>(&view.ImageItem->Data))
					return {view.Pool->Images[*index]};
				return {FromImages(*view.Pool, std::get<std::vector<ImageArrayItem>>(view.ImageItem->Data))};
			}
			return {ElementValue{view.Number}};
		}
		bool RearrangeIndex(NodeContext &context, RearrangeView view, size_t count, size_t &selected) {
			const ElementValue *leaf = view.Leaf;
			if (view.Item) leaf = std::get_if<ElementValue>(&view.Item->Data);
			if (view.Row || view.ImageItem || (view.Item && !leaf) || (leaf && PackedCount(*leaf)))
				return context.Fail(
					Status::UnsupportedExecution,
					"source rearrange Orders comparison is unrepresented",
					"orders"
				);
			long double number = view.Number;
			bool supported = true;
			if (leaf)
				std::visit(
					[&](const auto &data) {
						using T = std::decay_t<decltype(data)>;
						if constexpr (std::is_same_v<T, double>)
							number = DriverRoundHalfEven(data);
						else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
							number = data;
						else if constexpr (std::is_same_v<T, EnumValue>)
							number = data.Value;
						else if constexpr (std::is_same_v<T, Colour>)
							number = uint32_t(data.Red) | (uint32_t(data.Green) << 8) |
									 (uint32_t(data.Blue) << 16) | (uint32_t(data.Alpha) << 24);
						else
							supported = false;
					},
					*leaf
				);
			else
				number = DriverRoundHalfEven(view.Number);
			if (!supported)
				return context.Fail(
					Status::UnsupportedExecution,
					"source rearrange Orders conversion is unrepresented",
					"orders"
				);
			selected = number >= 0 && number < count ? size_t(number) : count;
			return true;
		}
		bool Rearrange(NodeContext &context) {
			const auto data = RearrangeRead(context, "array", false),
					   orders = RearrangeRead(context, "orders", true);
			if (!RearrangeValid(context, data, "array") || !RearrangeValid(context, orders, "orders"))
				return false;
			if (!data.IsArray)
				return context.Fail(
					Status::UnsupportedExecution,
					"source rearrange non-array return is history-dependent",
					"array"
				);
			if (orders.ValueData && std::holds_alternative<Quaternion>(*orders.ValueData))
				return context.Fail(
					Status::UnsupportedExecution,
					"source rearrange Orders quaternion getter is unrepresented",
					"orders"
				);
			if (orders.ValueData && std::holds_alternative<std::string>(*orders.ValueData))
				return context.Fail(
					Status::UnsupportedExecution,
					"source rearrange Orders text grammar is unrepresented",
					"orders"
				);
			const bool identity = !orders.IsArray || orders.Count != data.Count;
			if (!identity && RearrangeDeepOrders(orders))
				return context.Fail(
					Status::UnsupportedExecution,
					"source rearrange Orders comparison is unrepresented",
					"orders"
				);
			UniformCost cost;
			bool images = data.Images != nullptr;
			// Fixed stack selection has no heap lease and survives no evaluation.
			std::array<size_t, Limits::MaximumArrayElements> selection{};
			for (size_t index = 0; index < data.Count; ++index) {
				selection[index] = index;
				if (!identity &&
					!RearrangeIndex(context, RearrangeAt(orders, index), data.Count, selection[index]))
					return false;
				const auto member =
					selection[index] == data.Count ? RearrangeView{} : RearrangeAt(data, selection[index]);
				if (!RearrangeCost(member, cost, images))
					return context.Fail(
						Status::LimitExceeded, "source rearrange output exceeds bounds", "array"
					);
			}
			const ValueType emptyType = data.Array ? data.Array->ElementType : ValueType::Any;
			if (data.Count == 0 && data.Images) {
				if (!context.ReserveOutput(std::string{}.capacity(), "array")) return false;
				context.OutputImageArrays.emplace_back("array", ImageArray{});
				return true;
			}
			auto charge = context.ReserveWorkspace(cost.StorageBytes, "array");
			if (!charge) return false;
			Items output;
			output.reserve(data.Count);
			for (size_t index = 0; index < data.Count; ++index)
				output.push_back(RearrangeClone(
					selection[index] == data.Count ? RearrangeView{} : RearrangeAt(data, selection[index])
				));
			// Empty general rows carry no homogeneous type to put in Nested.
			if (emptyType == ValueType::Any && !images && !output.empty() &&
				std::all_of(output.begin(), output.end(), [](const auto &item) {
					const auto *row = std::get_if<Items>(&item.Data);
					return row && row->empty();
				})) {
				ArrayValue result{ValueType::Any, {}};
				result.Items = std::move(output);
				context.SetValue("array", std::move(result));
				return context.FailureCode == Status::Ok;
			}
			return Publish(context, std::move(output), "array", emptyType, images, true);
		}

		bool Split(NodeContext &context) {
			Input source;
			if (!Read(context, "array", source)) return false;
			const Items *input = source.Array();
			const int64_t minimum = context.Integer("minimum_outputs");
			if (minimum < 0 || uint64_t(minimum) > Limits::MaximumDynamicOutputsPerNode)
				return context.Fail(
					Status::LimitExceeded, "split minimum output count exceeds bounds", "minimum_outputs"
				);
			const size_t count = std::max(size_t(minimum), input ? input->size() : size_t{0});
			context.RuntimeOutputCount = count;
			for (size_t index = 0; index < count; ++index) {
				const std::string port = "val_" + std::to_string(index);
				const SourceArrayItem &item = input && index < input->size() ? (*input)[index] : Zero;
				TreeCost cost;
				if (!MeasureItem(item, cost))
					return context.Fail(Status::LimitExceeded, "split output exceeds bounds", port);
				if (const auto *row = std::get_if<Items>(&item.Data)) {
					auto scratch = context.ReserveWorkspace(cost.Bytes, port);
					if (!scratch) return false;
					Items output = *row;
					if (!Publish(context, std::move(output), port, source.EmptyType)) return false;
				} else if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!context.ReserveOutput(cost.Bytes, port)) return false;
					context.SetValue(
						port, std::visit([](const auto &value) -> Value { return value; }, *leaf)
					);
				} else {
					const auto &image = std::get<Image>(item.Data);
					if (!context.ReserveOutput(image.Pixels.size() + std::string{}.capacity(), port))
						return false;
					context.OutputImages.emplace_back(port, image);
				}
				if (context.FailureCode != Status::Ok) return false;
			}
			return true;
		}

		bool Pin(NodeContext &context) {
			auto inputsCharge =
				context.ReserveWorkspace(context.Authored.DynamicInputs.size() * sizeof(Input), "array");
			if (!inputsCharge) return false;
			std::vector<Input> inputs;
			inputs.reserve(context.Authored.DynamicInputs.size());
			TreeCost cost;
			ValueType emptyType = ValueType::Any;
			for (const auto &port : context.Authored.DynamicInputs) {
				if (!context.IsLinked(port.Id)) continue;
				inputs.emplace_back();
				if (!Read(context, port.Id, inputs.back())) return false;
				emptyType = inputs.back().EmptyType;
				if (!MeasureItem(inputs.back().Root, cost))
					return context.Fail(Status::LimitExceeded, "pin output exceeds bounds", "array");
			}
			auto charge = context.ReserveWorkspace(cost.Bytes, "array");
			if (!charge) return false;
			Items output;
			output.reserve(inputs.size());
			for (auto &input : inputs)
				output.push_back(std::move(input.Root));
			return Publish(context, std::move(output), "array", emptyType);
		}

		bool BooleanOperation(NodeContext &context) {
			Input first, second;
			if (!Read(context, "array_1", first) || !Read(context, "array_2", second)) return false;
			const Items *a = first.Array(), *b = second.Array();
			if (!a || !b)
				return context.Fail(Status::InvalidValue, "boolean array inputs must be arrays", "array_1");
			const double operation = context.SourceChoice("operation");
			if (operation != 0 && operation != 1 && operation != 2 && operation != 3)
				return context.Fail(Status::InvalidValue, "boolean array operation is invalid", "operation");
			for (const Items *row : {a, b})
				for (const auto &item : *row) {
					const auto *leaf = std::get_if<ElementValue>(&item.Data);
					if (!leaf ||
						(!std::holds_alternative<double>(*leaf) && !std::holds_alternative<int64_t>(*leaf) &&
						 !std::holds_alternative<std::string>(*leaf) && !std::holds_alternative<bool>(*leaf)))
						return context.Fail(
							Status::UnsupportedExecution,
							"boolean array opaque identity or key conversion is unrepresented",
							"array_1"
						);
				}
			auto charge = context.ReserveWorkspace(
				(a->size() + b->size()) * (sizeof(const SourceArrayItem *) + sizeof(bool)), "array_out"
			);
			if (!charge) return false;
			PlanItems plan;
			plan.reserve(a->size() + b->size());
			const auto contains = [&](const PlanItems &row, const SourceArrayItem &item) {
				return std::any_of(row.begin(), row.end(), [&](const auto *other) {
					return Equal(*other, item).value_or(false);
				});
			};
			const auto appendUnique = [&](const SourceArrayItem &item) {
				if (!contains(plan, item)) plan.push_back(&item);
			};
			const auto subtract = [&](const Items &left, const Items &right, bool unique) {
				std::vector<bool> consumed(right.size(), false);
				for (const auto &item : left) {
					bool removed = false;
					for (size_t index = 0; index < right.size(); ++index)
						if (!consumed[index] && Equal(item, right[index]).value_or(false)) {
							consumed[index] = true;
							removed = true;
							break;
						}
					if (!removed) {
						if (unique)
							appendUnique(item);
						else
							plan.push_back(&item);
					}
				}
			};
			if (operation == 0) {
				for (const auto &item : *a)
					appendUnique(item);
				for (const auto &item : *b)
					appendUnique(item);
			} else if (operation == 1)
				subtract(*a, *b, false);
			else if (operation == 2) {
				for (const auto &item : *a)
					if (std::any_of(b->begin(), b->end(), [&](const auto &other) {
							return Equal(item, other).value_or(false);
						}))
						appendUnique(item);
			} else {
				subtract(*a, *b, true);
				subtract(*b, *a, true);
			}
			return Finish(context, plan, "array_out", first.EmptyType);
		}

		bool WeightedSelector(NodeContext &context) {
			Input source;
			if (!Read(context, "array_in", source)) return false;
			const auto *input = source.Array();
			if (!input)
				return context.Fail(Status::InvalidValue, "selector input is not an array", "array_in");
			TreeCost cost;
			if (!MeasureSource(*input, cost))
				return context.Fail(Status::LimitExceeded, "selector contents exceed bounds", "array_in");
			const uint64_t bytes = sizeof(ArraySelectorData) + input->size() * sizeof(double) + cost.Bytes;
			if (!context.ReserveOutput(bytes, "array_selector")) return false;
			ArraySelectorValue result;
			result.Data.emplace();
			result.Data->Values.ElementType = ValueType::Any;
			result.Data->Values.Items = *input;
			result.Data->CumulativeWeights.reserve(input->size());
			for (size_t index = 0; index < input->size(); ++index) {
				result.Data->CumulativeWeights.push_back(result.Data->TotalWeight);
				const auto port = index < context.Authored.DynamicInputs.size()
									  ? context.Authored.DynamicInputs[index].Id
									  : std::string{};
				result.Data->TotalWeight += port.empty() ? 1 : context.Scalar(port, 1);
				if (!std::isfinite(result.Data->TotalWeight))
					return context.Fail(Status::InvalidValue, "selector weight total is nonfinite", port);
			}
			context.SetValue("array_selector", std::move(result));
			return context.FailureCode == Status::Ok;
		}

		bool Shuffle(NodeContext &context) {
			Input source;
			if (!Read(context, "array_in", source)) return false;
			const Items *input = source.Array();
			if (!input)
				return context.Fail(Status::InvalidValue, "shuffle input is not an array", "array_in");
			auto charge =
				context.ReserveWorkspace(input->size() * sizeof(const SourceArrayItem *), "array_in");
			if (!charge) return false;
			PlanItems plan;
			plan.reserve(input->size());
			for (const auto &item : *input)
				plan.push_back(&item);
			// Native seed policy uses the shared generator. HTML5 source shuffle uses Math.random().
			SourceRandom random(uint32_t(context.Integer("seed")));
			for (size_t count = plan.size(); count > 1; --count) {
				const size_t index = std::min<size_t>(random.Index(uint32_t(count)), count - 1);
				std::swap(plan[count - 1], plan[index]);
			}
			return Finish(context, plan, "shuffled_array", source.EmptyType);
		}

		bool Sample(NodeContext &context) {
			Input source;
			if (!Read(context, "array", source)) return false;
			const Items *input = source.Array();
			if (!input) return context.Fail(Status::InvalidValue, "sample input is not an array", "array");
			const double mode = context.SourceChoice("mode"),
						 amountType = context.SourceChoice("amount_type");
			if (context.FailureCode != Status::Ok) return false;
			if ((mode != 0 && mode != 1) || (mode == 0 && amountType != 0 && amountType != 1))
				return Publish(context, Items{}, "array", source.EmptyType);
			const double step = context.Scalar("step", 2);
			const int64_t dimension = std::max<int64_t>(0, context.Integer("dimension"));
			const int64_t amount = std::max<int64_t>(0, context.Integer("amount", 4));
			if (mode == 0 && (!(step > 0) || !std::isfinite(step)))
				return context.Fail(Status::InvalidValue, "sample step must be finite and positive", "step");
			if (uint64_t(dimension) > Limits::MaximumArrayDepth ||
				uint64_t(amount) > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "sample dimensions or amount exceed bounds", "amount"
				);
			const double shift = double(context.Integer("shift"));
			const double first = mode == 0 ? std::fmod(shift, step) : 0;
			const uint32_t seed = uint32_t(context.Integer("seed"));
			if (first < 0)
				return context.Fail(
					Status::InvalidValue, "source sample shift produces a negative index", "shift"
				);
			if (mode == 1 && context.Request.BuiltinRandomCaptures.size() > Limits::MaximumNodes)
				return context.Fail(
					Status::LimitExceeded, "Array Sample builtin capture count exceeds bounded nodes", "seed"
				);
			const SourceBuiltinRandomCapture *capture = nullptr;
			if (mode == 1 && std::any_of(
								 context.Request.BuiltinRandomCaptures.begin(),
								 context.Request.BuiltinRandomCaptures.end(),
								 [&](const auto &record) { return record.Authored.Id == context.Authored.Id; }
							 )) {
				if (!FindSourceBuiltinRandomCapture(context, capture)) return false;
			}
			size_t drawCursor = 0;
			const auto walk =
				[&](
					auto &&self, const Items &row, int64_t dim, size_t depth, TreeCost *cost, Items *output
				) -> bool {
				if (dim > 0) {
					if (output) output->reserve(row.size());
					for (const auto &item : row) {
						const auto *child = std::get_if<Items>(&item.Data);
						if (!child)
							return context.Fail(
								Status::InvalidValue, "sample dimension exceeds input shape", "dimension"
							);
						if (cost && !MeasureWrapper(*cost))
							return context.Fail(
								Status::LimitExceeded, "sample output exceeds bounds", "array"
							);
						Items clone;
						if (!self(self, *child, dim - 1, depth + 1, cost, output ? &clone : nullptr))
							return false;
						if (output) output->push_back({std::move(clone)});
					}
					return true;
				}
				if (row.empty()) {
					if (amountType == 1 && amount != 0)
						return context.Fail(
							Status::InvalidValue, "custom sampling requires nonempty input", "array"
						);
					return true;
				}
				const double countValue = amountType == 1 ? double(amount)
										  : mode == 1	  ? double(row.size())
										  : first < row.size()
											  ? std::ceil((double(row.size()) - first) / step)
											  : 0;
				if (!std::isfinite(countValue) || countValue > Limits::MaximumArrayElements)
					return context.Fail(Status::LimitExceeded, "sample output exceeds bounds", "array");
				const size_t count = size_t(countValue);
				if (output) output->reserve(count);
				double at = first;
				SourceRandom random(seed);
				for (size_t index = 0; index < count; ++index) {
					if (mode == 1) {
						if (capture) {
							if (drawCursor == capture->Draws.size())
								return context.Fail(
									Status::UnsupportedExecution,
									"Array Sample builtin recording is incomplete",
									"seed"
								);
							const auto &draw = capture->Draws[drawCursor++];
							if (draw.Operation != SourceBuiltinRandomOperation::IRandom || draw.Lower != 0 ||
								draw.Upper != double(row.size() - 1) || !std::isfinite(draw.Result) ||
								std::trunc(draw.Result) != draw.Result || draw.Result < 0 ||
								draw.Result > draw.Upper)
								return context.Fail(
									Status::InvalidValue,
									"Array Sample builtin draw does not match its source call",
									"seed"
								);
							at = draw.Result;
						} else
							at = random.Index(uint32_t(row.size()));
					}
					if (at >= row.size()) {
						if (amountType == 0) break;
						return context.Fail(
							Status::InvalidValue, "sample first index is outside input", "shift"
						);
					}
					const auto &selected = row[size_t(at)];
					if (cost && !MeasureItem(selected, *cost, depth))
						return context.Fail(Status::LimitExceeded, "sample output exceeds bounds", "array");
					if (output) output->push_back(selected);
					if (mode == 0) {
						at += step;
						if (amountType == 1) at = std::fmod(at, double(row.size()));
					}
				}
				return true;
			};
			TreeCost cost;
			if (!walk(walk, *input, dimension, 1, &cost, nullptr)) return false;
			if (capture && drawCursor != capture->Draws.size())
				return context.Fail(
					Status::InvalidValue, "Array Sample builtin recording has unused draws", "seed"
				);
			drawCursor = 0;
			auto charge = context.ReserveWorkspace(cost.Bytes, "array");
			if (!charge) return false;
			Items output;
			if (!walk(walk, *input, dimension, 1, nullptr, &output)) return false;
			return Publish(context, std::move(output), "array", source.EmptyType);
		}

		bool Unique(NodeContext &context) {
			Input source;
			if (!Read(context, "array_in", source)) return false;
			const Items *input = source.Array();
			if (!input)
				return context.Fail(Status::InvalidValue, "source unique input is not an array", "array_in");
			// The source calls a builtin comparator, not its recursive isEqual helper.
			// Owned recursive values retain content but cannot retain GameMaker reference
			// identity.
			if (input->size() > 1)
				for (const auto &item : *input)
					if (std::holds_alternative<Items>(item.Data))
						return context.Fail(
							Status::UnsupportedExecution,
							"source unique array reference comparison is unverified",
							"array_in"
						);
			auto planCharge =
				context.ReserveWorkspace(input->size() * sizeof(const SourceArrayItem *), "array_in");
			if (!planCharge) return false;
			PlanItems plan;
			plan.reserve(input->size());
			for (const auto &item : *input) {
				bool duplicate = false;
				for (const auto *retained : plan) {
					const auto equal = Equal(*retained, item);
					if (!equal)
						return context.Fail(
							Status::UnsupportedExecution,
							"source unique opaque identity comparison is unavailable",
							"array_in"
						);
					if (*equal) {
						duplicate = true;
						break;
					}
				}
				if (!duplicate) plan.push_back(&item);
			}
			// Stable first occurrence and exact integer comparisons are explicit native
			// policy.
			return Finish(context, plan, "unique_array", source.EmptyType);
		}
		bool QueryRemove(
			NodeContext &context,
			const Items &input,
			const Input &indices,
			const Input &value,
			ValueType emptyType
		) {
			auto charge = context.ReserveWorkspace(
				Limits::MaximumArrayElements * (sizeof(const SourceArrayItem *) + sizeof(int64_t))
			);
			if (!charge) return false;
			PlanItems plan;
			plan.reserve(Limits::MaximumArrayElements);
			for (const auto &item : input)
				plan.push_back(&item);
			const bool find = context.Authored.Type == "pc.array_find";
			const int64_t mode = find ? 1 : context.Integer("type");
			if (mode < 0 || mode > 1)
				return context.Fail(Status::InvalidValue, "remove type is invalid", "type");
			if (mode == 0) {
				std::vector<int64_t> indexes;
				indexes.reserve(indices.Array() ? indices.Array()->size() : 1);
				const auto add = [&](const SourceArrayItem &item) {
					int64_t at;
					if (Index(item, at)) indexes.push_back(at);
				};
				if (indices.Array())
					for (const auto &item : *indices.Array())
						add(item);
				else
					add(indices.Root);
				std::sort(indexes.begin(), indexes.end(), std::greater<>{});
				for (int64_t at : indexes) {
					if (at < 0) at += int64_t(plan.size());
					if (at < 0 || at >= int64_t(plan.size()))
						return context.Fail(
							Status::InvalidValue, "source remove index is outside bounds", "index"
						);
					plan.erase(plan.begin() + at);
				}
			} else {
				const Items *values = !find && context.Boolean("spread_array") ? value.Array() : nullptr;
				const size_t count = values ? values->size() : 1;
				for (size_t member = 0; member < count; ++member) {
					const SourceArrayItem &needle = values ? (*values)[member] : value.Root;
					int64_t found = -1;
					for (size_t i = 0; i < plan.size(); ++i) {
						const auto equal = Equal(*plan[i], needle);
						if (!equal)
							return context.Fail(
								Status::UnsupportedExecution,
								"source identity comparison is unavailable",
								"value"
							);
						if (*equal) {
							found = int64_t(i);
							break;
						}
					}
					if (find) {
						if (!context.ReserveOutput(0, "index")) return false;
						context.SetValue("index", found);
						return context.FailureCode == Status::Ok;
					}
					if (found >= 0) plan.erase(plan.begin() + found);
				}
			}
			return Finish(context, plan, "array", emptyType);
		}
		bool Zip(NodeContext &context) {
			auto storageCharge =
				context.ReserveWorkspace(context.Authored.DynamicInputs.size() * sizeof(Input));
			if (!storageCharge) return false;
			std::vector<Input> inputs;
			inputs.reserve(context.Authored.DynamicInputs.size());
			size_t length = Limits::MaximumArrayElements;
			bool hasArray = false;
			for (const auto &port : context.Authored.DynamicInputs) {
				inputs.emplace_back();
				if (!Read(context, port.Id, inputs.back())) return false;
				if (const auto *array = inputs.back().Array()) {
					hasArray = true;
					length = std::min(length, array->size());
				}
			}
			if (!hasArray || length == 0)
				return context.Fail(
					Status::InvalidValue, "source zip publishes no result without nonempty arrays", "output"
				);
			const bool spread = context.Boolean("spread_content");
			const auto select = [&](const Input &input, size_t at) -> const SourceArrayItem & {
				if (const auto *array = input.Array()) return (*array)[at];
				return at == 0 ? input.Root : Zero;
			};
			TreeCost cost;
			for (size_t row = 0; row < length; ++row) {
				if (!MeasureWrapper(cost))
					return context.Fail(
						Status::LimitExceeded, "source array output exceeds bounds", "output"
					);
				for (const auto &input : inputs) {
					const auto &item = select(input, row);
					const auto *children = spread ? std::get_if<Items>(&item.Data) : nullptr;
					if (children) {
						for (const auto &child : *children)
							if (!MeasureItem(child, cost, 2))
								return context.Fail(
									Status::LimitExceeded, "source array output exceeds bounds", "output"
								);
					} else if (!MeasureItem(item, cost, 2))
						return context.Fail(
							Status::LimitExceeded, "source array output exceeds bounds", "output"
						);
				}
			}
			auto outputCharge = context.ReserveWorkspace(cost.Bytes, "output");
			if (!outputCharge) return false;
			Items output;
			output.reserve(length);
			for (size_t row = 0; row < length; ++row) {
				size_t members = 0;
				for (const auto &input : inputs) {
					const auto &item = select(input, row);
					const auto *children = spread ? std::get_if<Items>(&item.Data) : nullptr;
					members += children ? children->size() : 1;
				}
				Items values;
				values.reserve(members);
				for (const auto &input : inputs) {
					const auto &item = select(input, row);
					const auto *children = spread ? std::get_if<Items>(&item.Data) : nullptr;
					if (children)
						values.insert(values.end(), children->begin(), children->end());
					else
						values.push_back(item);
				}
				output.push_back({std::move(values)});
			}
			return Publish(context, std::move(output), "output", ValueType::Integer);
		}
		bool Edit(NodeContext &context) {
			Input source, indices, value;
			if (!Read(context, "array", source) || !Read(context, "index", indices)) return false;
			const Items *input = source.Array();
			if (!input) return context.Fail(Status::InvalidValue, "source array input is missing", "array");
			const auto node = context.Authored.Type;
			if (node == "pc.array_get") return Get(context, *input, indices, source.EmptyType);
			if (!Read(context, "value", value)) return false;
			if (node == "pc.array_find" || node == "pc.array_remove")
				return QueryRemove(context, *input, indices, value, source.EmptyType);
			auto planCharge = context.ReserveWorkspace(
				Limits::MaximumArrayElements * sizeof(const SourceArrayItem *), "array"
			);
			if (!planCharge) return false;
			PlanItems plan;
			plan.reserve(Limits::MaximumArrayElements);
			for (const auto &item : *input)
				plan.push_back(&item);
			if (node == "pc.array_add") {
				auto inputsCharge =
					context.ReserveWorkspace(context.Authored.DynamicInputs.size() * sizeof(Input));
				if (!inputsCharge) return false;
				std::vector<Input> dynamic;
				dynamic.reserve(context.Authored.DynamicInputs.size());
				for (const auto &port : context.Authored.DynamicInputs) {
					dynamic.emplace_back();
					if (!Read(context, port.Id, dynamic.back())) return false;
					if (!Append(plan, dynamic.back().Root, context.Boolean("spread_array")))
						return context.Fail(
							Status::LimitExceeded, "source array output exceeds bounds", port.Id
						);
				}
				return Finish(context, plan, "output", source.EmptyType);
			}
			const Items *indexArray = indices.Array();
			const Items *values = value.Array();
			const size_t amount = indexArray ? indexArray->size() : 1;
			for (size_t i = 0; i < amount; ++i) {
				int64_t at;
				if (!Index(indexArray ? (*indexArray)[i] : indices.Root, at)) continue;
				if (at < 0) {
					if (node == "pc.array_insert" && indexArray && plan.empty())
						return context.Fail(
							Status::InvalidValue, "source insert index is outside bounds", "index"
						);
					at += int64_t(plan.size()) - ((node == "pc.array_insert" && indexArray) ? 1 : 0);
				}
				const SourceArrayItem *replacement =
					indexArray ? (values && !values->empty() ? &(*values)[i % values->size()]
								  : values					 ? &Zero
															 : &value.Root)
							   : &value.Root;
				if (node == "pc.array_set") {
					if (at < 0) continue;
					if (at >= int64_t(Limits::MaximumArrayElements))
						return context.Fail(
							Status::LimitExceeded, "source set index exceeds bounds", "index"
						);
					if (size_t(at) >= plan.size()) plan.resize(size_t(at) + 1, &Zero);
					plan[size_t(at)] = replacement;
				} else {
					const bool spread = !indexArray && context.Boolean("spread_array") && values;
					const size_t added = spread ? values->size() : 1;
					if (added == 0) continue;
					if (at < 0 || at > int64_t(plan.size()))
						return context.Fail(
							Status::InvalidValue, "source insert index is outside bounds", "index"
						);
					if (added > Limits::MaximumArrayElements - plan.size())
						return context.Fail(
							Status::LimitExceeded, "source array output exceeds bounds", "value"
						);
					if (spread)
						for (size_t member = 0; member < added; ++member)
							plan.insert(plan.begin() + at + int64_t(member), &(*values)[member]);
					else
						plan.insert(plan.begin() + at, replacement);
				}
			}
			return Finish(context, plan, "array", source.EmptyType);
		}
	} // namespace
	std::span<const ExecutorEntry> ArrayEditExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.array_add", Edit, true},
			ExecutorEntry{"pc.array_get", Edit, true},
			ExecutorEntry{"pc.array_set", Edit, true},
			ExecutorEntry{"pc.array_insert", Edit, true},
			ExecutorEntry{"pc.array_remove", Edit, true},
			ExecutorEntry{"pc.array_find", Edit, true},
			ExecutorEntry{"pc.array_zip", Zip, true},
			ExecutorEntry{"pc.array_unique", Unique, true},
			ExecutorEntry{"pc.array_uniform", Uniform, true},
			ExecutorEntry{"pc.array_rearrange", Rearrange, true},
			ExecutorEntry{"pc.array_sample", Sample, true},
			ExecutorEntry{"pc.array_shuffle", Shuffle, true},
			ExecutorEntry{"pc.array_randomizer", WeightedSelector, true},
			ExecutorEntry{"pc.array_boolean_opr", BooleanOperation, true},
			ExecutorEntry{"pc.array_pin", Pin, true},
			ExecutorEntry{"pc.array_split", Split, true}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
