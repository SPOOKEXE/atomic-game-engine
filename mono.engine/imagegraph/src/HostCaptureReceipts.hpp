#pragma once

#include "EvaluationAllocator.hpp"
#include "EvaluationBudget.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/HostCapture.hpp>

namespace engine::imagegraph::detail {
	// Keeps verified live observations available to owned builder recipes after provider calls return.
	struct HostCaptureReceiptSink {
		EvaluationBudget &Budget;
		AllocationReservation Charge;
		EvaluationVector<HostNodeCapture> Captures;
		explicit HostCaptureReceiptSink(EvaluationBudget &budget)
			: Budget(budget), Captures(EvaluationAllocator<HostNodeCapture>(budget)) {}
		bool Append(const HostNodeCapture &capture) {
			if (Captures.size() >= Limits::MaximumNodes) return false;
			const auto nodeBytes = NodeClonePayloadBytes(capture.Authored);
			if (!nodeBytes) return false;
			uint64_t bytes = *nodeBytes;
			const auto add = [&](uint64_t extra) {
				if (extra > Limits::MaximumEvaluationBytes - std::min(bytes, Limits::MaximumEvaluationBytes))
					return false;
				bytes += extra;
				return true;
			};
			if (!add(
					capture.Failure.size() + capture.Inputs.size() * sizeof(AuthoredValue) +
					capture.Outputs.size() * sizeof(AuthoredValue) +
					capture.InputImages.size() * sizeof(HostImageBinding) +
					capture.Images.size() * sizeof(HostCapturedImage) +
					capture.ImageArrays.size() * sizeof(HostCapturedImageArray)
				))
				return false;
			for (const auto *values : {&capture.Inputs, &capture.Outputs})
				for (const auto &value : *values) {
					const auto payload = ValueClonePayloadBytes(value.Data);
					if (!payload || !add(*payload + value.Port.size() + 32)) return false;
				}
			for (const auto &image : capture.Images)
				if (!add(image.Port.size() + image.Data.Pixels.size() + 32)) return false;
			for (const auto &binding : capture.InputImages)
				if (!add(binding.Port.size() + 32)) return false;
			for (const auto &images : capture.ImageArrays) {
				if (!add(images.Port.size() + images.Frames.size() * sizeof(Image) + 32)) return false;
				for (const auto &image : images.Frames)
					if (!add(image.Pixels.size())) return false;
			}
			auto charge = Budget.Reserve(bytes);
			if (!charge || !Charge.Merge(std::move(*charge))) return false;
			Captures.push_back(capture);
			return true;
		}
	};
}
