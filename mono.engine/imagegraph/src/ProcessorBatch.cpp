#include "ProcessorBatch.hpp"

#include "ArrayOps.hpp"
#include "FontTextBatch.hpp"
#include "SimulationAliases.hpp"
#include "SourceFontTransport.hpp"
#include "SourceGetterProjection.hpp"
#include "SourceLuaSockets.hpp"
#include "SourceMappedInputs.hpp"
#include "SourceRetainedImageOutputs.hpp"
#include "ValuePayload.hpp"
#include "nodes/SourceAnisoNoise.hpp"
#include "nodes/SourceBend.hpp"
#include "nodes/SourceCaustic.hpp"
#include "nodes/SourceCellular.hpp"
#include "nodes/SourceDisplace.hpp"
#include "nodes/SourceFoldNoise.hpp"
#include "nodes/SourceGaussianNoise.hpp"
#include "nodes/SourceGlow.hpp"
#include "nodes/SourceJpeg.hpp"
#include "nodes/SourceNoise.hpp"
#include "nodes/SourceNoiseCube.hpp"
#include "nodes/SourcePerlin.hpp"
#include "nodes/SourcePerlinExtra.hpp"
#include "nodes/SourcePixelMath.hpp"
#include "nodes/SourcePixelSort.hpp"
#include "nodes/SourcePolar.hpp"
#include "nodes/SourcePytagoreanTile.hpp"
#include "nodes/SourceScratchNoise.hpp"
#include "nodes/SourceShape3DExecutor.hpp"
#include "nodes/SourceShardNoise.hpp"
#include "nodes/SourceStrandNoise.hpp"
#include "nodes/SourceTileRandom.hpp"
#include "nodes/SourceTileTransform.hpp"
#include "nodes/SourceVoronoiExtra.hpp"
#include "nodes/SourceWaveletNoise.hpp"
#include "nodes/SourceWeave.hpp"

#include <algorithm>
#include <optional>

namespace engine::imagegraph::detail {
	namespace {
		std::string_view FamilyProfile(std::string_view type) {
			if (type == "pc.fft" || type == "pc.audio_window" || type == "pc.audio_loudness")
				return "imagegraph.node.audio";
			if (type.starts_with("pc.gradient_")) return "imagegraph.node.gradient";
			if (type == "pc.solid") return "imagegraph.node.generate";
			if (type == "pc.invert") return "imagegraph.node.filter";
			return "imagegraph.node.other";
		}

		bool Execute(NodeContext &context, Executor executor) {
			ENGINE_PROFILE_DYNAMIC_STABLE(
				"imagegraph.node", FamilyProfile(context.Authored.Type), core::ProfileCategory::Engine
			);
			core::Metrics::Count("imagegraph.node.executions", 1);
			return executor(context);
		}

		bool
		MoveGeneralItem(NodeContext &context, Value &value, std::string_view port, SourceArrayItem &item) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) {
				auto leaf = ArrayElement(std::move(value));
				if (!leaf)
					return context.Fail(Status::TypeMismatch, "processor value cannot enter an array", port);
				item.Data = std::move(*leaf);
				return true;
			}
			auto &source = std::get<ArrayValue>(value);
			if (!source.Items.empty()) {
				item.Data = std::move(source.Items);
				return true;
			}
			size_t nodes = source.Nested.empty() ? source.Elements.size() : source.Nested.size();
			for (const auto &children : source.Nested)
				nodes += children.size();
			if (nodes > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "processor generic array exceeds element budget", port
				);
			if (!context.ReserveOutput(nodes * sizeof(SourceArrayItem), port)) return false;
			std::vector<SourceArrayItem> members;
			if (source.Nested.empty()) {
				members.reserve(source.Elements.size());
				for (auto &leaf : source.Elements)
					members.push_back({std::move(leaf)});
			} else {
				members.reserve(source.Nested.size());
				for (auto &children : source.Nested) {
					std::vector<SourceArrayItem> entries;
					entries.reserve(children.size());
					for (auto &leaf : children)
						entries.push_back({std::move(leaf)});
					members.push_back({std::move(entries)});
				}
			}
			item.Data = std::move(members);
			return true;
		}

		struct InputRows {
			std::string_view Port;
			ValueType Type;
			int64_t Index;
			uint8_t Depth;
			const ArrayValue *Values = nullptr;
			const ImageArray *Images = nullptr;
			size_t Count = 1;
			bool Batch = false;
		};

		uint8_t LeafDepth(ValueType type) {
			switch (type) {
			case ValueType::Vector2:
			case ValueType::Vector3:
			case ValueType::Vector4:
			case ValueType::Quaternion:
			case ValueType::Area:
			case ValueType::Curve:
			case ValueType::Matrix:
				return 1;
			default:
				return 0;
			}
		}

		size_t GeneralDepth(const SourceArrayItem &item) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				return std::visit(
					[](const auto &data) { return size_t(LeafDepth(PayloadType(data))); }, *leaf
				);
			if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				return children->empty() ? 0 : 1 + GeneralDepth(children->front());
			return 0;
		}

		bool AddRows(
			NodeContext &context,
			const CatalogueInput &input,
			std::string_view port,
			int64_t index,
			std::vector<InputRows> &rows
		) {
			if (SourceMappedSynthetic(context.Entry, input)) return true;
			if (context.Entry.Type == "pc.font_data" && port == "font" &&
				!context.Boolean("attribute_process", true))
				return context.FailureCode == Status::Ok;
			const bool mapped = SourceRangeMapped(context, port);
			const bool hlslTuple =
				context.Entry.Type == "pc.hlsl" && port.starts_with("argument_value_") &&
				SourceArgumentType(
					context.Authored,
					port,
					context.Find(
						"argument_type_" +
						std::string(port.substr(std::string_view("argument_value_").size()))
					)
				) == ValueType::Array;
			InputRows selected{
				port, mapped ? ValueType::Any : input.Type, index, uint8_t(mapped ? 1 : input.ArrayDepth)
			};
			if (hlslTuple) {
				selected.Depth = 1;
				selected.Type = ValueType::Array;
			}
			for (const auto &[id, array] : context.ImageArrays)
				if (id == port) selected.Images = array;
			const Value *value = mapped ? SourceMappedRange(context, port) : context.Find(port);
			selected.Values = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!selected.Images && !selected.Values) return true;
			const bool occlusionUniformPair = context.Entry.Type == "pc.ambient_occlusion" &&
											  ((port == "height" && input.SourceKind == "Float") ||
											   (port == "intensity" && input.SourceKind == "Slider")) &&
											  !context.Boolean("attribute_process", true);
			if (occlusionUniformPair && selected.Values) {
				// With processing disabled, shader_set_f_map uploads
				// the original two-element array. Admit both controls
				// before execution, including inactive copies.
				const auto &pair = *selected.Values;
				if (!pair.Items.empty() || !pair.Nested.empty() || pair.Elements.size() != 2 ||
					(pair.ElementType != ValueType::Scalar && pair.ElementType != ValueType::Integer) ||
					!std::all_of(pair.Elements.begin(), pair.Elements.end(), [](const ElementValue &item) {
						return std::holds_alternative<double>(item) || std::holds_alternative<int64_t>(item);
					}))
					return context.Fail(
						Status::UnsupportedExecution,
						"AO disabled processing needs an exact "
						"two-number shader uniform",
						port
					);
				selected.Depth = 1;
			}
			const bool spriteShape = context.Entry.Type == "pc.sprite_stack" && port == "base_shape";
			if (spriteShape) {
				const auto *selector = context.Find("array_process");
				const auto mode = selector ? SourceChoiceNumber(*selector) : std::optional<double>{1};
				if (!mode || !std::isfinite(*mode))
					return context.Fail(
						Status::UnsupportedExecution,
						"source Sprite Stack array condition requires a scalar Array Process selector",
						"array_process"
					);
				// Source preGetInputs preserves the whole surface array in Combined mode.
				if (*mode != 0) return true;
				selected.Depth = 0;
			}
			const bool atlasDraw = context.Entry.Type == "pc.atlas_draw" && port == "input_1";
			if (atlasDraw) {
				const auto *selector = context.Find("combine");
				const auto *combine = selector ? std::get_if<bool>(selector) : nullptr;
				if (selector && !combine)
					return context.Fail(
						Status::UnsupportedExecution,
						"source Atlas Draw array depth requires a scalar Combine selector",
						"combine"
					);
				selected.Depth = !combine || *combine ? 1 : 0;
			}
			const bool shapeTexture = context.Entry.Type == "pc.shape_3_d" && port == "texture";
			if (shapeTexture) {
				selected.Depth = context.Boolean("array_texture", false) ? 1 : 0;
				if (context.FailureCode != Status::Ok) return false;
				if (selected.Images && selected.Depth == 1) {
					const auto &array = *selected.Images;
					bool nested = false;
					for (const auto &item : array.Items)
						nested = nested || std::holds_alternative<std::vector<ImageArrayItem>>(item.Data);
					const auto validLeaf = [&](const ImageArrayItem &item) {
						const auto *index = std::get_if<size_t>(&item.Data);
						return index && *index < array.Images.size();
					};
					for (const auto &item : array.Items) {
						if (!nested) {
							if (!validLeaf(item))
								return context.Fail(
									Status::InvalidValue, "Shape 3D texture list has an invalid surface", port
								);
						} else {
							const auto *children = std::get_if<std::vector<ImageArrayItem>>(&item.Data);
							if (!children || !std::all_of(children->begin(), children->end(), validLeaf))
								return context.Fail(
									Status::InvalidValue,
									"Shape 3D texture rows require flat surface lists",
									port
								);
						}
					}
					if (!nested) return true;
					selected.Count = array.Items.size();
					selected.Batch = true;
					rows.push_back(selected);
					return true;
				}
			}
			// Source Array Shift declares its array input depth 99 and consumes the entire shape.
			if (input.ArrayDepth >= Limits::MaximumArrayDepth) return true;
			if (context.Entry.Type == "pc.3_d_mesh_plane" && port == "both_side")
				return context.Fail(Status::UnsupportedExecution, "source Both Side rejects arrays", port);
			if (!mapped && !spriteShape && !atlasDraw && !hlslTuple && !occlusionUniformPair &&
				!shapeTexture && !input.ArrayDepthKnown)
				return context.Fail(
					Status::UnsupportedExecution, "source input array depth is dynamic", port
				);
			if (selected.Images && context.Entry.Type == "pc.surface_replace" &&
				(port == "target_image" || port == "replacement_image"))
				return true;
			if (selected.Images) {
				selected.Count = selected.Images->Items.size();
				selected.Batch = true;
				for (const auto &item : selected.Images->Items) {
					const auto *image = std::get_if<size_t>(&item.Data);
					if (!image || *image >= selected.Images->Images.size())
						return context.Fail(
							Status::UnsupportedExecution, "surface processor needs flat image rows", port
						);
				}
			} else if (!selected.Values->Items.empty()) {
				if (!ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "processor array payload is invalid", port);
				const auto &items = selected.Values->Items;
				const size_t depth = 1 + GeneralDepth(items.front());
				selected.Batch = depth > selected.Depth;
				selected.Count = items.size();
			} else {
				const auto leaf = selected.Values->ElementType;
				const bool numeric = leaf == ValueType::Scalar || leaf == ValueType::Integer;
				const bool supported =
					mapped || input.Type == ValueType::Any || input.Type == ValueType::Array ||
					leaf == input.Type ||
					(leaf == ValueType::Font && SourceFontInput(context.Entry.Type, port)) ||
					(leaf == ValueType::Path3D && port == "path" &&
					 (context.Entry.Type == "pc.path_sample" || context.Entry.Type == "pc.path_smoothen" ||
					  context.Entry.Type == "pc.path_spiral")) ||
					(leaf == ValueType::Atlas && input.Type == ValueType::Image) ||
					(numeric && (input.Type == ValueType::Scalar || input.Type == ValueType::Integer ||
								 input.Type == ValueType::Enum || input.Type == ValueType::Boolean ||
								 input.Type == ValueType::Vector2 || input.Type == ValueType::Vector3 ||
								 input.Type == ValueType::Vector4 || input.Type == ValueType::Quaternion));
				if (!supported)
					return context.Fail(
						Status::UnsupportedExecution,
						"processor array leaf type is unsupported for this input",
						port
					);
				if (!ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "processor array payload is invalid", port);
				const uint8_t depth =
					(selected.Values->Nested.empty() ? 1 : 2) + LeafDepth(selected.Values->ElementType);
				selected.Batch = depth > selected.Depth;
				selected.Count = selected.Values->Nested.empty() ? selected.Values->Elements.size()
																 : selected.Values->Nested.size();
			}
			if (selected.Batch && selected.Count == 0)
				return context.Fail(Status::InvalidValue, "empty processor input has no typed row", port);
			rows.push_back(selected);
			return true;
		}

		// Source inverse indexes a suffix table for every input, not only batched arrays.
		// Missing constructor positions cannot be reconstructed from a compact native array list.
		bool SourceSlotLengths(
			NodeContext &context, const std::vector<InputRows> &rows, std::vector<size_t> &lengths
		) {
			std::vector<size_t> indices;
			indices.reserve(context.Entry.Inputs.size() + context.Authored.DynamicInputs.size());
			const auto add = [&](int64_t index) {
				if (index < 0 || size_t(index) >= Limits::MaximumDynamicInputsPerNode) return false;
				indices.push_back(size_t(index));
				return true;
			};
			for (const CatalogueInput &input : context.Entry.Inputs) {
				if (SourceMappedSynthetic(context.Entry, input)) continue;
				if (input.SourceIndex < 0) {
					if (!input.Id.starts_with("attribute_") && input.SourceKind != "DimensionUnit")
						return context.Fail(
							Status::UnsupportedExecution,
							"source processor input slot layout is unresolved",
							input.Id
						);
					continue;
				}
				if (!add(input.SourceIndex))
					return context.Fail(
						Status::UnsupportedExecution,
						"source processor input slot layout is unresolved",
						input.Id
					);
			}
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *source = FindDynamicTemplate(context.Entry, input.Id, group);
				if (!source || context.Entry.DynamicGroupLength <= 0 || source->SourceIndex < 0 ||
					source->SourceIndex >= context.Entry.DynamicGroupLength ||
					group >= Limits::MaximumDynamicInputsPerNode ||
					!add(
						int64_t(context.Entry.DynamicFixedLength) +
						int64_t(group) * context.Entry.DynamicGroupLength + source->SourceIndex
					))
					return context.Fail(
						Status::UnsupportedExecution,
						"source processor dynamic slot layout is unresolved",
						input.Id
					);
			}
			std::sort(indices.begin(), indices.end());
			for (size_t slot = 0; slot < indices.size(); slot++)
				if (indices[slot] != slot)
					return context.Fail(
						Status::UnsupportedExecution,
						"source processor input slot layout is unresolved",
						"attribute_array_process"
					);
			lengths.assign(indices.size(), 1);
			for (const auto &input : rows) {
				if (input.Index < 0 || size_t(input.Index) >= lengths.size())
					return context.Fail(
						Status::UnsupportedExecution,
						"source processor input slot layout is unresolved",
						input.Port
					);
				if (input.Batch) lengths[size_t(input.Index)] = input.Count;
			}
			return true;
		}

		double Number(const ElementValue &element) {
			if (const auto *number = std::get_if<double>(&element)) return *number;
			if (const auto *number = std::get_if<int64_t>(&element)) return static_cast<double>(*number);
			return 0;
		}

		Value GeneralArray(const std::vector<SourceArrayItem> &children, ValueType target) {
			const bool vectorTarget = target == ValueType::Vector2 || target == ValueType::Vector3 ||
									  target == ValueType::Vector4 || target == ValueType::Quaternion;
			std::optional<ValueType> common;
			bool homogeneous = true, numeric = true;
			for (const auto &child : children) {
				const auto *leaf = std::get_if<ElementValue>(&child.Data);
				if (!leaf) {
					homogeneous = false;
					numeric = false;
					break;
				}
				const auto type = std::visit([](const auto &data) { return PayloadType(data); }, *leaf);
				if (common && *common != type) homogeneous = false;
				common = type;
				numeric = numeric && (type == ValueType::Scalar || type == ValueType::Integer);
			}
			ArrayValue value{ValueType::Any, {}};
			if (homogeneous || ((vectorTarget || target == ValueType::Array) && numeric)) {
				value.ElementType = (vectorTarget || !homogeneous) && numeric
										? ValueType::Scalar
										: common.value_or(ValueType::Scalar);
				value.Elements.reserve(children.size());
				for (const auto &child : children) {
					const auto &leaf = std::get<ElementValue>(child.Data);
					if ((vectorTarget || !homogeneous) && numeric)
						value.Elements.emplace_back(Number(leaf));
					else
						value.Elements.push_back(leaf);
				}
			} else
				value.Items = children;
			return value;
		}

		Value GeneralValue(const SourceArrayItem &item, ValueType target) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				return std::visit([](const auto &data) -> Value { return data; }, *leaf);
			return GeneralArray(std::get<std::vector<SourceArrayItem>>(item.Data), target);
		}

		Value NormalizeVector(Value value, ValueType target) {
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array || !array->Nested.empty() ||
				(array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer))
				return value;
			const auto component = [&](size_t index) {
				return index < array->Elements.size() ? Number(array->Elements[index]) : 0;
			};
			if (target == ValueType::Vector2) return Vector2{component(0), component(1)};
			if (target == ValueType::Vector3) return Vector3{component(0), component(1), component(2)};
			if (target == ValueType::Vector4)
				return Vector4{component(0), component(1), component(2), component(3)};
			if (target == ValueType::Quaternion)
				return Quaternion{component(0), component(1), component(2), component(3)};
			return value;
		}

		bool CheckOutputs(NodeContext &context, uint64_t maximumBytes) {
			uint64_t bytes = 0;
			if (context.OutputDiagnostics.size() >
				context.Entry.Outputs.size() + context.Authored.DynamicOutputs.size())
				return context.Fail(Status::InvalidOutput, "too many output diagnostics");
			for (size_t index = 0; index < context.OutputDiagnostics.size(); ++index) {
				const auto &diagnostic = context.OutputDiagnostics[index];
				const bool declared = std::any_of(
										  context.Entry.Outputs.begin(),
										  context.Entry.Outputs.end(),
										  [&](const auto &output) { return output.Id == diagnostic.Port; }
									  ) ||
									  std::any_of(
										  context.Authored.DynamicOutputs.begin(),
										  context.Authored.DynamicOutputs.end(),
										  [&](const auto &output) { return output.Id == diagnostic.Port; }
									  );
				if (!declared || diagnostic.Code != Status::UnsupportedExecution ||
					diagnostic.NodeId != context.Authored.Id || diagnostic.Message.empty())
					return context.Fail(Status::InvalidOutput, "invalid output diagnostic", diagnostic.Port);
				if (diagnostic.NodeId.size() > Limits::MaximumTextBytes ||
					diagnostic.Port.size() > Limits::MaximumTextBytes ||
					diagnostic.Message.size() > Limits::MaximumTextBytes)
					return context.Fail(
						Status::LimitExceeded, "output diagnostic text exceeds bounds", diagnostic.Port
					);
				for (size_t previous = 0; previous < index; ++previous)
					if (context.OutputDiagnostics[previous].Port == diagnostic.Port)
						return context.Fail(
							Status::InvalidOutput, "duplicate output diagnostic", diagnostic.Port
						);
				const uint64_t size =
					diagnostic.NodeId.capacity() + diagnostic.Port.capacity() + diagnostic.Message.capacity();
				if (size > maximumBytes - bytes)
					return context.Fail(
						Status::LimitExceeded,
						"processor output diagnostics exceed byte budget",
						diagnostic.Port
					);
				bytes += size;
				for (const auto &[port, image] : context.OutputImages)
					if (port == diagnostic.Port)
						return context.Fail(
							Status::InvalidOutput, "output payload conflicts with a refusal", port
						);
				for (const auto &[port, images] : context.OutputImageArrays)
					if (port == diagnostic.Port)
						return context.Fail(
							Status::InvalidOutput, "output payload conflicts with a refusal", port
						);
				for (const auto &value : context.OutputValues)
					if (value.Port == diagnostic.Port)
						return context.Fail(
							Status::InvalidOutput, "output payload conflicts with a refusal", value.Port
						);
			}
			for (const auto &[port, image] : context.OutputImages) {
				if (image.Pixels.size() > maximumBytes - bytes)
					return context.Fail(Status::LimitExceeded, "processor outputs exceed byte budget", port);
				bytes += image.Pixels.size();
			}
			for (const auto &[port, array] : context.OutputImageArrays) {
				if (array.Images.size() > Limits::MaximumArrayElements ||
					array.Items.size() > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "processor image array exceeds element budget", port
					);
				uint64_t size =
					array.Images.size() * sizeof(Image) + array.Items.size() * sizeof(ImageArrayItem);
				for (const Image &image : array.Images)
					size += image.Pixels.size();
				size_t items = array.Items.size();
				for (const ImageArrayItem &item : array.Items) {
					if (const auto *leaf = std::get_if<size_t>(&item.Data)) {
						if (*leaf >= array.Images.size())
							return context.Fail(Status::InvalidValue, "image array has invalid index", port);
					} else {
						const auto &children = std::get<std::vector<ImageArrayItem>>(item.Data);
						if (children.size() > Limits::MaximumArrayElements - items)
							return context.Fail(
								Status::LimitExceeded, "image array exceeds nested element budget", port
							);
						items += children.size();
						size += children.size() * sizeof(ImageArrayItem);
						for (const ImageArrayItem &child : children) {
							const auto *leaf = std::get_if<size_t>(&child.Data);
							if (!leaf || *leaf >= array.Images.size())
								return context.Fail(
									Status::UnsupportedExecution,
									"image array exceeds supported nested depth",
									port
								);
						}
					}
				}
				if (size > maximumBytes - bytes)
					return context.Fail(
						Status::LimitExceeded, "processor image arrays exceed byte budget", port
					);
				bytes += size;
			}
			for (const auto &value : context.OutputValues) {
				if (!ValidRuntimeValue(value.Data))
					return context.Fail(
						Status::InvalidValue, "processor produced an invalid typed value", value.Port
					);
				const uint64_t size = ValuePayloadBytes(value.Data);
				if (size > maximumBytes - bytes)
					return context.Fail(
						Status::LimitExceeded, "processor outputs exceed byte budget", value.Port
					);
				bytes += size;
			}
			return true;
		}

		struct PendingOutputs {
			NodeContext &Context;
			bool Committed = false;
			~PendingOutputs() {
				if (Committed) return;
				Context.ClearOutputs();
			}
		};

		struct RestoreInputs {
			NodeContext &Context;
			std::vector<std::pair<std::string_view, const Image *>> Images;
			std::vector<std::pair<std::string_view, const Value *>> Values;
			std::span<const std::pair<std::string_view, const Value *>> OriginalValues;
			size_t Row = Context.ProcessorRow, Count = Context.ProcessorCount;
			std::span<const SimulationReplayEntry> SimulationRows = Context.PendingSimulationRows;
			AllocationReservation AliasCharge = std::move(Context.SimulationAliasCharge);
			std::vector<std::pair<std::string_view, Value>> Aliases =
				std::move(Context.SimulationAliasValues);
			~RestoreInputs() {
				Context.SimulationAliasValues = std::move(Aliases);
				Context.SimulationAliasCharge = std::move(AliasCharge);
				Context.PendingSimulationRows = SimulationRows;
				Context.ProcessorRow = Row;
				Context.ProcessorCount = Count;
				Context.ProcessorOriginalValues = OriginalValues;
				Context.Images = std::move(Images);
				Context.ValueViews = std::move(Values);
			}
		};
	}

	bool RunProcessorBatch(NodeContext &context, Executor executor, ProcessorObserver observer, void *state) {
		ENGINE_PROFILE("imagegraph.processor");
		const uint64_t budget = context.ByteBudget;
		PendingOutputs pending{context};
		SourceGetterProjection getters(context);
		if (!getters.Prepare()) return false;
		struct RestoreBudget {
			NodeContext &Context;
			uint64_t Budget;
			~RestoreBudget() {
				Context.ByteBudget = Budget;
			}
		} restoreBudget{context, budget};
		const auto observe = [&] {
			if (!observer || observer(context, state)) return context.FailureCode == Status::Ok;
			if (context.FailureCode == Status::Ok)
				context.Fail(Status::UnsupportedExecution, "processor observer refused the selected row");
			return false;
		};
		bool retainedInactive = false;
		if (!SourceRetainedProcessorInactive(context, retainedInactive)) return false;
		if (retainedInactive) {
			pending.Committed = observe() && CheckOutputs(context, budget);
			return pending.Committed;
		}
		const bool processor = FindCatalogueInput(context.Entry, "attribute_process") != nullptr;
		if (!processor) {
			pending.Committed = Execute(context, executor) && context.FailureCode == Status::Ok &&
								observe() && context.FailureCode == Status::Ok &&
								CheckOutputs(context, budget);
			return pending.Committed;
		}
		const size_t maximumRows = context.Entry.Inputs.size() + context.Authored.DynamicInputs.size();
		const uint64_t selectionBytes =
			maximumRows * (sizeof(InputRows) + 2 * sizeof(size_t) + sizeof(Value) +
						   3 * sizeof(std::pair<std::string_view, const Value *>)) +
			2 * context.Images.size() * sizeof(std::pair<std::string_view, const Image *>) +
			2 * context.ValueViews.size() * sizeof(std::pair<std::string_view, const Value *>);
		auto selectionCharge = context.ReserveWorkspace(selectionBytes, "attribute_array_process");
		if (!selectionCharge) return false;
		std::vector<InputRows> rows;
		rows.reserve(maximumRows);
		for (const CatalogueInput &input : context.Entry.Inputs) {
			if (input.SourceIndex < 0) continue;
			if (!AddRows(context, input, input.Id, input.SourceIndex, rows)) return false;
		}
		for (const auto &input : context.Authored.DynamicInputs) {
			size_t group = 0;
			const auto *source = FindDynamicTemplate(context.Entry, input.Id, group);
			if (source && !AddRows(
							  context,
							  *source,
							  input.Id,
							  int64_t(context.Entry.DynamicFixedLength) +
								  int64_t(group) * context.Entry.DynamicGroupLength + source->SourceIndex,
							  rows
						  ))
				return false;
		}
		std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.Index < b.Index; });
		std::vector<size_t> lengths;
		lengths.reserve(maximumRows);
		for (const auto &input : rows)
			if (input.Batch) lengths.push_back(input.Count);
		if (!lengths.empty() && !context.Boolean("attribute_process", true))
			return context.Fail(
				Status::UnsupportedExecution,
				"native executor cannot consume unprocessed outer arrays",
				"attribute_process"
			);
		const int64_t mode = context.Integer("attribute_array_process");
		if (context.FailureCode != Status::Ok) return false;
		if (mode < 0 || mode > 3)
			return context.Fail(
				Status::InvalidValue, "array processor mode is invalid", "attribute_array_process"
			);
		const bool sourceInverse = mode == int64_t(ArrayProcessMode::ExpandInverse);
		if (sourceInverse && !lengths.empty() && !SourceSlotLengths(context, rows, lengths)) return false;
		auto scheduleCharge = context.ReserveWorkspace(0, "attribute_array_process");
		if (!scheduleCharge) return false;
		std::vector<std::vector<size_t>> schedule;
		if (!lengths.empty()) {
			ArrayScheduleFootprint footprint;
			const Status measured = MeasureArraySchedule(
				lengths, static_cast<ArrayProcessMode>(mode), Limits::MaximumArrayElements, footprint
			);
			if (measured != Status::Ok)
				return context.Fail(
					measured, "array processor exceeds row budget", "attribute_array_process"
				);
			auto charge = context.ReserveWorkspace(footprint.PeakBytes, "attribute_array_process");
			if (!charge) return false;
			if (!scheduleCharge->Merge(std::move(*charge))) std::terminate();
			const Status status = BuildSourceArraySchedule(
				lengths,
				static_cast<ArrayProcessMode>(mode),
				Limits::MaximumArrayElements,
				schedule,
				footprint.PeakBytes
			);
			if (status != Status::Ok)
				return context.Fail(status, "array processor exceeds row budget", "attribute_array_process");
			if (!scheduleCharge->Resize(footprint.RetainedBytes)) std::terminate();
		}
		const size_t count = schedule.empty() ? 1 : schedule.size();
		const uint64_t scheduleBytes =
			uint64_t(schedule.size()) * (sizeof(std::vector<size_t>) + lengths.size() * sizeof(size_t));
		const uint64_t rowViewsBytes =
			rows.size() * (sizeof(Value) + sizeof(std::pair<std::string_view, const Value *>));
		if (scheduleBytes > budget || rowViewsBytes > budget - scheduleBytes)
			return context.Fail(
				Status::LimitExceeded,
				"processor selection workspace exceeds byte budget",
				"attribute_array_process"
			);
		const uint64_t workspaceBytes = scheduleBytes + rowViewsBytes;
		const uint64_t shapeBytes = uint64_t(count) * context.Entry.Outputs.size() *
									(sizeof(ElementValue) + sizeof(ImageArrayItem) + sizeof(Image));
		if (count > 1 && shapeBytes > budget - workspaceBytes)
			return context.Fail(
				Status::LimitExceeded,
				"array processor output shape exceeds byte budget",
				"attribute_array_process"
			);
		auto aggregateCharge = context.ReserveWorkspace(
			count > 1 ? shapeBytes + context.Entry.Outputs.size() *
										 (sizeof(AuthoredValue) + sizeof(std::pair<std::string, ImageArray>))
					  : 0,
			"attribute_array_process"
		);
		if (!aggregateCharge) return false;
		RestoreInputs restore{context, context.Images, context.ValueViews, context.ProcessorOriginalValues};
		context.ProcessorOriginalValues = restore.Values;
		auto selectedCharge = context.ReserveWorkspace(0, "attribute_array_process");
		if (!selectedCharge) return false;
		std::vector<Value> selected;
		selected.reserve(rows.size());
		context.ValueViews.reserve(rows.size() + restore.Values.size());
		std::vector<std::pair<std::string, ImageArray>> images;
		std::vector<AuthoredValue> values;
		std::vector<Diagnostic> diagnostics;
		const auto blocked = [&](std::string_view port) {
			return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto &diagnostic) {
				return diagnostic.Port == port;
			});
		};
		if (count > 1) {
			images.reserve(context.Entry.Outputs.size());
			values.reserve(context.Entry.Outputs.size());
		}
		auto replayRowsCharge = context.ReserveWorkspace(0, "attribute_array_process");
		if (!replayRowsCharge) return false;
		std::vector<SimulationReplayEntry> simulationRows;
		std::vector<SurfaceFrameReplayEntry> surfaceRows;
		std::vector<RandomReplayEntry> randomRows;
		std::vector<DataReplayEntry> dataRows;
		const auto admitReplayRows = [&](auto &target, const auto &updates, size_t capacity) {
			if (updates.empty() || target.capacity()) return true;
			using Entry = typename std::decay_t<decltype(target)>::value_type;
			auto charge = context.ReserveWorkspace(capacity * sizeof(Entry), "attribute_array_process");
			if (!charge) return false;
			if (!replayRowsCharge->Merge(std::move(*charge))) std::terminate();
			target.reserve(capacity);
			return true;
		};

		context.ProcessorCount = count;
		uint64_t accumulated = workspaceBytes + (count > 1 ? shapeBytes : 0);
		const auto selectRow = [&](size_t row, uint64_t &scratchOwned) -> bool {
			selected.clear();
			selectedCharge = context.ReserveWorkspace(0, "attribute_array_process");
			if (!selectedCharge) return false;
			context.ValueViews = restore.Values;
			context.Images = restore.Images;
			context.ClearOutputs();
			context.ProcessorRow = row;
			context.ByteBudget = budget;
			size_t slot = 0;
			scratchOwned = 0;
			for (const InputRows &input : rows) {
				const size_t index =
					input.Batch ? schedule[row][sourceInverse ? size_t(input.Index) : slot++] : 0;
				if (input.Images) {
					if (context.Entry.Type == "pc.shape_3_d" && input.Port == "texture" && input.Depth == 1) {
						// Borrow the selected nested texture list through an internal row view.
						selected.emplace_back(int64_t(index));
						context.ValueViews.emplace_back("__shape3d_texture_row", &selected.back());
						continue;
					}
					const size_t imageIndex = std::get<size_t>(input.Images->Items[index].Data);
					context.Images.emplace_back(input.Port, &input.Images->Images[imageIndex]);
				} else {
					const ArrayValue &array = *input.Values;
					if (!input.Batch && array.Items.empty() && input.Type != ValueType::Vector2 &&
						input.Type != ValueType::Vector3 && input.Type != ValueType::Vector4 &&
						input.Type != ValueType::Quaternion)
						continue;
					uint64_t scratchBytes = 0;
					if (input.Batch && !array.Items.empty()) {
						const auto &general = array.Items[index];
						if (const auto *image = std::get_if<Image>(&general.Data)) {
							if (input.Type != ValueType::Image && input.Type != ValueType::Any)
								return context.Fail(
									Status::UnsupportedExecution,
									"selected surface requires an unsupported source conversion",
									input.Port
								);
							context.Images.emplace_back(input.Port, image);
							continue;
						}
						scratchBytes = RetainedPayloadBytes(general);
					} else if (input.Batch && !array.Nested.empty()) {
						for (const auto &element : array.Nested[index])
							scratchBytes +=
								sizeof(ElementValue) +
								std::visit(
									[](const auto &leaf) { return RetainedPayloadBytes(leaf); }, element
								);
					} else if (input.Batch)
						scratchBytes = std::visit(
							[](const auto &leaf) { return RetainedPayloadBytes(leaf); }, array.Elements[index]
						);
					else
						scratchBytes = RetainedPayloadBytes(array);
					if (scratchBytes > context.ByteBudget - scratchOwned)
						return context.Fail(
							Status::LimitExceeded, "processor selected row exceeds byte budget", input.Port
						);
					scratchOwned += scratchBytes;
					auto charge = context.ReserveWorkspace(scratchBytes, input.Port);
					if (!charge || !selectedCharge->Merge(std::move(*charge))) return false;
					Value item =
						!input.Batch && !array.Items.empty()  ? GeneralArray(array.Items, input.Type)
						: input.Batch && !array.Items.empty() ? GeneralValue(array.Items[index], input.Type)
						: input.Batch
							? (!array.Nested.empty()
								   ? Value{ArrayValue{array.ElementType, array.Nested[index]}}
								   : std::visit(
										 [](const auto &leaf) -> Value { return leaf; }, array.Elements[index]
									 ))
							: Value{array};
					item = NormalizeVector(std::move(item), input.Type);
					const bool lookAtControl = context.Entry.Type == "pc.quarternion_lookat" &&
											   (input.Port == "origin" || input.Port == "target" ||
												input.Port == "up" || input.Port == "unit");
					// Look At's source getters accept scalar coordinates and numeric selector leaves.
					if (const auto kind = PayloadType(item);
						!lookAtControl && !SourceFontInput(context.Entry.Type, input.Port) &&
						input.Type != ValueType::Any && input.Type != ValueType::Array &&
						kind != input.Type && !(kind == ValueType::Array && input.Depth > 0) &&
						!(kind == ValueType::Path3D && input.Port == "path" &&
						  (context.Entry.Type == "pc.path_sample" ||
						   context.Entry.Type == "pc.path_smoothen" ||
						   context.Entry.Type == "pc.path_spiral")) &&
						!(kind == ValueType::Atlas && input.Type == ValueType::Image &&
						  (context.Entry.Type == "pc.wrap_area" || context.Entry.Type == "pc.bend" ||
						   context.Entry.Type == "pc.pixel_math") &&
						  input.Port == "surface_in") &&
						!((kind == ValueType::Scalar || kind == ValueType::Integer) &&
						  (input.Type == ValueType::Scalar || input.Type == ValueType::Integer ||
						   input.Type == ValueType::Enum || input.Type == ValueType::Boolean ||
						   input.Type == ValueType::Vector2)))
						return context.Fail(
							Status::UnsupportedExecution,
							"selected processor leaf requires an unsupported conversion",
							input.Port
						);

					selected.push_back(std::move(item));
					context.ValueViews.emplace_back(input.Port, &selected.back());
				}
			}
			return true;
		};
		FontTextBatch textBatch;
		struct RestoreTextBatch {
			NodeContext &Context;
			FontTextBatch *Previous;
			~RestoreTextBatch() {
				Context.TextBatch = Previous;
			}
		} restoreTextBatch{context, context.TextBatch};
		if (context.Authored.Type == "pc.text") {
			context.TextBatch = &textBatch;
			if (!BeginFontTextBatch(context, count, textBatch)) return false;
			for (size_t row = 0; row < count; ++row) {
				uint64_t scratchOwned = 0;
				if (!selectRow(row, scratchOwned) || !QuoteFontTextMeasurementRow(context, textBatch))
					return false;
			}
			for (size_t row = 0; row < count; ++row) {
				uint64_t scratchOwned = 0;
				if (!selectRow(row, scratchOwned) || !PrepareFontTextRow(context, textBatch)) return false;
			}
			selected.clear();
			selectedCharge->Reset();
			if (!AdmitFontTextBatch(context, textBatch)) return false;
		}
		const auto previousTileReference = context.TileReferenceDimension;
		struct RestoreTileReference {
			NodeContext &Context;
			std::optional<Vector2> Previous;
			~RestoreTileReference() {
				Context.TileReferenceDimension = Previous;
			}
		} restoreTileReference{context, previousTileReference};
		if (context.Authored.Type == "pc.tile") {
			size_t previewIndex = 0;
			uint64_t scratchOwned = 0;
			Vector2 reference;
			if (!SourceTilePreviewIndex(context, count, previewIndex) ||
				!selectRow(previewIndex, scratchOwned) || !SourceTileReferenceDimension(context, reference))
				return false;
			context.TileReferenceDimension = reference;
		}
		struct RestoreDisplaceReference {
			NodeContext &Context;
			std::optional<Vector2> Previous;
			~RestoreDisplaceReference() {
				Context.DisplaceReferenceDimension = Previous;
			}
		} restoreDisplaceReference{context, context.DisplaceReferenceDimension};
		if (context.Authored.Type == "pc.displace") {
			uint64_t scratchOwned = 0;
			if (!selectRow(0, scratchOwned)) return false;
			const auto *source = context.Input("surface_in");
			if (!source)
				return context.Fail(Status::InvalidValue, "Displace requires Surface In", "surface_in");
			context.DisplaceReferenceDimension = Vector2{double(source->Width), double(source->Height)};
		}
		const auto admission =
			context.Authored.Type == "pc.bend"				? AdmitSourceBend
			: context.Authored.Type == "pc.pixel_math"		? AdmitSourcePixelMath
			: context.Authored.Type == "pc.glow"			? AdmitSourceGlow
			: context.Authored.Type == "pc.displace"		? AdmitSourceDisplace
			: context.Authored.Type == "pc.jpeg"			? AdmitSourceJpeg
			: context.Authored.Type == "pc.pixel_sort"		? AdmitSourcePixelSort
			: context.Authored.Type == "pc.noise"			? AdmitSourceNoise
			: context.Authored.Type == "pc.caustic"			? AdmitSourceCaustic
			: context.Authored.Type == "pc.cellular"		? AdmitSourceCellular
			: context.Authored.Type == "pc.perlin"			? AdmitSourcePerlin
			: context.Authored.Type == "pc.voronoi_extra"	? AdmitSourceVoronoiExtra
			: context.Authored.Type == "pc.shard_noise"		? AdmitSourceShardNoise
			: context.Authored.Type == "pc.noise_strand"	? AdmitSourceStrandNoise
			: context.Authored.Type == "pc.weave"			? AdmitSourceWeave
			: context.Authored.Type == "pc.pytagorean_tile" ? AdmitSourcePytagoreanTile
			: context.Authored.Type == "pc.noise_gaussian"	? AdmitSourceGaussianNoise
			: context.Authored.Type == "pc.noise_aniso"		? AdmitSourceAnisoNoise
			: context.Authored.Type == "pc.fold_noise"		? AdmitSourceFoldNoise
			: context.Authored.Type == "pc.noise_scratch"	? AdmitSourceScratchNoise
			: context.Authored.Type == "pc.wavelet_noise"	? AdmitSourceWaveletNoise
			: context.Authored.Type == "pc.perlin_extra"	? AdmitSourcePerlinExtra
			: (context.Authored.Type == "pc.perlin_cube" || context.Authored.Type == "pc.cellular_cube" ||
			   context.Authored.Type == "pc.simplex_cube")
				? AdmitSourceNoiseCube
			: context.Authored.Type == "pc.polar"		? AdmitSourcePolar
			: context.Authored.Type == "pc.shape_3_d"	? AdmitSourceShape3D
			: context.Authored.Type == "pc.tile_random" ? AdmitSourceTileRandom
			: context.Authored.Type == "pc.tile"		? AdmitSourceTileTransform
														: nullptr;
		if (admission) {
			uint64_t batchWork = 0;
			for (size_t row = 0; row < count; ++row) {
				uint64_t scratchOwned = 0;
				if (!selectRow(row, scratchOwned)) return false;
				if (!admission(context, batchWork)) return false;
			}
		}
		for (size_t row = 0; row < count; row++) {
			uint64_t scratchOwned = 0;
			if (!selectRow(row, scratchOwned)) return false;
			if (!simulationRows.empty()) {
				context.PendingSimulationRows = simulationRows;
				if (!ResolveSimulationInputAliases(context)) return false;
			}
			if (!Execute(context, executor) || context.FailureCode != Status::Ok || !observe() ||
				context.FailureCode != Status::Ok ||
				!CheckOutputs(context, budget - accumulated - scratchOwned))
				return false;
			if (count > 1 && !context.OutputImageArrays.empty())
				return context.Fail(
					Status::UnsupportedExecution, "processor output exceeds supported image array depth"
				);
			if (count == 1) {
				selected.clear();
				if (!CaptureSourceRetainedProcessorOutputs(context)) return false;
				pending.Committed = true;
				return true;
			}
			if (context.PixelBuilderUpdate)
				return context.Fail(
					Status::UnsupportedExecution, "Pixel Builder layer commands cannot be processor arrays"
				);
			if (context.SimulationUpdates.size() > 2 || context.SurfaceUpdates.size() > 1 ||
				context.RandomUpdates.size() > 1 || context.DataUpdates.size() > 1)
				return context.Fail(Status::LimitExceeded, "processor row exceeds its replay entry bound");
			if (!admitReplayRows(simulationRows, context.SimulationUpdates, count * 2) ||
				!admitReplayRows(surfaceRows, context.SurfaceUpdates, count) ||
				!admitReplayRows(randomRows, context.RandomUpdates, count) ||
				!admitReplayRows(dataRows, context.DataUpdates, count))
				return false;
			for (auto &update : context.SimulationUpdates) {
				const uint64_t retained = RetainedSimulationEntryBytes(update);
				if (retained > budget - accumulated)
					return context.Fail(
						Status::LimitExceeded, "processor simulation snapshots exceed byte budget"
					);
				accumulated += retained;
				simulationRows.push_back(std::move(update));
			}
			for (auto &update : context.SurfaceUpdates) {
				const uint64_t retained = RetainedSurfaceFrameEntryBytes(update);
				if (retained > budget - accumulated)
					return context.Fail(
						Status::LimitExceeded, "processor surface snapshots exceed byte budget"
					);
				accumulated += retained;
				surfaceRows.push_back(std::move(update));
			}
			for (auto &update : context.RandomUpdates) {
				const uint64_t retained = RetainedRandomEntryBytes(update);
				if (retained > budget - accumulated)
					return context.Fail(
						Status::LimitExceeded, "processor random snapshots exceed byte budget"
					);
				accumulated += retained;
				randomRows.push_back(std::move(update));
			}
			for (auto &update : context.DataUpdates) {
				const uint64_t retained = RetainedDataReplayEntryBytes(update);
				if (retained > budget - accumulated)
					return context.Fail(Status::LimitExceeded, "processor data snapshots exceed byte budget");
				accumulated += retained;
				dataRows.push_back(std::move(update));
			}

			if (!context.OutputDiagnostics.empty() && !diagnostics.capacity()) {
				const size_t slots = context.Entry.Outputs.size() + context.Authored.DynamicOutputs.size();
				auto charge = context.ReserveWorkspace(slots * sizeof(Diagnostic), "attribute_array_process");
				if (!charge || !aggregateCharge->Merge(std::move(*charge))) return false;
				diagnostics.reserve(slots);
			}
			for (auto &diagnostic : context.OutputDiagnostics) {
				if (blocked(diagnostic.Port)) {
					const uint64_t strings = diagnostic.NodeId.capacity() + diagnostic.Port.capacity() +
											 diagnostic.Message.capacity();
					{
						Diagnostic dropped;
						std::swap(diagnostic, dropped);
					}
					auto released = context.OutputCharge.Split(strings);
					continue;
				}
				const auto oldImage = std::find_if(images.begin(), images.end(), [&](const auto &item) {
					return item.first == diagnostic.Port;
				});
				if (oldImage != images.end()) {
					uint64_t retained = oldImage->second.Images.capacity() * sizeof(Image) +
										oldImage->second.Items.capacity() * sizeof(ImageArrayItem);
					for (const auto &image : oldImage->second.Images)
						retained += image.Pixels.capacity();
					images.erase(oldImage);
					auto released = aggregateCharge->Split(retained);
				}
				const auto oldValue = std::find_if(values.begin(), values.end(), [&](const auto &item) {
					return item.Port == diagnostic.Port;
				});
				if (oldValue != values.end()) {
					const uint64_t retained = RetainedPayloadBytes(oldValue->Data);
					values.erase(oldValue);
					auto released = aggregateCharge->Split(retained);
				}
				diagnostics.push_back(std::move(diagnostic));
			}
			context.OutputDiagnostics.clear();
			for (auto &[port, image] : context.OutputImages) {
				if (blocked(port)) {
					const uint64_t retained = image.Pixels.capacity() + port.capacity();
					image = {};
					std::string{}.swap(port);
					auto released = context.OutputCharge.Split(retained);
					continue;
				}
				auto target = std::find_if(images.begin(), images.end(), [&](const auto &item) {
					return item.first == port;
				});
				if (target == images.end()) {
					if (!context.ReserveOutput(port.size(), port)) return false;
					images.emplace_back(port, ImageArray{});
					target = images.end() - 1;
					target->second.Images.reserve(count);
					target->second.Items.reserve(count);
				}
				if (image.Pixels.size() > budget - accumulated)
					return context.Fail(
						Status::LimitExceeded, "processor image array exceeds byte budget", port
					);
				accumulated += image.Pixels.size();
				target->second.Items.push_back({target->second.Images.size()});
				target->second.Images.push_back(std::move(image));
			}
			for (auto &value : context.OutputValues) {
				if (blocked(value.Port)) {
					const uint64_t retained = RetainedPayloadBytes(value.Data) + value.Port.capacity();
					{
						AuthoredValue dropped;
						std::swap(value, dropped);
					}
					auto released = context.OutputCharge.Split(retained);
					continue;
				}
				// Source Look At can alternate between Euler triples and quaternion tuples per row.
				const bool generic =
					(context.Authored.Type == "pc.quarternion_lookat" && value.Port == "rotation") ||
					(context.Authored.Type == "pc.font_data" && value.Port == "font") ||
					std::any_of(
						context.Entry.Outputs.begin(), context.Entry.Outputs.end(), [&](const auto &output) {
							return output.Id == value.Port && output.Type == ValueType::Any;
						}
					);
				auto target = std::find_if(values.begin(), values.end(), [&](const auto &item) {
					return item.Port == value.Port;
				});
				if (target == values.end()) {
					if (!context.ReserveOutput(value.Port.size(), value.Port)) return false;
					values.push_back({value.Port, ArrayValue{PayloadType(value.Data), {}}});
					target = values.end() - 1;
					auto &array = std::get<ArrayValue>(target->Data);
					if (generic) {
						if (!context.ReserveOutput(count * sizeof(SourceArrayItem), value.Port)) return false;
						array.ElementType = ValueType::Any;
						array.Items.reserve(count);
					} else if (std::holds_alternative<ArrayValue>(value.Data) &&
							   context.Authored.Type == "pc.array_shift") {
						if (!context.ReserveOutput(count * sizeof(SourceArrayItem), value.Port)) return false;
						array.ElementType = ValueType::Any;
						array.Items.reserve(count);
					} else if (std::holds_alternative<ArrayValue>(value.Data))
						array.Nested.reserve(count);
					else
						array.Elements.reserve(count);
				}
				auto &array = std::get<ArrayValue>(target->Data);
				const uint64_t owned =
					std::visit([](const auto &item) { return PayloadOwnedBytes(item); }, value.Data);
				if (owned > budget - accumulated)
					return context.Fail(
						Status::LimitExceeded, "processor typed array exceeds byte budget", value.Port
					);
				const uint64_t growth = std::holds_alternative<ArrayValue>(value.Data)
											? owned + sizeof(std::vector<ElementValue>)
											: owned + sizeof(ElementValue);
				if (growth > Limits::MaximumArrayBytes - PayloadOwnedBytes(array))
					return context.Fail(
						Status::LimitExceeded, "processor typed array exceeds 4 MiB", value.Port
					);
				accumulated += owned;
				if (generic) {
					SourceArrayItem item;
					if (!MoveGeneralItem(context, value.Data, value.Port, item)) return false;
					array.Items.push_back(std::move(item));
					if (PayloadOwnedBytes(array) > Limits::MaximumArrayBytes)
						return context.Fail(
							Status::LimitExceeded, "processor generic array exceeds 4 MiB", value.Port
						);
					continue;
				}
				if (auto *nested = std::get_if<ArrayValue>(&value.Data)) {
					if (context.Authored.Type == "pc.array_shift") {
						std::vector<SourceArrayItem> members;
						if (!nested->Items.empty())
							members = std::move(nested->Items);
						else if (nested->Nested.empty()) {
							if (!context.ReserveOutput(
									nested->Elements.size() * sizeof(SourceArrayItem), value.Port
								))
								return false;
							members.reserve(nested->Elements.size());
							for (auto &leaf : nested->Elements)
								members.push_back({std::move(leaf)});
						} else {
							size_t nodes = nested->Nested.size();
							for (const auto &children : nested->Nested)
								nodes += children.size();
							if (!context.ReserveOutput(nodes * sizeof(SourceArrayItem), value.Port))
								return false;
							members.reserve(nested->Nested.size());
							for (auto &children : nested->Nested) {
								std::vector<SourceArrayItem> entries;
								entries.reserve(children.size());
								for (auto &leaf : children)
									entries.push_back({std::move(leaf)});
								members.push_back({std::move(entries)});
							}
						}
						array.Items.push_back({std::move(members)});
						continue;
					}
					if (!nested->Nested.empty())
						return context.Fail(
							Status::UnsupportedExecution,
							"processor output exceeds supported nested array depth",
							value.Port
						);
					if (!array.Elements.empty() || (row && array.ElementType != nested->ElementType))
						return context.Fail(
							Status::InvalidValue,
							"processor output array shape changed between rows",
							value.Port
						);
					array.ElementType = nested->ElementType;
					array.Nested.push_back(std::move(nested->Elements));
				} else {
					if (!array.Nested.empty() || array.ElementType != PayloadType(value.Data))
						return context.Fail(
							Status::InvalidValue,
							"processor output value type changed between rows",
							value.Port
						);
					auto element = ArrayElement(std::move(value.Data));
					array.Elements.push_back(std::move(*element));
				}
			}
			if (!aggregateCharge->Merge(std::move(context.OutputCharge))) std::terminate();
			selected.clear();
		}
		context.ClearOutputs();
		std::vector<std::pair<std::string, Image>>{}.swap(context.OutputImages);
		std::vector<Diagnostic>{}.swap(context.OutputDiagnostics);
		context.SimulationUpdates = std::move(simulationRows);
		context.SurfaceUpdates = std::move(surfaceRows);
		context.RandomUpdates = std::move(randomRows);
		context.DataUpdates = std::move(dataRows);
		context.OutputValues = std::move(values);
		context.OutputImageArrays = std::move(images);
		context.OutputDiagnostics = std::move(diagnostics);
		context.ReplaceOutputReservation(std::move(*aggregateCharge));
		for (const auto &value : context.OutputValues)
			if (!ValidRuntimeValue(value.Data))
				return context.Fail(
					Status::LimitExceeded, "processor typed array exceeds payload limits", value.Port
				);
		if (!CaptureSourceRetainedProcessorOutputs(context)) return false;
		context.ByteBudget = budget - accumulated;
		pending.Committed = true;
		return true;
	}
}
