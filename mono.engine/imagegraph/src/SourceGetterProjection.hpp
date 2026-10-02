#pragma once

// Whole-input source getter conversion precedes processor row selection.
#include "NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	class SourceGetterProjection {
	  public:
		explicit SourceGetterProjection(NodeContext &context) : Context(context) {}
		~SourceGetterProjection();
		SourceGetterProjection(const SourceGetterProjection &) = delete;
		SourceGetterProjection &operator=(const SourceGetterProjection &) = delete;
		// All replacement storage is admitted while original inputs remain live. Failure restores views.
		bool Prepare();

	  private:
		NodeContext &Context;
		AllocationReservation Charge;
		std::vector<std::pair<std::string_view, const Value *>> OriginalViews;
		std::vector<std::pair<std::string_view, Value>> Projected;
		bool Installed = false;
	};
}
