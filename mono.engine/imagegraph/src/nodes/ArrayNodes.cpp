// The Array node spreads one outer level. Numeric rows retain their typed shape.

#include "../ValuePayload.hpp"
#include "Families.hpp"

#include <array>

namespace engine::imagegraph::detail {
	namespace {
		template <class T>
		bool AppendLeaf(NodeContext &context, ArrayValue &output, const T &leaf, std::string_view port) {
			const ValueType type = [&] {
				if constexpr (std::is_same_v<T, ElementValue>)
					return std::visit([](const auto &item) { return PayloadType(item); }, leaf);
				else
					return PayloadType(leaf);
			}();
			if (output.Elements.empty()) output.ElementType = type;
			if (type != output.ElementType)
				return context.Fail(
					Status::UnsupportedExecution, "Array requires homogeneous leaf types", port
				);
			const uint64_t bytes = sizeof(ElementValue) + [&] {
				if constexpr (std::is_same_v<T, ElementValue>)
					return std::visit([](const auto &item) { return PayloadOwnedBytes(item); }, leaf);
				else
					return PayloadOwnedBytes(leaf);
			}();
			if (output.Elements.size() >= Limits::MaximumArrayElements ||
				bytes > Limits::MaximumArrayBytes - PayloadOwnedBytes(output))
				return context.Fail(Status::LimitExceeded, "Array exceeds payload budget", port);
			output.Elements.push_back(leaf);
			return true;
		}

		bool AppendRow(
			NodeContext &context,
			ArrayValue &output,
			ValueType type,
			const std::vector<ElementValue> &row,
			std::string_view port
		) {
			if (output.Nested.empty()) output.ElementType = type;
			if (type != output.ElementType)
				return context.Fail(
					Status::UnsupportedExecution, "Array requires homogeneous row types", port
				);
			size_t count = 0;
			for (const auto &existing : output.Nested)
				count += existing.size();
			uint64_t bytes = sizeof(std::vector<ElementValue>) + row.size() * sizeof(ElementValue);
			for (const auto &leaf : row)
				bytes += std::visit([](const auto &item) { return PayloadOwnedBytes(item); }, leaf);
			if (output.Nested.size() >= Limits::MaximumArrayElements ||
				row.size() > Limits::MaximumArrayElements - count ||
				bytes > Limits::MaximumArrayBytes - PayloadOwnedBytes(output))
				return context.Fail(Status::LimitExceeded, "Array exceeds nested payload budget", port);
			output.Nested.push_back(row);
			return true;
		}

		bool ArrayNode(NodeContext &context) {
			for (const auto &input : context.Authored.DynamicInputs) {
				const Value *value = context.Find(input.Id);
				const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
				if (array && (array->ElementType == ValueType::Any || !array->Items.empty()))
					return CollectSourceArray(context);
			}
			const int64_t type = context.Integer("type");
			const bool spread = context.Boolean("spread_array");
			if (type < 0 || type > 4)
				return context.Fail(Status::InvalidValue, "Array type is invalid", "type");
			size_t flatCount = 0, rowCount = 0, imageCount = 0, itemCount = 0;
			uint64_t ownedBytes = std::max(std::string_view("array").size(), std::string{}.capacity());
			for (const auto &input : context.Authored.DynamicInputs) {
				const Image *image = context.Input(input.Id);
				const ImageArray *imageArray = nullptr;
				for (const auto &[port, array] : context.ImageArrays)
					if (port == input.Id) imageArray = array;
				const Value *value = context.Find(input.Id);
				if (image) {
					++imageCount;
					++itemCount;
					ownedBytes += image->Pixels.size();
				} else if (imageArray) {
					imageCount += imageArray->Images.size();
					itemCount += spread ? imageArray->Items.size() : 1;
					if (!spread) ownedBytes += imageArray->Items.size() * sizeof(ImageArrayItem);
					for (const Image &copy : imageArray->Images)
						ownedBytes += copy.Pixels.size();
				} else if (value) {
					if (!ValidRuntimeValue(*value))
						return context.Fail(Status::InvalidValue, "Array input payload is invalid", input.Id);
					if (const auto *array = std::get_if<ArrayValue>(value)) {
						if (spread && array->Nested.empty()) {
							flatCount += array->Elements.size();
							for (const auto &leaf : array->Elements)
								ownedBytes += std::visit(
									[](const auto &item) { return RetainedPayloadBytes(item); }, leaf
								);
						} else {
							rowCount += array->Nested.empty() ? 1 : array->Nested.size();
							const auto addRow = [&](const auto &row) {
								ownedBytes += row.size() * sizeof(ElementValue);
								for (const auto &leaf : row)
									ownedBytes += std::visit(
										[](const auto &item) { return RetainedPayloadBytes(item); }, leaf
									);
							};
							if (array->Nested.empty())
								addRow(array->Elements);
							else
								for (const auto &row : array->Nested)
									addRow(row);
						}
					} else {
						++flatCount;
						ownedBytes += RetainedPayloadBytes(*value);
					}
				}
				if (flatCount > Limits::MaximumArrayElements || rowCount > Limits::MaximumArrayElements ||
					imageCount > Limits::MaximumArrayElements || itemCount > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "Array exceeds retained shape budget", input.Id
					);
			}
			ownedBytes += flatCount * sizeof(ElementValue) + rowCount * sizeof(std::vector<ElementValue>) +
						  imageCount * sizeof(Image) + itemCount * sizeof(ImageArrayItem);
			if (!context.ReserveOutput(ownedBytes, "array")) return false;
			ArrayValue output{
				type == 3	? ValueType::Colour
				: type == 4 ? ValueType::Text
							: ValueType::Scalar,
				{}
			};
			ImageArray images;
			output.Elements.reserve(flatCount);
			output.Nested.reserve(rowCount);
			images.Images.reserve(imageCount);
			images.Items.reserve(itemCount);
			bool surface = type == 1, shapeChosen = type != 0;
			for (const auto &input : context.Authored.DynamicInputs) {
				const Image *image = context.Input(input.Id);
				const ImageArray *arrayImages = nullptr;
				for (const auto &[port, array] : context.ImageArrays)
					if (port == input.Id) arrayImages = array;
				const Value *value = context.Find(input.Id);
				if (!image && !arrayImages && !value)
					return context.Fail(Status::InvalidValue, "Array input is missing", input.Id);
				const bool inputSurface = image || arrayImages;
				if (!shapeChosen) {
					surface = inputSurface;
					shapeChosen = true;
				}
				if (surface != inputSurface)
					return context.Fail(
						Status::UnsupportedExecution, "Array cannot mix images and typed values", input.Id
					);
				if (surface) {
					const size_t copies = image ? 1 : arrayImages->Images.size();
					if (copies > Limits::MaximumArrayElements - images.Images.size())
						return context.Fail(Status::LimitExceeded, "Array exceeds image count", input.Id);
					const size_t itemCount = image ? 1 : arrayImages->Items.size();
					size_t shapeCount = images.Items.size();
					for (const auto &item : images.Items)
						if (const auto *children = std::get_if<std::vector<ImageArrayItem>>(&item.Data))
							shapeCount += children->size();
					const size_t addedItems = itemCount + ((arrayImages && !spread) ? 1 : 0);
					if (addedItems > Limits::MaximumArrayElements - shapeCount)
						return context.Fail(
							Status::LimitExceeded, "Array exceeds image shape count", input.Id
						);
					if (image) {
						images.Items.push_back({images.Images.size()});
						images.Images.push_back(*image);
					} else {
						const size_t offset = images.Images.size();
						auto childrenCharge = context.ReserveWorkspace(
							spread ? arrayImages->Items.size() * sizeof(ImageArrayItem) : 0, input.Id
						);
						if (!childrenCharge) return false;
						std::vector<ImageArrayItem> children;
						children.reserve(arrayImages->Items.size());
						for (const auto &item : arrayImages->Items) {
							const auto *leaf = std::get_if<size_t>(&item.Data);
							if (!leaf || *leaf >= arrayImages->Images.size())
								return context.Fail(
									Status::UnsupportedExecution,
									"Array supports one nested image level",
									input.Id
								);
							children.push_back({*leaf + offset});
						}
						if (spread)
							for (auto &item : children)
								images.Items.push_back(std::move(item));
						else
							images.Items.push_back({std::move(children)});
						for (const Image &item : arrayImages->Images)
							images.Images.push_back(item);
					}
					continue;
				}
				if (!ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "Array input payload is invalid", input.Id);
				const auto *array = std::get_if<ArrayValue>(value);
				const auto acceptType = [&](ValueType actual) {
					if (type == 3) return actual == ValueType::Colour;
					if (type == 4) return actual == ValueType::Text;
					if (type == 2)
						return actual == ValueType::Scalar || actual == ValueType::Integer ||
							   actual == ValueType::Boolean;
					return true;
				};
				if (!acceptType(array ? array->ElementType : PayloadType(*value)))
					return context.Fail(
						Status::TypeMismatch, "Array input does not match selected type", input.Id
					);
				if (!array) {
					if (!output.Nested.empty())
						return context.Fail(
							Status::UnsupportedExecution, "Array cannot mix scalar and nested rows", input.Id
						);
					if (!std::visit(
							[&](const auto &leaf) {
								if constexpr (std::is_constructible_v<ElementValue, decltype(leaf)>)
									return AppendLeaf(context, output, leaf, input.Id);
								else
									return context.Fail(
										Status::UnsupportedExecution,
										"Array input has no runtime leaf representation",
										input.Id
									);
							},
							*value
						))
						return false;
				} else if (spread && array->Nested.empty()) {
					if (!output.Nested.empty() && !array->Elements.empty())
						return context.Fail(
							Status::UnsupportedExecution, "Array cannot mix scalar and nested rows", input.Id
						);
					for (const auto &leaf : array->Elements)
						if (!AppendLeaf(context, output, leaf, input.Id)) return false;
				} else {
					if (!output.Elements.empty())
						return context.Fail(
							Status::UnsupportedExecution, "Array cannot mix scalar and nested rows", input.Id
						);
					if (!array->Nested.empty() && !spread)
						return context.Fail(
							Status::UnsupportedExecution,
							"Array output exceeds supported nested depth",
							input.Id
						);
					if (array->Nested.empty()) {
						if (!AppendRow(context, output, array->ElementType, array->Elements, input.Id))
							return false;
					} else
						for (const auto &row : array->Nested)
							if (!AppendRow(context, output, array->ElementType, row, input.Id)) return false;
				}
			}
			if (surface)
				context.OutputImageArrays.emplace_back("array", std::move(images));
			else
				context.SetValue("array", std::move(output));
			return true;
		}
	}

	std::span<const ExecutorEntry> ArrayExecutors() {
		static constexpr std::array ENTRIES{ExecutorEntry{"pc.array", ArrayNode, true}};
		return ENTRIES;
	}
}
