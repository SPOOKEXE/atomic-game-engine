#include "../FontTextBatch.hpp"
#include "Families.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool Text(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.text");
			if (!context.TextBatch)
				return context.Fail(Status::InvalidValue, "Text requires its admitted processor row plan");
			return RenderFontTextRow(context, *context.TextBatch);
		}
	}
	std::span<const ExecutorEntry> SourceFontTextExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.text", Text, false}};
		return entries;
	}
}
