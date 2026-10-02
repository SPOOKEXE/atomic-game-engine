#include "../HostCaptureReceipts.hpp"
#include "Families.hpp"
#include "Processor.hpp"

#include <engine/imagegraph/HostCapture.hpp>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace engine::imagegraph::detail {
	namespace {
		bool CopyImage(NodeContext &context, std::string_view port, const Image &source) {
			if (!ValidSurfaceLayout(
					source, context.Request.MaximumImageDimension, Limits::MaximumOutputBytes
				) ||
				!FiniteSurfaceSamples(source))
				return context.Fail(Status::InvalidValue, "recorded host surface storage is invalid", port);
			auto *result = context.NewImage(port, source.Width, source.Height, source.Format);
			if (!result) return false;
			std::copy(source.Pixels.begin(), source.Pixels.end(), result->Pixels.begin());
			result->Hash = SurfaceHash(*result);
			return true;
		}
		bool RecordedHost(NodeContext &context) {
			if (context.Request.HostCaptures.size() > Limits::MaximumNodes)
				return context.Fail(Status::LimitExceeded, "host recording count exceeds its budget");
			const HostNodeCapture *capture = nullptr;
			for (const auto &record : context.Request.HostCaptures) {
				if (record.Authored.Id != context.Authored.Id || record.Tick != context.Request.Tick ||
					record.Subframe != context.Request.Subframe ||
					record.NegativeFrame != context.Request.NegativeFrame)
					continue;
				if (capture)
					return context.Fail(Status::DuplicateId, "host recording node and time are duplicated");
				capture = &record;
			}
			HostNodeCapture live;
			AllocationReservation liveInputs;
			if (!capture && context.Request.HostProvider) {
				uint64_t bytes = (context.Values.size() + context.ValueViews.size()) * sizeof(AuthoredValue) +
								 context.Images.size() * sizeof(HostResolvedImage);
				for (const auto &[port, value] : context.Values)
					bytes += port.size() +
							 std::visit([](const auto &item) { return RetainedPayloadBytes(item); }, value);
				for (const auto &[port, value] : context.ValueViews)
					if (value)
						bytes +=
							port.size() +
							std::visit([](const auto &item) { return RetainedPayloadBytes(item); }, *value);
				auto charge = context.ReserveWorkspace(bytes, "host_inputs");
				if (!charge) return false;
				liveInputs = std::move(*charge);
				std::vector<AuthoredValue> values;
				std::vector<HostResolvedImage> images;
				for (const auto &[port, value] : context.Values)
					if (context.Find(port) == &value) values.push_back({std::string(port), value});
				for (const auto &[port, value] : context.ValueViews)
					if (value && context.Find(port) == value) values.push_back({std::string(port), *value});
				for (const auto &[port, image] : context.Images)
					images.push_back({port, image});
				std::optional<SurfaceFormat> outputFormat = context.InheritedSurfaceFormat;
				if (context.Authored.Type == "pc.lua_surface") {
					if (context.Integer("attribute_color_depth", 1) == 0)
						return context.Fail(
							Status::UnsupportedExecution,
							"Lua Surface source precision requires an image source",
							"attribute_color_depth"
						);
					outputFormat = ResolveProcessorSurfaceFormat(context, nullptr);
					if (!outputFormat) return false;
				}
				std::string failure;
				if (!context.Request.HostProvider->Capture(
						{context.Authored,
						 context.Request,
						 values,
						 images,
						 context.AvailableBytes(),
						 context.Timeline,
						 outputFormat,
						 context.InheritedInterpolation},
						live,
						failure
					))
					return context.Fail(
						Status::UnsupportedExecution, failure.empty() ? "host capability failed" : failure
					);
				capture = &live;
			}

			if (!capture)
				return context.Fail(
					Status::UnsupportedExecution,
					"node needs explicit host capability input or a recorded host result; ambient execution "
					"is unavailable"
				);
			if (capture->Authored != context.Authored)
				return context.Fail(Status::InvalidValue, "host recording authored node is stale");
			if (capture->Tick != context.Request.Tick || capture->Subframe != context.Request.Subframe ||
				capture->NegativeFrame != context.Request.NegativeFrame)
				return context.Fail(Status::InvalidValue, "host recording authored time is stale");
			if (capture->Inputs.size() > Limits::MaximumLinks ||
				capture->InputImages.size() > Limits::MaximumLinks ||
				(capture->State == HostCaptureState::Recorded &&
				 capture->Outputs.size() + capture->Images.size() + capture->ImageArrays.size() !=
					 context.Entry.Outputs.size() + context.Authored.DynamicOutputs.size()) ||
				capture->Failure.size() > Limits::MaximumTextBytes)
				return context.Fail(
					Status::InvalidValue, "host recording slot counts or failure text are invalid"
				);
			std::unordered_set<std::string_view> seenInputs;
			for (const auto &input : capture->Inputs) {
				const auto *resolved = context.Find(input.Port);
				if (!seenInputs.insert(input.Port).second || !resolved || *resolved != input.Data)
					return context.Fail(
						Status::InvalidValue, "host recording resolved control is stale", input.Port
					);
			}
			for (const auto &[port, value] : context.Values)
				if (!seenInputs.contains(port))
					return context.Fail(
						Status::InvalidValue, "host recording omits a resolved control", port
					);
			for (const auto &[port, value] : context.ValueViews)
				if (value && !seenInputs.contains(port))
					return context.Fail(
						Status::InvalidValue, "host recording omits a resolved control", port
					);
			if (!context.ImageArrays.empty() || capture->InputImages.size() != context.Images.size())
				return context.Fail(
					Status::UnsupportedExecution,
					"host image arrays require explicit normalized frame recordings"
				);
			std::unordered_set<std::string_view> seenImages;
			for (const auto &binding : capture->InputImages) {
				const auto image =
					std::find_if(context.Images.begin(), context.Images.end(), [&](const auto &entry) {
						return entry.first == binding.Port;
					});
				if (!seenImages.insert(binding.Port).second || image == context.Images.end() ||
					!image->second || SurfaceHash(*image->second) != binding.Hash)
					return context.Fail(
						Status::InvalidValue, "host recording image input is stale", binding.Port
					);
			}
			if (capture->State != HostCaptureState::Recorded)
				return context.Fail(
					Status::UnsupportedExecution,
					capture->Failure.empty() ? "host capability was refused or failed" : capture->Failure
				);
			const auto outputType = [&](std::string_view id) -> std::optional<ValueType> {
				if (context.Authored.Type == "pc.csv_file_read" && id == "content") {
					const auto *value = context.Find("convert_to_number");
					const auto *convert = value ? std::get_if<bool>(value) : nullptr;
					if (convert && *convert) return ValueType::Scalar;
				}
				if (context.Authored.Type == "pc.json_file_read" && id == "struct") return ValueType::Any;
				for (const auto &port : context.Entry.Outputs)
					if (port.Id == id) return port.Type;
				for (const auto &port : context.Authored.DynamicOutputs)
					if (port.Id == id) return port.Type;
				return std::nullopt;
			};
			std::unordered_set<std::string_view> outputs;
			for (const auto &output : capture->Outputs) {
				const auto type = outputType(output.Port);
				const auto *array = std::get_if<ArrayValue>(&output.Data);
				const bool matches =
					type &&
					(*type == ValueType::Any || *type == PayloadType(output.Data) ||
					 (*type == ValueType::Object && std::holds_alternative<StructValue>(output.Data)) ||
					 (array && array->ElementType == *type));
				if (!outputs.insert(output.Port).second || !matches || !ValidRuntimeValue(output.Data))
					return context.Fail(
						Status::TypeMismatch,
						"recorded host output does not match its typed port",
						output.Port
					);
				context.SetValue(output.Port, output.Data);
				if (context.FailureCode != Status::Ok) return false;
			}
			for (const auto &output : capture->Images) {
				const auto type = outputType(output.Port);
				if (!outputs.insert(output.Port).second || !type || *type != ValueType::Image)
					return context.Fail(
						Status::TypeMismatch, "recorded host image does not match its port", output.Port
					);
				if (!CopyImage(context, output.Port, output.Data)) return false;
			}
			for (const auto &output : capture->ImageArrays) {
				const auto type = outputType(output.Port);
				if (!outputs.insert(output.Port).second || !type ||
					(*type != ValueType::Image && *type != ValueType::Array) ||
					output.Frames.size() > Limits::MaximumRangeFrames)
					return context.Fail(
						Status::TypeMismatch, "recorded host image array does not match its port", output.Port
					);
				uint64_t bytes = output.Frames.size() * (sizeof(Image) + sizeof(ImageArrayItem));
				for (const auto &frame : output.Frames) {
					if (!ValidSurfaceLayout(
							frame, context.Request.MaximumImageDimension, Limits::MaximumOutputBytes
						) ||
						!FiniteSurfaceSamples(frame) ||
						frame.Pixels.size() > Limits::MaximumEvaluationBytes - bytes)
						return context.Fail(
							Status::InvalidValue,
							"recorded host frame array exceeds its surface budget",
							output.Port
						);
					bytes += frame.Pixels.size();
				}
				if (!context.ReserveOutput(bytes, output.Port)) return false;
				ImageArray frames;
				frames.Images.reserve(output.Frames.size());
				frames.Items.reserve(output.Frames.size());
				for (const auto &frame : output.Frames) {
					frames.Items.push_back({frames.Images.size()});
					frames.Images.push_back(frame);
					frames.Images.back().Hash = SurfaceHash(frames.Images.back());
				}
				context.OutputImageArrays.emplace_back(output.Port, std::move(frames));
			}

			if (capture == &live && context.HostReceipts && !context.HostReceipts->Append(*capture))
				return context.Fail(
					Status::LimitExceeded, "retained live host receipt exceeds the evaluation byte budget"
				);
			return true;
		}
		bool LayerSurface(NodeContext &context) {
			const auto *dataValue = context.Find("data");
			const auto *nameValue = context.Find("layer_name");
			const auto *data = dataValue ? std::get_if<StructValue>(dataValue) : nullptr;
			const auto *name = nameValue ? std::get_if<std::string>(nameValue) : nullptr;
			if (!data || !data->Data || !name)
				return context.Fail(
					Status::TypeMismatch, "layer selection needs parsed layered content and a name"
				);
			const auto field = [](const StructData &data, std::string_view key) -> const Value * {
				const auto found =
					std::find_if(data.Fields.begin(), data.Fields.end(), [&](const auto &field) {
						return field.first == key;
					});
				return found == data.Fields.end() ? nullptr : &found->second;
			};
			const Value *widthValue = field(*data->Data, "width"),
						*heightValue = field(*data->Data, "height"),
						*layersValue = field(*data->Data, "layerData");
			const auto *width = widthValue ? std::get_if<int64_t>(widthValue) : nullptr;
			const auto *height = heightValue ? std::get_if<int64_t>(heightValue) : nullptr;
			const auto *layers = layersValue ? std::get_if<ArrayValue>(layersValue) : nullptr;
			if (!width || !height || !layers || *width <= 0 || *height <= 0 ||
				*width > context.Request.MaximumImageDimension ||
				*height > context.Request.MaximumImageDimension)
				return context.Fail(Status::InvalidValue, "layered content canvas or layer list is invalid");
			const StructData *selected = nullptr;
			for (const auto &item : layers->Elements) {
				const auto *layer = std::get_if<StructValue>(&item);
				if (!layer || !layer->Data)
					return context.Fail(Status::InvalidValue, "layered content has an invalid layer");
				const auto *layerNameValue = field(*layer->Data, "name");
				const auto *layerName = layerNameValue ? std::get_if<std::string>(layerNameValue) : nullptr;
				if (!layerName)
					return context.Fail(Status::InvalidValue, "layered content has no layer name");
				if (*layerName == *name) selected = &*layer->Data;
			}
			if (!selected)
				return context.Fail(Status::InvalidValue, "selected layer name does not exist", "layer_name");
			const auto *imageValue = field(*selected, "image"), *xValue = field(*selected, "x"),
					   *yValue = field(*selected, "y");
			const auto *image = imageValue ? std::get_if<SurfaceValue>(imageValue) : nullptr;
			const auto *x = xValue ? std::get_if<int64_t>(xValue) : nullptr,
					   *y = yValue ? std::get_if<int64_t>(yValue) : nullptr;
			if (!image || !x || !y || *x < INT32_MIN || *x > INT32_MAX || *y < INT32_MIN || *y > INT32_MAX ||
				image->Data.Format != SurfaceFormat::RGBA8Unorm ||
				!ValidSurfaceLayout(
					image->Data, context.Request.MaximumImageDimension, Limits::MaximumOutputBytes
				))
				return context.Fail(
					Status::InvalidValue, "selected layer pixel storage or offsets are invalid"
				);
			auto *output = context.NewImage(
				"surface_out", static_cast<uint32_t>(*width), static_cast<uint32_t>(*height)
			);
			if (!output) return false;
			for (uint32_t row = 0; row < image->Data.Height; row++)
				for (uint32_t column = 0; column < image->Data.Width; column++) {
					const int64_t targetX = *x + column, targetY = *y + row;
					if (targetX < 0 || targetY < 0 || targetX >= *width || targetY >= *height) continue;
					const size_t source = (size_t{row} * image->Data.Width + column) * 4,
								 target = (static_cast<size_t>(targetY) * output->Width +
										   static_cast<size_t>(targetX)) *
										  4;
					std::copy_n(
						image->Data.Pixels.begin() + static_cast<ptrdiff_t>(source),
						4,
						output->Pixels.begin() + static_cast<ptrdiff_t>(target)
					);
				}
			output->Hash = SurfaceHash(*output);
			context.SetValue("layer_name", *name);
			return context.FailureCode == Status::Ok;
		}

		bool ExportPreview(NodeContext &context) {
			for (const auto &[port, array] : context.ImageArrays)
				if (port == "surface" && array) {
					uint64_t bytes = array->Images.size() * sizeof(Image);
					const auto chargeShape =
						[&](auto &&self, const std::vector<ImageArrayItem> &items, size_t depth) -> bool {
						if (depth > Limits::MaximumArrayDepth ||
							items.size() > Limits::MaximumArrayElements ||
							items.size() * sizeof(ImageArrayItem) > Limits::MaximumEvaluationBytes - bytes)
							return false;
						bytes += items.size() * sizeof(ImageArrayItem);
						for (const auto &item : items)
							if (const auto *children = std::get_if<std::vector<ImageArrayItem>>(&item.Data)) {
								if (!self(self, *children, depth + 1)) return false;
							} else if (std::get<size_t>(item.Data) >= array->Images.size())
								return false;
						return true;
					};
					if (!chargeShape(chargeShape, array->Items, 1))
						return context.Fail(
							Status::LimitExceeded, "export preview array shape exceeds its budget", "surface"
						);
					for (const auto &image : array->Images) {
						if (!ValidSurfaceLayout(
								image, context.Request.MaximumImageDimension, Limits::MaximumOutputBytes
							) ||
							!FiniteSurfaceSamples(image) ||
							image.Pixels.size() > Limits::MaximumEvaluationBytes - bytes)
							return context.Fail(
								Status::InvalidValue, "export preview array surface is invalid", "surface"
							);
						bytes += image.Pixels.size();
					}
					if (!context.ReserveOutput(bytes, "preview")) return false;
					context.OutputImageArrays.emplace_back("preview", *array);
					return true;
				}
			const auto source =
				std::find_if(context.Images.begin(), context.Images.end(), [](const auto &entry) {
					return entry.first == "surface";
				});
			if (source == context.Images.end() || !source->second)
				return context.Fail(
					Status::InvalidValue, "Export preview requires the explicit upstream surface", "surface"
				);
			return CopyImage(context, "preview", *source->second);
		}
	}
	std::span<const ExecutorEntry> HostExecutors() {
		static constexpr ExecutorEntry entries[]{
			{"pc.image", RecordedHost, true},
			{"pc.image_sequence", RecordedHost, true},
			{"pc.datetime_get", RecordedHost, true},	  {"pc.export", ExportPreview, true},
			{"pc.wav_file_write", RecordedHost, true},	  {"pc.byte_file_read", RecordedHost, true},
			{"pc.byte_file_write", RecordedHost, true},	  {"pc.text_file_read", RecordedHost, true},
			{"pc.text_file_write", RecordedHost, true},	  {"pc.csv_file_read", RecordedHost, true},
			{"pc.csv_file_write", RecordedHost, true},	  {"pc.json_file_read", RecordedHost, true},
			{"pc.json_file_write", RecordedHost, true},	  {"pc.xml_file_read", RecordedHost, true},
			{"pc.xml_file_write", RecordedHost, true},	  {"pc.http_request", RecordedHost, true},
			{"pc.http_request_file", RecordedHost, true}, {"pc.shell", RecordedHost, true},
			{"pc.midi_in", RecordedHost, true},			  {"pc.spout_send", RecordedHost, true},
			{"pc.spout_receive", RecordedHost, true},	  {"pc.ase_file_read", RecordedHost, true},
			{"pc.ase_layer", RecordedHost, true},		  {"pc.ase_tag", RecordedHost, true},
			{"pc.ase_tileset", RecordedHost, true},		  {"pc.ora_file_read", RecordedHost, true},
			{"pc.ora_layer", LayerSurface, true},		  {"pc.krita_file_read", RecordedHost, true},
			{"pc.krita_layer", LayerSurface, true},		  {"pc.gmroom", RecordedHost, true},
			{"pc.lua_compute", RecordedHost, true},		  {"pc.lua_global", RecordedHost, true},
			{"pc.lua_surface", RecordedHost, true},		  {"pc.image_mp4", RecordedHost, true},
			{"pc.image_gif", RecordedHost, true},		  {"pc.3_d_mesh_obj", RecordedHost, true},
			{"pc.3_d_mesh_json", RecordedHost, true},	  {"pc.3_d_mesh_export", RecordedHost, true}
		};
		return entries;
	}
}
