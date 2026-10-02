#include "ValuePayload.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <limits>
#include <new>

namespace engine::imagegraph {
	std::optional<uint64_t> KeyframePayloadBytes(const Keyframe &key) {
		if (!detail::ValidRuntimeValue(key.Data) ||
			(key.SourceDriver && !ValidKeyframeSourceDriver(*key.SourceDriver)))
			return std::nullopt;
		uint64_t bytes = sizeof(Keyframe);
		const auto add = [&](uint64_t amount) {
			if (amount > std::numeric_limits<uint64_t>::max() - bytes) return false;
			bytes += amount;
			return true;
		};
		if (!add(detail::ValuePayloadBytes(key.Data) - sizeof(Value)) || !add(key.NodeId.size()) ||
			!add(key.Port.size()) || !add(key.Interpolation.size()))
			return std::nullopt;
		if (key.Ease && (!add(key.Ease->InType.size()) || !add(key.Ease->OutType.size())))
			return std::nullopt;
		if (key.SourceDriver) {
			const auto *curve = std::get_if<KeyframeCurveDriver>(&*key.SourceDriver);
			if (curve && !add(detail::PayloadOwnedBytes(curve->Data))) return std::nullopt;
		}
		return bytes;
	}

	Status PrepareKeyframeCloneForProperty(
		const Document &document,
		const Keyframe &key,
		std::string_view nodeId,
		std::string_view property,
		Keyframe &result,
		Diagnostic &diagnostic
	) {
		const auto fail = [&](Status status, std::string message) {
			diagnostic = {status, std::string(nodeId), std::string(property), std::move(message)};
			return status;
		};
		const auto node = [&](std::string_view id) -> const Node * {
			for (const auto &candidate : document.Nodes)
				if (candidate.Id == id) return &candidate;
			return nullptr;
		};
		const Node *source = node(key.NodeId), *target = node(nodeId);
		if (!source || !target)
			return fail(Status::UnknownNode, "key transfer needs existing source and target nodes");
		const auto type = [](const Node &owner, std::string_view port) -> std::optional<ValueType> {
			if (const NodeSchema *schema = FindSchema(owner.Type))
				for (const auto &candidate : schema->Properties)
					if (candidate.Id == port) return candidate.Type;
			for (const auto &input : owner.DynamicInputs)
				if (input.Id == port && IsAuthoredValueType(input.Type)) return input.Type;
			return std::nullopt;
		};
		const auto sourceType = type(*source, key.Port), targetType = type(*target, property);
		if (!sourceType || !targetType)
			return fail(Status::UnknownPort, "key transfer needs existing authored properties");
		const auto input = [](const Node &owner, std::string_view port) -> const CatalogueInput * {
			const auto *entry = FindCatalogueEntry(owner.Type);
			if (!entry) return nullptr;
			if (const auto *fixed = FindCatalogueInput(*entry, port)) return fixed;
			size_t group = 0;
			return FindDynamicTemplate(*entry, port, group);
		};
		const auto *sourceInput = input(*source, key.Port), *targetInput = input(*target, property);
		// Only base source declarations establish these disjoint bits; packed native types do not.
		const auto sourceBit = [](const CatalogueInput *socket) -> unsigned {
			if (!socket) return 0;
			const auto kind = socket->SourceKind;
			if (kind == "Float" || kind == "Int" || kind == "Bool") return 1;
			if (kind == "Color") return 4;
			if (kind == "Text") return 10;
			return 0;
		};
		const unsigned sourceDomain = sourceBit(sourceInput), targetDomain = sourceBit(targetInput);
		if (sourceDomain && targetDomain && sourceDomain != targetDomain)
			return fail(Status::TypeMismatch, "source property declarations have disjoint value bits");
		if (*sourceType != *targetType)
			return fail(
				Status::UnsupportedExecution, "cross-domain key transfer has no verified native getter policy"
			);
		if (sourceInput || targetInput) {
			if (!sourceInput || !targetInput || !sourceInput->SourceArrayClassification ||
				!targetInput->SourceArrayClassification)
				return fail(
					Status::UnsupportedExecution, "source property display classification is unresolved"
				);
			if (*sourceInput->SourceArrayClassification != *targetInput->SourceArrayClassification)
				return fail(Status::TypeMismatch, "source property display classifications differ");
		}
		// Source typeArray classifies the widget, not processor nesting. Do not substitute ArrayDepth.
		if (*targetType == ValueType::Array) {
			const ArrayValue *targetArray = nullptr;
			for (const auto &value : target->Values)
				if (value.Port == property) targetArray = std::get_if<ArrayValue>(&value.Data);
			for (const auto &value : target->DynamicInputs)
				if (value.Id == property && value.Default)
					targetArray = std::get_if<ArrayValue>(&*value.Default);
			const auto *sourceArray = std::get_if<ArrayValue>(&key.Data);
			if (!targetArray || !sourceArray || sourceArray->ElementType != targetArray->ElementType)
				return fail(
					Status::UnsupportedExecution,
					"key transfer array leaf domain is unresolved or incompatible"
				);
		}
		const auto accepts = [&](const Node &owner, const CatalogueInput *socket, ValueType declared) {
			if (const auto *array = std::get_if<ArrayValue>(&key.Data);
				array && declared != ValueType::Array) {
				const auto *entry = FindCatalogueEntry(owner.Type);
				return document.FormatVersion >= 8 && key.Interpolation == "source" && entry && socket &&
					   CatalogueAuthoredArray(*entry, *socket, *array);
			}
			return detail::PayloadType(key.Data) == declared ||
				   (socket && CatalogueSourceEnumValue(*socket, key.Data)) ||
				   (document.FormatVersion >= 8 && key.Interpolation == "source" && socket &&
					CatalogueSourceRawValue(*socket, key.Data));
		};
		if (!accepts(*source, sourceInput, *sourceType) || !accepts(*target, targetInput, *targetType))
			return fail(Status::TypeMismatch, "key payload does not fit both native property domains");
		const auto bytes = KeyframePayloadBytes(key);
		if (!bytes || !ValidFrameTime(GetFrameTime(key)))
			return fail(Status::InvalidValue, "key transfer requires a finite bounded key");
		// Replacement names can allocate while the copied original names are still resident.
		if (*bytes > Limits::MaximumDocumentBytes || nodeId.size() > Limits::MaximumDocumentBytes - *bytes ||
			property.size() > Limits::MaximumDocumentBytes - *bytes - nodeId.size())
			return fail(Status::LimitExceeded, "key transfer exceeds the copy byte budget");
		try {
			Keyframe candidate = key;
			candidate.NodeId = nodeId;
			candidate.Port = property;
			result = std::move(candidate);
		} catch (const std::bad_alloc &) {
			return fail(Status::LimitExceeded, "key transfer allocation was refused");
		}
		diagnostic = {};
		return Status::Ok;
	}
}
