#include "PcxControls.hpp"

#include "nodes/ArraySource.hpp"

#include <engine/imagegraph/HostCapture.hpp>
namespace engine::imagegraph::detail {
	namespace {
		bool ArraySnapshot(const ImageArray &source, ArrayValue &value) {
			value.ElementType = ValueType::Any;
			source_array::TreeCost cost;
			if (!source.Items.empty()) {
				if (!source_array::ImageCost(source, source.Items, cost, 1)) return false;
				value.Items = source_array::FromImages(source, source.Items);
				return true;
			}
			if (source.Images.size() > Limits::MaximumArrayElements) return false;
			cost.Bytes = source.Images.size() * sizeof(SourceArrayItem);
			for (const auto &image : source.Images) {
				if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
					image.Pixels.size() >
						Limits::MaximumArrayBytes - std::min(cost.Bytes, Limits::MaximumArrayBytes))
					return false;
				cost.Bytes += image.Pixels.size();
				value.Items.push_back({image});
			}
			return true;
		}
	}
	bool ApplyPcxInputExpressions(NodeContext &context, std::span<const PcxInputProgram> programs) {
		for (const auto &program : programs) {
			const auto &expression = *program.Expression;
			if (!expression.Enabled || expression.Code.empty()) continue;
			uint64_t scratchBytes =
				4 * Limits::MaximumArrayBytes + expression.Code.size() * 8 + 4096 * sizeof(PcxInstruction);
			for (const auto &[port, value] : context.Values)
				scratchBytes += RetainedPayloadBytes(value) + port.size();
			for (const auto &[port, value] : context.ValueViews)
				if (value) scratchBytes += RetainedPayloadBytes(*value) + port.size();
			for (const auto &[port, image] : context.Images)
				if (image) scratchBytes += image->Pixels.size() + port.size();
			for (const auto &[port, images] : context.ImageArrays)
				if (images) scratchBytes += Limits::MaximumArrayBytes + port.size();
			auto scratch = context.ReserveWorkspace(scratchBytes, expression.Port);
			if (!scratch) return false;
			PcxExpressionValue tree;
			Diagnostic diagnostic;
			if (CompilePcxProgram(expression.Code, tree, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, expression.Port);
			const Value *original = context.Find(expression.Port);
			Value current = original ? *original : Value{double{0}};
			if (const auto *surface = context.Input(expression.Port)) current = SurfaceValue{*surface};
			for (const auto &[port, images] : context.ImageArrays)
				if (images && port == expression.Port) {
					ArrayValue array;
					if (!ArraySnapshot(*images, array))
						return context.Fail(
							Status::LimitExceeded, "PCX input image array exceeds snapshot bounds", port
						);
					current = std::move(array);
				}
			StructValue values;
			values.Data.emplace();
			for (const auto &[port, value] : context.Values)
				values.Data->Fields.emplace_back(std::string(port), value);
			for (const auto &[port, value] : context.ValueViews)
				if (value) values.Data->Fields.emplace_back(std::string(port), *value);
			for (const auto &[port, image] : context.Images)
				if (image) values.Data->Fields.emplace_back(std::string(port), SurfaceValue{*image});
			for (const auto &[port, images] : context.ImageArrays)
				if (images) {
					ArrayValue array;
					if (!ArraySnapshot(*images, array))
						return context.Fail(
							Status::LimitExceeded, "PCX self image array exceeds snapshot bounds", port
						);
					values.Data->Fields.emplace_back(std::string(port), std::move(array));
				}
			const auto *input = FindCatalogueInput(context.Entry, expression.Port);
			const std::array parameters{
				AuthoredValue{"name", std::string(input ? input->Name : expression.Port)},
				AuthoredValue{"node_name", program.Owner->SourceDisplayName},
				AuthoredValue{"value", std::move(current)},
				AuthoredValue{"node_values", std::move(values)}
			};
			PcxExecutionContext execution{
				context.Request,
				parameters,
				context.Timeline,
				{double(context.Project.SurfaceWidth), double(context.Project.SurfaceHeight)},
				context.Request.ProjectName
			};
			execution.Names = context.PcxNames;
			PcxExecutionResult result;
			if (ExecutePcxExpression(tree, execution, result, diagnostic) != Status::Ok)
				return context.Fail(diagnostic.Code, diagnostic.Message, expression.Port);
			if (std::holds_alternative<UndefinedValue>(result.Data)) result.Data = double{0};
			if (!result.Messages.empty()) {
				uint64_t bytes = 2 * result.Messages.size() * sizeof(PcxMessage);
				for (const auto &message : result.Messages)
					bytes += message.Text.size();
				auto charge = context.ReserveWorkspace(bytes, expression.Port);
				if (!charge || !context.PcxControlCharge.Merge(std::move(*charge))) return false;
				for (auto &message : result.Messages)
					context.PcxControlMessages.push_back(std::move(message));
			}
			auto retained = context.ReserveWorkspace(RetainedPayloadBytes(result.Data), expression.Port);
			if (!retained || !context.PcxControlCharge.Merge(std::move(*retained))) return false;
			std::erase_if(context.ValueViews, [&](const auto &value) {
				return value.first == expression.Port;
			});
			std::erase_if(context.Images, [&](const auto &value) { return value.first == expression.Port; });
			std::erase_if(context.ImageArrays, [&](const auto &value) {
				return value.first == expression.Port;
			});
			auto found = std::find_if(context.Values.begin(), context.Values.end(), [&](const auto &value) {
				return value.first == expression.Port;
			});
			if (found == context.Values.end()) {
				context.Values.emplace_back(expression.Port, std::move(result.Data));
				found = context.Values.end() - 1;
			} else
				found->second = std::move(result.Data);
			if (const auto *surface = std::get_if<SurfaceValue>(&found->second))
				context.Images.emplace_back(expression.Port, &surface->Data);
		}
		return true;
	}
}
