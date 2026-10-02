#pragma once

#include <new>
#include <studio/ImageGraph.hpp>
#include <type_traits>

namespace studio {
	// The host owns preview invalidation; this transaction owns the authored undo entry.
	// An unchanged result requires the entire staged edit callback to succeed.
	template <class Edit>
	bool ApplyImageGraphDocumentEdit(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		const Edit &edit,
		bool *unchanged = nullptr
	) try {
		if (unchanged) *unchanged = false;
		engine::imagegraph::Document staged = document;
		if constexpr (std::is_same_v<std::invoke_result_t<Edit, engine::imagegraph::Document &>, bool>) {
			if (!edit(staged)) return false;
		} else {
			edit(staged);
		}
		if (document == staged) {
			if (unchanged) *unchanged = true;
			return false;
		}
		if (!history.TryRecord(document, staged)) return false;
		document = std::move(staged);
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
}
