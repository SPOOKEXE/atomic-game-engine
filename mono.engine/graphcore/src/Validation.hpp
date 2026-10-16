#pragma once

#include <engine/graphcore/Schema.hpp>

namespace engine::graphcore::detail {

	CompileStatus Refuse(
		Diagnostic &diagnostic, CompileStatus status, std::string_view node = {}, std::string_view port = {}
	);

	CompileStatus CountIdentity(
		std::string_view text,
		size_t &bytes,
		const Limits &limits,
		Diagnostic &diagnostic,
		std::string_view node,
		std::string_view port = {}
	);

	CompileStatus CheckSchemaBudgets(
		std::span<const NodeSchema> schemas, const Limits &limits, size_t &bytes, Diagnostic &diagnostic
	);
}
