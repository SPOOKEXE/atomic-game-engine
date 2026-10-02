#pragma once
#include "NodeExecutors.hpp"

#include <engine/imagegraph/SourceBuiltinRandom.hpp>

#include <unordered_set>
namespace engine::imagegraph::detail {
	inline bool
	FindSourceBuiltinRandomCapture(NodeContext &context, const SourceBuiltinRandomCapture *&capture) {
		capture = nullptr;
		if (context.Request.BuiltinRandomCaptures.size() > Limits::MaximumNodes)
			return context.Fail(Status::LimitExceeded, "Builtin RNG capture count exceeds bounded nodes");
		for (const auto &record : context.Request.BuiltinRandomCaptures) {
			if (record.Authored.Id != context.Authored.Id || record.ProcessorRow != context.ProcessorRow ||
				record.Tick != context.Request.Tick || record.Subframe != context.Request.Subframe ||
				record.NegativeFrame != context.Request.NegativeFrame)
				continue;
			if (capture)
				return context.Fail(
					Status::DuplicateId, "Builtin RNG node, row and time are duplicated", "seed"
				);
			capture = &record;
		}
		if (!capture)
			return context.Fail(
				Status::UnsupportedExecution,
				"Source builtin RNG draw observations are unavailable; source seed stream remains unverified",
				"seed"
			);
		if (capture->Authored != context.Authored || capture->Tick != context.Request.Tick ||
			capture->Subframe != context.Request.Subframe ||
			capture->NegativeFrame != context.Request.NegativeFrame)
			return context.Fail(Status::InvalidValue, "Builtin RNG recording is stale", "seed");
		if (capture->Inputs.size() > Limits::MaximumLinks ||
			capture->InputImages.size() > Limits::MaximumLinks || capture->Draws.size() > 65536)
			return context.Fail(Status::LimitExceeded, "Builtin RNG recording exceeds its bounded slots");
		auto controlsCharge = context.ReserveWorkspace(
			(capture->Inputs.size() + capture->InputImages.size()) * (sizeof(std::string_view) + 32)
		);
		if (!controlsCharge) return false;
		std::unordered_set<std::string_view> ports;
		for (const auto &input : capture->Inputs) {
			const Value *resolved = context.Find(input.Port);
			if (!ports.insert(input.Port).second || !resolved || *resolved != input.Data)
				return context.Fail(
					Status::InvalidValue, "Builtin RNG resolved control is stale", input.Port
				);
		}
		for (const auto &[port, value] : context.Values)
			if (!ports.contains(port))
				return context.Fail(
					Status::InvalidValue, "Builtin RNG recording omits a resolved control", port
				);
		for (const auto &[port, value] : context.ValueViews)
			if (value && !ports.contains(port))
				return context.Fail(
					Status::InvalidValue, "Builtin RNG recording omits a resolved control", port
				);
		ports.clear();
		for (const auto &input : capture->InputImages) {
			const Image *resolved = context.Input(input.Port);
			if (!ports.insert(input.Port).second || !resolved || resolved->Width != input.Data.Width ||
				resolved->Height != input.Data.Height || resolved->Format != input.Data.Format ||
				resolved->Pixels != input.Data.Pixels)
				return context.Fail(Status::InvalidValue, "Builtin RNG resolved image is stale", input.Port);
		}
		for (const auto &[port, image] : context.Images)
			if (image && !ports.contains(port))
				return context.Fail(
					Status::InvalidValue, "Builtin RNG recording omits a resolved image", port
				);
		// ProcessorBatch retains its original array while binding one owned row image by port.
		for (const auto &[port, images] : context.ImageArrays)
			if (images && !context.Input(port))
				return context.Fail(
					Status::UnsupportedExecution,
					"Builtin RNG image arrays require a per-row source recording",
					port
				);
		return true;
	}
}
