#include "../ValuePayload.hpp"
#include "Families.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool Argument(NodeContext &context) {
			const double mode = context.SourceChoice("type");
			if (context.FailureCode != Status::Ok) return false;
			if (mode != 0 && mode != 1)
				return context.Fail(
					Status::UnsupportedExecution, "argument Type has no source branch", "type"
				);
			if (!ReplayRecordedHostOutputs(context)) return false;
			const Value *value = nullptr;
			for (const auto &output : context.OutputValues)
				if (output.Port == "value") value = &output.Data;
			if (!value || !ValidRuntimeValue(*value) ||
				(mode == 1 && !std::holds_alternative<bool>(*value) &&
				 !std::holds_alternative<int64_t>(*value) && !std::holds_alternative<double>(*value)))
				return context.Fail(
					Status::UnsupportedExecution,
					"argument recording has no bounded source mode payload",
					"value"
				);
			return context.SetOutputDomain(
				"value",
				{mode == 0 ? ValueType::Text : ValueType::Scalar,
				 std::nullopt,
				 mode == 0 ? SourceSocketKind::Text : SourceSocketKind::Float}
			);
		}
	}
	std::span<const ExecutorEntry> SourceArgumentExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.argument", Argument, true}};
		return entries;
	}
}
