#include "SourceRetainedImageOutputs.hpp"

#include "nodes/ArraySource.hpp"
#include "nodes/Processor.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

namespace engine::imagegraph::detail {
	namespace {
		const DataReplayEntry *PriorMetadata(NodeContext &c) {
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (!owner) return nullptr;
			auto scratch =
				c.ReserveWorkspace(owner->Entries.size() * sizeof(size_t), "retained image metadata");
			if (!scratch) return nullptr;
			Diagnostic diagnostic;
			const auto valid = ValidateDataReplay(*owner, c.ByteBudget, diagnostic);
			if (valid != Status::Ok) {
				c.Fail(diagnostic);
				return nullptr;
			}
			const DataReplayEntry *prior = nullptr;
			for (const auto &entry : owner->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == 0) prior = &entry;
			if (prior && (prior->Values.size() != 1 || prior->Values.front().Frame != prior->Tick ||
						  CompareFrameTime(
							  {prior->Tick, prior->Subframe, prior->NegativeFrame},
							  {c.Request.Tick, c.Request.Subframe, c.Request.NegativeFrame}
						  ) > 0 ||
						  !std::holds_alternative<StructValue>(prior->Values.front().Data))) {
				c.Fail(
					Status::InvalidValue,
					"retained image metadata requires a matching prior frame and typed record"
				);
				return nullptr;
			}
			return prior;
		}
		bool PublishImageTree(NodeContext &c, std::string_view port, const source_array::Items &items) {
			source_array::TreeCost cost;
			if (!source_array::Measure(items, cost) || !source_array::AllImages(items))
				return c.Fail(Status::InvalidValue, "retained image output contains non-image leaves", port);
			const uint64_t bytes =
				cost.Bytes +
				cost.Nodes * (sizeof(Image) + sizeof(ImageArrayItem) + sizeof(std::vector<ImageArrayItem>)) +
				std::max(port.size(), std::string{}.capacity());
			if (!c.ReserveOutput(bytes, port)) return false;
			auto copy = items;
			ImageArray result;
			result.Images.reserve(cost.Nodes);
			source_array::ExportImages(copy, result, result.Items);
			c.OutputImageArrays.emplace_back(port, std::move(result));
			return true;
		}
		bool PublishRetainedField(NodeContext &c, std::string_view port, const Value &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value)) {
				Image *image =
					c.NewImage(port, surface->Data.Width, surface->Data.Height, surface->Data.Format);
				if (!image) return false;
				image->Pixels = surface->Data.Pixels;
				image->Hash = surface->Data.Hash;
				return true;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value);
				array && array->ElementType == ValueType::Any && array->Elements.empty() &&
				array->Nested.empty())
				return PublishImageTree(c, port, array->Items);
			return c.Fail(Status::InvalidValue, "retained depth is not a surface or image array", port);
		}
		const ImageArray *InputArray(const NodeContext &c, std::string_view port) {
			for (const auto &[id, array] : c.ImageArrays)
				if (id == port) return array;
			return nullptr;
		}
	}
	const Value *SourceRetainedField(NodeContext &c, std::string_view port) {
		const auto *prior = PriorMetadata(c);
		if (!prior) return nullptr;
		const auto &record = std::get<StructValue>(prior->Values.front().Data);
		const size_t requiredFields = c.Entry.Type == "pc.smear" ? 1 : 2;
		if (!record.Data || record.Data->Fields.size() != requiredFields) {
			c.Fail(Status::InvalidValue, "retained image metadata record has an invalid field count");
			return nullptr;
		}
		for (const auto &[name, value] : record.Data->Fields)
			if (name == port) return &value;
		c.Fail(Status::InvalidValue, "retained image metadata field is missing", port);
		return nullptr;
	}
	bool StoreSourceRetainedMetadata(
		NodeContext &c, std::span<const std::pair<std::string_view, const Value *>> fields
	) {
		if (!c.CurrentData && !c.Request.DataReplay) return true;
		if (fields.empty() || fields.size() > 3)
			return c.Fail(Status::InvalidValue, "retained image metadata field count is invalid");
		uint64_t bytes = sizeof(DataReplayEntry) + std::max(c.Authored.Id.size(), std::string{}.capacity()) +
						 sizeof(DataReplayValueFrame) + sizeof(StructData) +
						 fields.size() * sizeof(std::pair<std::string, Value>);
		uint64_t logicalBytes = sizeof(StructData) + fields.size() * sizeof(std::pair<std::string, Value>);
		size_t logicalElements = 1;
		const auto countItems = [&](const auto &self, const source_array::Items &items) -> void {
			logicalElements += items.size();
			for (const auto &item : items) {
				if (const auto *children = std::get_if<source_array::Items>(&item.Data))
					self(self, *children);
				else if (std::holds_alternative<ElementValue>(item.Data))
					++logicalElements;
			}
		};
		for (const auto &[name, value] : fields) {
			const uint64_t fieldBytes =
				value ? std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, *value) : 0;
			if (!value || fieldBytes > Limits::MaximumArrayBytes - logicalBytes ||
				name.size() > Limits::MaximumArrayBytes - logicalBytes - fieldBytes)
				return c.Fail(
					Status::LimitExceeded, "retained image metadata record exceeds typed payload bounds", name
				);
			logicalBytes += fieldBytes + name.size();
			++logicalElements;
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				logicalElements += array->Elements.size();
				for (const auto &row : array->Nested)
					logicalElements += row.size();
				countItems(countItems, array->Items);
			}
			if (logicalElements > Limits::MaximumArrayElements)
				return c.Fail(
					Status::LimitExceeded, "retained image metadata record exceeds typed element bounds", name
				);
			const auto clone = ValueClonePayloadBytes(*value);
			const uint64_t backing = clone ? *clone - sizeof(Value) : 0;
			if (!value || !ValidValuePayload(*value, true) || !clone ||
				backing > Limits::MaximumEvaluationBytes - bytes)
				return c.Fail(
					Status::LimitExceeded, "retained image metadata clone exceeds its payload budget", name
				);
			const uint64_t nameBytes = std::max(name.size(), std::string{}.capacity());
			if (nameBytes > Limits::MaximumEvaluationBytes - bytes - backing)
				return c.Fail(
					Status::LimitExceeded, "retained image metadata text exceeds payload budget", name
				);
			bytes += backing + nameBytes;
		}
		if (!c.ReserveOutput(bytes, "retained image metadata")) return false;
		StructValue record;
		record.Data.emplace().Fields.reserve(fields.size());
		for (const auto &[name, value] : fields)
			record.Data->Fields.emplace_back(name, *value);
		DataReplayEntry update;
		update.NodeId = c.Authored.Id;
		update.ProcessorRow = 0;
		update.Tick = c.Request.Tick;
		update.Subframe = c.Request.Subframe;
		update.NegativeFrame = c.Request.NegativeFrame;
		update.Initialized = true;
		update.Values.reserve(1);
		update.Values.push_back({c.Request.Tick, std::move(record)});
		// Admission already includes the complete entry and cloned backing pixels.
		c.DataUpdates.push_back(std::move(update));
		return true;
	}
	bool SourceRetainedProcessorInactive(NodeContext &c, bool &handled) {
		handled = c.Entry.Type == "pc.smear" && !c.Boolean("active", true);
		if (!handled) return true;
		if (const auto *array = InputArray(c, "surface_in")) {
			source_array::TreeCost cost;
			if (!source_array::ImageCost(*array, array->Items, cost, 1))
				return c.Fail(
					Status::LimitExceeded, "inactive Smear source array clone exceeds bounds", "surface_out"
				);
			auto cloneCharge = c.ReserveWorkspace(cost.Bytes, "surface_out");
			if (!cloneCharge) return false;
			auto items = source_array::FromImages(*array, array->Items);
			if (!PublishImageTree(c, "surface_out", items)) return false;
		} else {
			bool failed = false;
			if (!CopyWhenInactive(c, failed) || failed) return false;
		}
		const Value *depth = SourceRetainedField(c, "depth_pass");
		if (c.FailureCode != Status::Ok) return false;
		if (!depth || std::holds_alternative<UndefinedValue>(*depth))
			return c.SetOutputDiagnostic(
				"depth_pass",
				Status::UnsupportedExecution,
				depth ? "previous active Smear depth was undefined"
					  : "fresh inactive Smear has no prior depth surface"
			);
		return PublishRetainedField(c, "depth_pass", *depth);
	}
	bool CaptureSourceRetainedProcessorOutputs(NodeContext &c) {
		if (c.Entry.Type != "pc.smear" || !c.Boolean("active", true) ||
			(!c.CurrentData && !c.Request.DataReplay))
			return true;
		auto temporaryCharge = c.ReserveWorkspace(0, "retained image metadata");
		if (!temporaryCharge) return false;
		Value value = UndefinedValue{};
		if (!c.OutputDiagnostic("depth_pass")) {
			const Image *image = nullptr;
			const ImageArray *images = nullptr;
			for (const auto &[port, output] : c.OutputImages)
				if (port == "depth_pass") image = &output;
			for (const auto &[port, output] : c.OutputImageArrays)
				if (port == "depth_pass") images = &output;
			if (image) {
				auto required = c.ReserveWorkspace(image->Pixels.size(), "depth_pass");
				if (!required) return false;
				*temporaryCharge = std::move(*required);
				value = SurfaceValue{*image};
			} else if (images) {
				source_array::TreeCost cost;
				if (!source_array::ImageCost(*images, images->Items, cost, 1))
					return c.Fail(
						Status::LimitExceeded, "Smear retained depth array exceeds bounds", "depth_pass"
					);
				auto required = c.ReserveWorkspace(cost.Bytes, "depth_pass");
				if (!required) return false;
				*temporaryCharge = std::move(*required);
				ArrayValue array;
				array.ElementType = ValueType::Any;
				array.Items = source_array::FromImages(*images, images->Items);
				value = std::move(array);
			} else
				return c.Fail(Status::InvalidOutput, "Smear depth publication is absent", "depth_pass");
		}
		const std::array fields{std::pair<std::string_view, const Value *>{"depth_pass", &value}};
		return StoreSourceRetainedMetadata(c, fields);
	}
}
