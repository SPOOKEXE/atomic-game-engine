#include "PixelBuilderPayload.hpp"
#include "SourceInputOrigin.hpp"
#include "SourcePathShiftMemo.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>

namespace engine::imagegraph {
	namespace {
		Status RecordingNodeStatus(const Node &node) {
			if (node.Values.size() > Limits::MaximumArrayElements ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
				node.DynamicInputs.size() > MaximumDynamicInputsForNode(node) ||
				node.DynamicOutputs.size() > Limits::MaximumDynamicOutputsPerNode ||
				node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
				node.SourceAnimatedInputs.size() > Limits::MaximumArrayElements ||
				node.SourceStaticInputs.size() > Limits::MaximumArrayElements ||
				node.SourceInputExpressions.size() > Limits::MaximumSourceInputExpressionsPerNode)
				return Status::LimitExceeded;
			for (const auto *values : {&node.Values, &node.SourceProperties})
				for (const auto &input : *values)
					if (!detail::ValidPixelBuilderRecordingValue(input.Data)) return Status::InvalidValue;
			std::array<uint64_t, Limits::MaximumPixelBuilderDynamicInputsPerNode> origins{};
			static_assert(Limits::MaximumDynamicInputsPerNode <= origins.size());
			std::string_view failedPort;
			const auto originStatus =
				detail::ValidateSourceInputOrigins(node.DynamicInputs, origins, failedPort);
			if (originStatus != Status::Ok) return originStatus;
			for (const auto &input : node.DynamicInputs)
				if (input.Default && !detail::ValidPixelBuilderRecordingValue(*input.Default))
					return Status::InvalidValue;
			return std::isfinite(node.Position.X) && std::isfinite(node.Position.Y) ? Status::Ok
																					: Status::InvalidValue;
		}

		std::optional<uint64_t> PriorRecordingBytes(const SourceBuiltinRandomCapture &capture) {
			if (capture.Inputs.size() > Limits::MaximumLinks ||
				capture.InputImages.size() > Limits::MaximumLinks || capture.Draws.size() > 65536 ||
				RecordingNodeStatus(capture.Authored) != Status::Ok)
				return std::nullopt;
			const auto authored = NodeClonePayloadBytes(capture.Authored);
			if (!authored) return std::nullopt;
			uint64_t bytes = sizeof(capture);
			const auto add = [&](uint64_t extra) {
				if (extra > Limits::MaximumEvaluationBytes - bytes) return false;
				bytes += extra;
				return true;
			};
			if (!add(*authored) || !add(capture.Inputs.capacity() * sizeof(AuthoredValue)) ||
				!add(capture.Draws.capacity() * sizeof(SourceBuiltinRandomDraw)) ||
				!add(capture.InputImages.capacity() * sizeof(SourceBuiltinRandomInputImage)))
				return std::nullopt;
			const auto &node = capture.Authored;
			const auto slack = [&](const auto &items) {
				return add(
					(items.capacity() - items.size()) *
					sizeof(typename std::decay_t<decltype(items)>::value_type)
				);
			};
			const auto textSlack = [&](const std::string &text) {
				const auto cloned = std::max(text.size(), std::string{}.capacity());
				return add(text.capacity() > cloned ? text.capacity() - cloned : 0);
			};
			if (!slack(node.Values) || !slack(node.SourceProperties) || !slack(node.DynamicInputs) ||
				!slack(node.DynamicOutputs) || !slack(node.InstanceOverrides) ||
				!slack(node.SourceAnimatedInputs) || !slack(node.SourceStaticInputs) ||
				!slack(node.SourceInputExpressions))
				return std::nullopt;
			for (const auto *text :
				 {&node.Id,
				  &node.Type,
				  &node.GroupId,
				  &node.InstanceBase,
				  &node.SourceDisplayName,
				  &node.SourceInternalName})
				if (!textSlack(*text)) return std::nullopt;
			for (const auto *values : {&node.Values, &node.SourceProperties})
				for (const auto &value : *values)
					if (!textSlack(value.Port)) return std::nullopt;
			for (const auto *ports :
				 {&node.InstanceOverrides, &node.SourceAnimatedInputs, &node.SourceStaticInputs})
				for (const auto &port : *ports)
					if (!textSlack(port)) return std::nullopt;
			for (const auto &input : node.DynamicInputs)
				if (!textSlack(input.Id) || !textSlack(input.SourceLayerName) ||
					!textSlack(input.SourceInputId))
					return std::nullopt;
			for (const auto &output : node.DynamicOutputs)
				if (!textSlack(output.Id)) return std::nullopt;
			for (const auto &expression : node.SourceInputExpressions)
				if (!textSlack(expression.Port) || !textSlack(expression.Code)) return std::nullopt;
			for (const auto &input : capture.Inputs) {
				if (!detail::ValidPixelBuilderRecordingValue(input.Data)) return std::nullopt;
				const auto payload = ValueClonePayloadBytes(input.Data);
				if (!payload || !add(*payload) || !add(input.Port.capacity())) return std::nullopt;
			}
			for (const auto &input : capture.InputImages) {
				if (!add(input.Port.capacity()) || !add(input.Data.Pixels.capacity())) return std::nullopt;
			}
			return bytes;
		}
	} // namespace
	Status ValidateBuiltinRandomCaptures(
		std::span<const SourceBuiltinRandomCapture> captures,
		uint64_t maximumBytes,
		uint64_t &ownedBytes,
		Diagnostic &diagnostic
	) {
		ownedBytes = 0;
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const auto fail = [&](Status code, const SourceBuiltinRandomCapture &capture, std::string message) {
			diagnostic = {
				code, capture.Authored.Id.substr(0, Limits::MaximumTextBytes), {}, std::move(message)
			};
			return code;
		};
		if (captures.size() > Limits::MaximumNodes) {
			diagnostic = {Status::LimitExceeded, {}, {}, "Builtin RNG capture count exceeds bounded nodes"};
			return diagnostic.Code;
		}
		const auto add = [&](uint64_t bytes) {
			if (bytes > maximumBytes - ownedBytes) return false;
			ownedBytes += bytes;
			return true;
		};
		for (size_t index = 0; index < captures.size(); ++index) {
			const auto &capture = captures[index];
			const uint64_t recordStart = ownedBytes;
			if (capture.Authored.Id.empty() || capture.Authored.Id.size() > Limits::MaximumTextBytes ||
				capture.Authored.Type.empty() || capture.Authored.Type.size() > Limits::MaximumTextBytes ||
				!ValidFrameTime({capture.Tick, capture.Subframe, capture.NegativeFrame}))
				return fail(
					Status::InvalidValue, capture, "Builtin RNG authored identity or time is invalid"
				);
			if (capture.ProcessorRow >= Limits::MaximumArrayElements ||
				capture.Inputs.size() > Limits::MaximumLinks ||
				capture.InputImages.size() > Limits::MaximumLinks || capture.Draws.size() > 65536)
				return fail(Status::LimitExceeded, capture, "Builtin RNG recording exceeds bounded slots");
			for (size_t previous = 0; previous < index; ++previous) {
				const auto &other = captures[previous];
				if (capture.Authored.Id == other.Authored.Id && capture.ProcessorRow == other.ProcessorRow &&
					capture.Tick == other.Tick && capture.Subframe == other.Subframe &&
					capture.NegativeFrame == other.NegativeFrame)
					return fail(
						Status::DuplicateId, capture, "Builtin RNG node, row and time are duplicated"
					);
			}
			if (const Status nodeStatus = RecordingNodeStatus(capture.Authored); nodeStatus != Status::Ok)
				return fail(
					nodeStatus,
					capture,
					"Builtin RNG authored recording closure is invalid or "
					"exceeds bounded slots"
				);
			const auto nodeBytes = NodeClonePayloadBytes(capture.Authored);
			if (!nodeBytes || !add(sizeof(capture) + *nodeBytes) ||
				!add(
					capture.Inputs.size() * (sizeof(AuthoredValue) + sizeof(std::string_view)) +
					capture.Draws.size() * sizeof(SourceBuiltinRandomDraw) +
					capture.InputImages.size() *
						(sizeof(SourceBuiltinRandomInputImage) + sizeof(std::string_view))
				))
				return fail(
					Status::LimitExceeded, capture, "Builtin RNG recordings exceed owned byte admission"
				);
			std::vector<std::string_view> ports;
			ports.reserve(capture.Inputs.size());
			for (size_t inputIndex = 0; inputIndex < capture.Inputs.size(); ++inputIndex) {
				const auto &input = capture.Inputs[inputIndex];
				if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes ||
					!detail::ValidPixelBuilderRecordingValue(input.Data))
					return fail(Status::InvalidValue, capture, "Builtin RNG resolved control is invalid");
				ports.push_back(input.Port);
				const auto bytes = ValueClonePayloadBytes(input.Data);
				if (!bytes || !add(*bytes) || !add(input.Port.size() + 32))
					return fail(
						Status::LimitExceeded, capture, "Builtin RNG controls exceed owned byte admission"
					);
			}
			std::sort(ports.begin(), ports.end());
			if (std::adjacent_find(ports.begin(), ports.end()) != ports.end())
				return fail(Status::DuplicateId, capture, "Builtin RNG resolved control is duplicated");
			ports.clear();
			ports.reserve(capture.InputImages.size());
			for (const auto &input : capture.InputImages) {
				if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes ||
					!ValidSurfaceLayout(input.Data, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
					return fail(
						Status::InvalidValue, capture, "Builtin RNG resolved image layout is invalid"
					);
				if (!add(input.Port.size() + 32) || !add(input.Data.Pixels.size()))
					return fail(
						Status::LimitExceeded, capture, "Builtin RNG images exceed owned byte admission"
					);
				if (!FiniteSurfaceSamples(input.Data))
					return fail(Status::InvalidValue, capture, "Builtin RNG resolved image is nonfinite");
				ports.push_back(input.Port);
			}
			std::sort(ports.begin(), ports.end());
			if (std::adjacent_find(ports.begin(), ports.end()) != ports.end())
				return fail(Status::DuplicateId, capture, "Builtin RNG resolved image is duplicated");
			for (const auto &draw : capture.Draws) {
				if (!std::isfinite(draw.Lower) || !std::isfinite(draw.Upper) || !std::isfinite(draw.Result))
					return fail(Status::InvalidValue, capture, "Builtin RNG draw is nonfinite");
				bool valid = false;
				switch (draw.Operation) {
				case SourceBuiltinRandomOperation::Random:
					valid = draw.Lower == 0 && draw.Upper >= 0 && draw.Result >= 0 &&
							(draw.Upper == 0 ? draw.Result == 0 : draw.Result < draw.Upper);
					break;
				case SourceBuiltinRandomOperation::IRandom:
					valid = draw.Lower == 0 && draw.Upper >= 0 && draw.Result >= 0 &&
							draw.Result <= std::floor(draw.Upper) && std::trunc(draw.Result) == draw.Result;
					break;
				case SourceBuiltinRandomOperation::IRandomRange:
					valid = draw.Result >= std::floor(std::min(draw.Lower, draw.Upper)) &&
							draw.Result <= std::floor(std::max(draw.Lower, draw.Upper)) &&
							std::trunc(draw.Result) == draw.Result;
					break;
				case SourceBuiltinRandomOperation::SeedObservation:
					valid = draw.Lower == 0 && draw.Upper == 0;
					break;
				case SourceBuiltinRandomOperation::RandomRange:
					valid = draw.Result >= std::min(draw.Lower, draw.Upper) &&
							draw.Result <= std::max(draw.Lower, draw.Upper);
					break;
				case SourceBuiltinRandomOperation::CRand:
					valid = draw.Lower == 0 && draw.Upper == 0 && draw.Result >= 0 &&
							draw.Result <= INT_MAX && std::trunc(draw.Result) == draw.Result;
					break;
				}
				if (!valid)
					return fail(
						Status::InvalidValue, capture, "Builtin RNG draw violates its operation bounds"
					);
			}
			const auto retained = PriorRecordingBytes(capture);
			if (!retained ||
				(*retained > ownedBytes - recordStart && !add(*retained - (ownedBytes - recordStart))))
				return fail(
					Status::LimitExceeded,
					capture,
					"Builtin RNG retained recording capacity exceeds byte admission"
				);
		}
		diagnostic = {};
		return Status::Ok;
	}
	Status PrepareSourceBuiltinRandomCapture(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		SourceBuiltinRandomCapture &capture,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (nodeId.empty() || nodeId.size() > Limits::MaximumTextBytes) {
			diagnostic = {
				Status::InvalidValue,
				std::string(nodeId.substr(0, Limits::MaximumTextBytes)),
				{},
				"Builtin RNG preparation node identity is invalid"
			};
			return diagnostic.Code;
		}
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end()) {
			diagnostic = {
				Status::UnknownNode, std::string(nodeId), {}, "Builtin RNG capture node is missing"
			};
			return diagnostic.Code;
		}

		if (const Status nodeStatus = RecordingNodeStatus(*node); nodeStatus != Status::Ok) {
			diagnostic = {
				nodeStatus,
				std::string(nodeId),
				{},
				"Builtin RNG preparation node is invalid or exceeds bounded slots"
			};
			return diagnostic.Code;
		}
		if (!ValidFrameTime(GetFrameTime(request))) {
			diagnostic = {
				Status::InvalidValue,
				std::string(nodeId),
				{},
				"Builtin RNG preparation node or frame is invalid"
			};
			return diagnostic.Code;
		}

		const auto priorBytes = PriorRecordingBytes(capture);
		if (!priorBytes || *priorBytes > maximumBytes) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Builtin RNG previous recording exceeds bounded replacement residency"
			};
			return diagnostic.Code;
		}
		const uint64_t availableBytes = maximumBytes - *priorBytes;
		const auto nodeBytes = NodeClonePayloadBytes(*node);
		if (!nodeBytes || availableBytes < sizeof(SourceBuiltinRandomCapture) ||
			*nodeBytes > availableBytes - sizeof(SourceBuiltinRandomCapture)) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Builtin RNG preparation authoring exceeds owned byte admission"
			};
			return diagnostic.Code;
		}
		EvaluationSnapshot snapshot;
		const auto status =
			EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic, availableBytes);
		if (status != Status::Ok) return status;
		if (!snapshot.ImageArrays().empty()) {
			diagnostic = {
				Status::UnsupportedExecution,
				std::string(nodeId),
				{},
				"Builtin RNG capture image arrays require a per-row "
				"source recording contract"
			};
			return diagnostic.Code;
		}

		if (snapshot.Values().size() > Limits::MaximumLinks ||
			snapshot.Images().size() > Limits::MaximumLinks) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Builtin RNG preparation controls exceed bounded slots"
			};
			return diagnostic.Code;
		}
		uint64_t preparationBytes = *nodeBytes + sizeof(SourceBuiltinRandomCapture);
		const auto admit = [&](uint64_t extra) {
			if (extra > availableBytes - preparationBytes) return false;
			preparationBytes += extra;
			return true;
		};
		if (!admit(snapshot.Values().size() * (sizeof(AuthoredValue) + sizeof(std::string_view)))) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Builtin RNG preparation controls exceed owned byte admission"
			};
			return diagnostic.Code;
		}
		for (const auto &input : snapshot.Values()) {
			if (!detail::ValidPixelBuilderRecordingValue(input.Data)) {
				diagnostic = {
					Status::InvalidValue,
					std::string(nodeId),
					input.Port,
					"Builtin RNG preparation control closure is invalid"
				};
				return diagnostic.Code;
			}
			const auto payload = ValueClonePayloadBytes(input.Data);
			if (!payload || !admit(*payload) || !admit(input.Port.size() + 32)) {
				diagnostic = {
					Status::LimitExceeded,
					std::string(nodeId),
					input.Port,
					"Builtin RNG preparation controls exceed owned byte admission"
				};
				return diagnostic.Code;
			}
		}
		if (!admit(
				snapshot.Images().size() * (sizeof(SourceBuiltinRandomInputImage) + sizeof(std::string_view))
			)) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Builtin RNG preparation image slots exceed owned byte admission"
			};
			return diagnostic.Code;
		}
		for (const auto &input : snapshot.Images()) {
			if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes ||
				!ValidSurfaceLayout(input.Data, Limits::MaximumDimension, Limits::MaximumEvaluationBytes)) {
				diagnostic = {
					Status::InvalidValue,
					std::string(nodeId),
					input.Port,
					"Builtin RNG preparation image layout is invalid"
				};
				return diagnostic.Code;
			}
			if (!admit(input.Port.size() + 32) || !admit(input.Data.Pixels.size())) {
				diagnostic = {
					Status::LimitExceeded,
					std::string(nodeId),
					input.Port,
					"Builtin RNG preparation images exceed owned byte admission"
				};
				return diagnostic.Code;
			}
			if (!FiniteSurfaceSamples(input.Data)) {
				diagnostic = {
					Status::InvalidValue,
					std::string(nodeId),
					input.Port,
					"Builtin RNG preparation image is nonfinite"
				};
				return diagnostic.Code;
			}
		}
		if (snapshot.RetainedBytes() > availableBytes - preparationBytes) {
			diagnostic = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Builtin RNG preparation snapshot and owned controls exceed their shared byte bound"
			};
			return diagnostic.Code;
		}
		SourceBuiltinRandomCapture prepared;
		prepared.Authored = *node;
		detail::StripSourcePathShiftIdentities(prepared.Authored);
		prepared.Tick = request.Tick;
		prepared.Subframe = request.Subframe;
		prepared.NegativeFrame = request.NegativeFrame;
		prepared.Inputs.reserve(snapshot.Values().size());
		for (const auto &input : snapshot.Values())
			prepared.Inputs.push_back({input.Port, input.Data});
		prepared.InputImages.reserve(snapshot.Images().size());
		for (const auto &input : snapshot.Images())
			prepared.InputImages.push_back({input.Port, input.Data});
		uint64_t bytes = 0;
		if (ValidateBuiltinRandomCaptures({&prepared, 1}, maximumBytes, bytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		capture = std::move(prepared);
		diagnostic = {};
		return Status::Ok;
	}
} // namespace engine::imagegraph
