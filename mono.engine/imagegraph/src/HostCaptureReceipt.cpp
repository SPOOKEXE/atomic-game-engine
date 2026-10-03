#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/HostCapture.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>
namespace engine::imagegraph {
	namespace {
		std::optional<uint64_t> ReceiptBytes(const HostNodeCapture &receipt) {
			const auto node = NodeClonePayloadBytes(receipt.Authored);
			if (!node) return {};
			uint64_t bytes = sizeof(HostNodeCapture) + *node;
			const auto add = [&](uint64_t extra) {
				if (extra > UINT64_MAX - bytes) return false;
				bytes += extra;
				return true;
			};
			// NodeClonePayloadBytes admits the clone footprint; existing receipt capacity can be larger.
			const auto &n = receipt.Authored;
			const auto extra = [&](const auto &v) {
				return add(
					(v.capacity() - v.size()) * sizeof(typename std::decay_t<decltype(v)>::value_type)
				);
			};
			if (!extra(n.Values) || !extra(n.SourceProperties) || !extra(n.DynamicInputs) ||
				!extra(n.DynamicOutputs) || !extra(n.SourceInputExpressions) || !extra(n.InstanceOverrides) ||
				!extra(n.SourceAnimatedInputs) || !extra(n.SourceStaticInputs))
				return {};
			const auto textExtra = [&](const std::string &text) {
				return add(text.capacity() - std::max(text.size(), std::string{}.capacity()));
			};
			for (const auto *text :
				 {&n.Id, &n.Type, &n.GroupId, &n.InstanceBase, &n.SourceDisplayName, &n.SourceInternalName})
				if (!textExtra(*text)) return {};
			for (const auto *values : {&n.Values, &n.SourceProperties})
				for (const auto &value : *values)
					if (!textExtra(value.Port)) return {};
			for (const auto &input : n.DynamicInputs)
				if (!textExtra(input.Id) || !textExtra(input.SourceLayerName)) return {};
			for (const auto &output : n.DynamicOutputs)
				if (!textExtra(output.Id)) return {};
			for (const auto &expression : n.SourceInputExpressions)
				if (!textExtra(expression.Port) || !textExtra(expression.Code)) return {};
			for (const auto *ports : {&n.InstanceOverrides, &n.SourceAnimatedInputs, &n.SourceStaticInputs})
				for (const auto &port : *ports)
					if (!textExtra(port)) return {};
			for (const auto *values : {&receipt.Inputs, &receipt.Outputs}) {
				if (!add(values->capacity() * sizeof(AuthoredValue))) return {};
				for (const auto &value : *values) {
					const auto payload = ValueClonePayloadBytes(value.Data);
					if (!payload || !add(*payload + value.Port.capacity() + 1)) return {};
				}
			}
			if (!add(receipt.InputImages.capacity() * sizeof(HostImageBinding)) ||
				!add(receipt.Images.capacity() * sizeof(HostCapturedImage)) ||
				!add(receipt.ImageArrays.capacity() * sizeof(HostCapturedImageArray)) ||
				!add(receipt.Failure.capacity() + 1))
				return {};
			for (const auto &image : receipt.InputImages)
				if (!add(image.Port.capacity() + 1)) return {};
			for (const auto &image : receipt.Images)
				if (!add(image.Port.capacity() + image.Data.Pixels.capacity() + 1)) return {};
			for (const auto &array : receipt.ImageArrays) {
				if (!add(array.Port.capacity() + array.Frames.capacity() * sizeof(Image) + 1)) return {};
				for (const auto &image : array.Frames)
					if (!add(image.Pixels.capacity())) return {};
			}
			return bytes;
		}
	}
	std::optional<uint64_t> HostCaptureRetainedPayloadBytes(const HostNodeCapture &receipt) {
		return ReceiptBytes(receipt);
	}
	Status PrepareResolvedHostCapture(
		const HostNodeInvocation &invocation,
		uint64_t maximumBytes,
		HostNodeCapture &output,
		uint64_t &retainedBytes,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph resolved host receipt");
		const auto fail = [&](Status status, const char *message) {
			diagnostic = {status, invocation.Authored.Id, {}, message};
			return status;
		};
		maximumBytes =
			std::min({maximumBytes, invocation.MaximumOperationBytes, Limits::MaximumEvaluationBytes});
		if (invocation.Inputs.size() > Limits::MaximumLinks ||
			invocation.Images.size() > Limits::MaximumLinks ||
			invocation.Request.Tick > Limits::MaximumTick || !std::isfinite(invocation.Request.Subframe) ||
			invocation.Request.Subframe < 0 || invocation.Request.Subframe >= 1)
			return fail(Status::InvalidValue, "host receipt controls or clock are invalid");
		const auto node = NodeClonePayloadBytes(invocation.Authored), previous = ReceiptBytes(output);
		if (!node || !previous) return fail(Status::LimitExceeded, "host receipt owned payload is invalid");
		uint64_t bytes = sizeof(HostNodeCapture) + *node;
		const auto add = [&](uint64_t extra) {
			if (extra > maximumBytes || bytes > maximumBytes - extra) return false;
			bytes += extra;
			return true;
		};
		if (!add(invocation.Inputs.size() * sizeof(AuthoredValue)) ||
			!add(invocation.Images.size() * sizeof(HostImageBinding)))
			return fail(Status::LimitExceeded, "host receipt storage exceeds operation budget");
		for (size_t i = 0; i < invocation.Inputs.size(); ++i) {
			const auto &input = invocation.Inputs[i];
			const auto payload = ValueClonePayloadBytes(input.Data);
			if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes || !payload ||
				!add(*payload + std::max(input.Port.size(), std::string{}.capacity()) + 1))
				return fail(Status::LimitExceeded, "host receipt resolved control exceeds budget");
		}
		for (size_t i = 0; i < invocation.Images.size(); ++i) {
			const auto &image = invocation.Images[i];
			if (image.Port.empty() || image.Port.size() > Limits::MaximumTextBytes || !image.Data ||
				!ValidSurfaceLayout(*image.Data, invocation.Request.MaximumImageDimension, maximumBytes) ||
				!FiniteSurfaceSamples(*image.Data) ||
				!add(std::max(image.Port.size(), std::string{}.capacity()) + 1))
				return fail(Status::InvalidValue, "host receipt image binding is invalid");
		}
		// The existing receipt remains alive during replacement, and clone staging is admitted before copies.
		const uint64_t identityCount = invocation.Inputs.size() + invocation.Images.size();
		const uint64_t identityBytes =
			sizeof(std::vector<std::string_view>) + identityCount * sizeof(std::string_view);
		if (*previous > maximumBytes || identityBytes > maximumBytes - *previous ||
			bytes > (maximumBytes - *previous - identityBytes) / 2)
			return fail(
				Status::LimitExceeded, "host receipt replacement and clone/index scratch exceed budget"
			);
		std::vector<std::string_view> identities;
		identities.reserve(identityCount);
		if (identities.capacity() * sizeof(std::string_view) > identityBytes)
			return fail(Status::LimitExceeded, "host receipt identity capacities exceed admitted scratch");
		for (const auto &input : invocation.Inputs)
			identities.push_back(input.Port);
		for (const auto &image : invocation.Images)
			identities.push_back(image.Port);
		auto split = identities.begin() + invocation.Inputs.size();
		std::sort(identities.begin(), split);
		std::sort(split, identities.end());
		if (std::adjacent_find(identities.begin(), split) != split)
			return fail(Status::DuplicateId, "host receipt control repeats");
		if (std::adjacent_find(split, identities.end()) != identities.end())
			return fail(Status::DuplicateId, "host receipt image binding repeats");
		HostNodeCapture candidate;
		candidate.Authored = invocation.Authored;
		candidate.Tick = invocation.Request.Tick;
		candidate.Subframe = invocation.Request.Subframe;
		candidate.NegativeFrame = invocation.Request.NegativeFrame;
		candidate.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
		candidate.InputImages.reserve(invocation.Images.size());
		for (const auto &image : invocation.Images)
			candidate.InputImages.push_back({std::string(image.Port), SurfaceHash(*image.Data)});
		const auto actual = ReceiptBytes(candidate);
		if (!actual || *actual > maximumBytes - *previous)
			return fail(Status::LimitExceeded, "host receipt retained capacities exceed budget");
		core::Metrics::Count("imagegraph.host.receipt_retained_payload_bytes", *actual);
		core::Metrics::Count("imagegraph.host.receipts", 1);
		output = std::move(candidate);
		retainedBytes = *actual;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, invocation.Authored.Id, {}, "host receipt allocation failed"};
		return diagnostic.Code;
	} catch (const std::length_error &) {
		diagnostic = {
			Status::LimitExceeded, invocation.Authored.Id, {}, "host receipt allocation length exceeded"
		};
		return diagnostic.Code;
	}
}
