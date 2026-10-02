#pragma once
#include "NodeExecutors.hpp"
#include "ParticlePayload.hpp"
namespace engine::imagegraph::detail {
	// Source renderer consumes one flat particle pool or a flat array of pools.
	template <class Draw> bool VisitSourceVfxParticles(NodeContext &context, const Value *input, Draw draw) {
		const auto *array = input ? std::get_if<ArrayValue>(input) : nullptr;
		if (!array) return true;
		uint64_t count = 0;
		const auto leaf = [&](const ElementValue &value) {
			if (++count > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "VFX particle traversal exceeds bounded work");
			const auto *particle = std::get_if<ParticleValue>(&value);
			if (!particle || !particle->Data || !ValidParticlePayload(*particle))
				return context.Fail(Status::InvalidValue, "VFX renderer reads an undefined particle object");
			return draw(*particle->Data);
		};
		if (!array->Items.empty()) {
			const bool pools =
				std::holds_alternative<std::vector<SourceArrayItem>>(array->Items.front().Data);
			for (const auto &item : array->Items) {
				if (pools) {
					const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
					if (!row)
						return context.Fail(
							Status::InvalidValue, "VFX renderer pool rows have inconsistent depth"
						);
					for (const auto &child : *row) {
						const auto *value = std::get_if<ElementValue>(&child.Data);
						if (!value || !leaf(*value))
							return context.FailureCode != Status::Ok
									   ? false
									   : context.Fail(
											 Status::InvalidValue, "VFX renderer pool exceeds source depth"
										 );
					}
				} else {
					const auto *value = std::get_if<ElementValue>(&item.Data);
					if (!value || !leaf(*value))
						return context.FailureCode != Status::Ok
								   ? false
								   : context.Fail(
										 Status::InvalidValue, "VFX renderer pool exceeds source depth"
									 );
				}
			}
		} else if (!array->Nested.empty()) {
			for (const auto &row : array->Nested)
				for (const auto &value : row)
					if (!leaf(value)) return false;
		} else
			for (const auto &value : array->Elements)
				if (!leaf(value)) return false;
		return true;
	}
}
