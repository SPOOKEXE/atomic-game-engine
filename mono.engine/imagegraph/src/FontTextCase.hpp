#pragma once

#include "FontUnicode.hpp"

#include <engine/imagegraph/SourceFont.hpp>

namespace engine::imagegraph::detail {
	// ASCII cases use the native implementation. Other cases borrow exact owned source evidence.
	inline Status FindFontTextCase(
		std::string_view original,
		uint8_t changeCase,
		const SourceFontContext *context,
		std::optional<std::string_view> &output,
		std::string &failure
	) {
		bool nonAscii = false;
		FontScalarCursor cursor{original};
		uint32_t point = 0;
		while (cursor.Next(point))
			nonAscii = nonAscii || point > 127;
		if (cursor.Invalid) {
			failure = "Text casing requires valid Unicode";
			return Status::UnsupportedExecution;
		}
		if (changeCase > 3) {
			failure = "Text casing choice is invalid";
			return Status::InvalidValue;
		}
		if (!changeCase) {
			output.reset();
			return Status::Ok;
		}
		if (!context && !nonAscii) {
			output.reset();
			return Status::Ok;
		}
		if (!context) {
			failure = "non-ASCII Text casing requires a recorded source transformation";
			return Status::UnsupportedExecution;
		}
		if (!SourceFontContextRetainedBytes(*context)) {
			failure = "source Text transformation context is malformed";
			return Status::InvalidValue;
		}
		const SourceFontTextTransform *found = nullptr;
		for (const auto &record : context->TextTransforms)
			if (record.Original == original && record.ChangeCase == changeCase) {
				if (found) {
					failure = "source Text transformation key is duplicated";
					return Status::DuplicateId;
				}
				found = &record;
			}
		if (!found && (!nonAscii || context->TextCaseProfile == FontTextCaseProfile::UnicodeDefault)) {
			output.reset();
			failure.clear();
			return Status::Ok;
		}
		if (!found) {
			failure = "non-ASCII Text casing requires a recorded source transformation";
			return Status::UnsupportedExecution;
		}
		output = found->Transformed;
		failure.clear();
		return Status::Ok;
	}
}
