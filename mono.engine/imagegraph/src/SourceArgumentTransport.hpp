#pragma once

#include "ValuePayload.hpp"

#include <engine/imagegraph/Catalogue.hpp>

namespace engine::imagegraph::detail {
	inline bool SourceArgumentDefault(const Node &node, std::string_view port) {
		if (node.Type != "pc.argument" || port != "default_value") return false;
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto *input = entry ? FindCatalogueInput(*entry, port) : nullptr;
		if (!input || input->SourceIndex != 2 || input->SourceKind != "Text" || input->Type != ValueType::Any)
			return false;
		for (const auto &property : entry->Schema.Properties)
			if (property.Id == port) return property.Type == ValueType::Any;
		return false;
	}
	inline bool SourceArgumentAuthoredDefault(const Node &node, std::string_view port, const Value &value) {
		return SourceArgumentDefault(node, port) &&
			   IsAuthoredValueType(std::visit([](const auto &leaf) { return PayloadType(leaf); }, value));
	}
}
