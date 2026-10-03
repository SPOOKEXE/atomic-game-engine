#pragma once

#include "NodeExecutors.hpp"
#include "SourcePathSequentialMath.hpp"
#include "SourcePathWeight.hpp"

namespace engine::imagegraph::detail {
	inline const DataReplayEntry *SourceSequentialPrevious(NodeContext &context, size_t row) {
		if (!context.CurrentData) return nullptr;
		for (const auto &entry : context.CurrentData->Entries)
			if (entry.NodeId == context.Authored.Id && entry.ProcessorRow == row) return &entry;
		return nullptr;
	}
	inline bool LoadSourceSamplerBuffers(NodeContext &context) {
		if (context.SourceSamplerBuffers) return true;
		std::array<SourcePathPointBuffer, 3> buffers{};
		if (const auto *previous = SourceSequentialPrevious(context, 0)) {
			if (!previous->Initialized || previous->Tick > context.Request.Tick ||
				previous->Values.size() != 1)
				return context.Fail(
					Status::InvalidValue, "Source path sampler replay receipt is invalid", "path"
				);
			const auto *array = std::get_if<ArrayValue>(&previous->Values[0].Data);
			if (!array || array->ElementType != ValueType::Vector4 || array->Elements.size() != 6 ||
				!array->Nested.empty() || !array->Items.empty())
				return context.Fail(
					Status::InvalidValue, "Source path sampler buffers have an invalid layout", "path"
				);
			for (size_t i = 0; i < buffers.size(); ++i) {
				const auto *point = std::get_if<Vector4>(&array->Elements[i * 2]);
				const auto *extra = std::get_if<Vector4>(&array->Elements[i * 2 + 1]);
				if (!point || !extra || (point->W != 0 && point->W != 1) ||
					(extra->Y != 0 && extra->Y != 1) || extra->Z != 0 || extra->W != 0)
					return context.Fail(
						Status::InvalidValue, "Source path sampler buffer fields are invalid", "path"
					);
				buffers[i].Class =
					point->W == 0 ? SourcePathPointClass::Planar : SourcePathPointClass::Spatial;
				buffers[i].Position = {point->X, point->Y};
				buffers[i].Weight = point->Z;
				if (extra->Y != 0)
					buffers[i].Z = extra->X;
				else if (extra->X != 0)
					return context.Fail(
						Status::InvalidValue, "Source path sampler absent Z is not canonical", "path"
					);
				if (!SourceSequentialFinitePoint(buffers[i]))
					return context.Fail(
						Status::InvalidValue, "Source path sampler buffer is nonfinite", "path"
					);
			}
		}
		context.SourceSamplerBuffers = buffers;
		return true;
	}
	inline bool SetSourceSamplerOutputs(NodeContext &context, bool inverse) {
		const auto &buffers = *context.SourceSamplerBuffers;
		const auto &point = buffers[0], &before = buffers[1], &after = buffers[2];
		if (!SourceSequentialFinitePoint(point) || !SourceSequentialFinitePoint(before) ||
			!SourceSequentialFinitePoint(after))
			return context.Fail(Status::InvalidValue, "Source sample geometry is undefined", "path");
		const double sign = inverse ? -1 : 1;
		if (point.Class == SourcePathPointClass::Spatial) {
			if (!before.Z || !after.Z)
				return context.Fail(
					Status::InvalidValue, "Source spatial sample direction reads an absent probe Z", "path"
				);
			context.SetValue("position", Vector3{point.Position.X, point.Position.Y, *point.Z});
			context.SetValue(
				"direction",
				Vector3{
					(before.Position.X - after.Position.X) * sign,
					(before.Position.Y - after.Position.Y) * sign,
					(*before.Z - *after.Z) * sign
				}
			);
		} else {
			context.SetValue("position", point.Position);
			context.SetValue(
				"direction",
				SourceWeightDirection(
					(after.Position.X - before.Position.X) * sign,
					(after.Position.Y - before.Position.Y) * sign
				)
			);
		}
		context.SetValue("weight", point.Weight);
		return context.FailureCode == Status::Ok;
	}
	inline bool PublishSourceSamplerBuffers(NodeContext &context) {
		if (context.ProcessorRow + 1 != context.ProcessorCount) return true;
		const uint64_t bytes = sizeof(DataReplayEntry) +
							   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
							   sizeof(DataReplayValueFrame) + 6 * sizeof(ElementValue);
		if (!context.ReserveOutput(bytes, "path")) return false;
		ArrayValue array;
		array.ElementType = ValueType::Vector4;
		array.Elements.reserve(6);
		for (const auto &point : *context.SourceSamplerBuffers) {
			array.Elements.emplace_back(
				Vector4{
					point.Position.X,
					point.Position.Y,
					point.Weight,
					point.Class == SourcePathPointClass::Planar ? 0. : 1.
				}
			);
			array.Elements.emplace_back(Vector4{point.Z.value_or(0), point.Z.has_value() ? 1. : 0., 0, 0});
		}
		DataReplayEntry entry;
		entry.NodeId = context.Authored.Id;
		entry.Tick = context.Request.Tick;
		entry.Subframe = context.Request.Subframe;
		entry.NegativeFrame = context.Request.NegativeFrame;
		entry.Initialized = true;
		entry.Values.push_back({context.Request.Tick, std::move(array)});
		if (RetainedDataReplayEntryBytes(entry) > bytes)
			return context.Fail(
				Status::LimitExceeded, "Source path sampler receipt capacity exceeds admission", "path"
			);
		context.DataUpdates.push_back(std::move(entry));
		return true;
	}
}
