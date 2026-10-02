#include "Families.hpp"

namespace engine::imagegraph::detail {
	namespace {
		// The input sampler supplies the source key-map pulse, including linked Trigger values.
		bool Trigger(NodeContext &context) {
			const auto *value = context.Find("trigger");
			const auto *pulse = value ? std::get_if<bool>(value) : nullptr;
			if (!pulse)
				return context.Fail(Status::TypeMismatch, "Trigger requires a boolean pulse", "trigger");
			context.SetValue("trigger", *pulse);
			return context.FailureCode == Status::Ok;
		}
		constexpr ExecutorEntry TRIGGER_EXECUTORS[]{{"pc.trigger", Trigger}};
	}
	std::span<const ExecutorEntry> TriggerExecutors() {
		return TRIGGER_EXECUTORS;
	}
}
