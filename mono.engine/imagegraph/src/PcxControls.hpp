#pragma once
#include "NodeExecutors.hpp"
namespace engine::imagegraph::detail {
	struct PcxInputProgram {
		const Node *Owner;
		const SourceInputExpression *Expression;
	};
	bool ApplyPcxInputExpressions(NodeContext &context, std::span<const PcxInputProgram> programs);
}
