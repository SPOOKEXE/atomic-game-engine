#pragma once

// Private access to the document value text reader, shared with the source catalogue.

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <string_view>

namespace engine::imagegraph::detail {
	// Reads one complete value in current document text form, such as "d 0.5" or "c 255 255 255 255".
	bool ReadValueText(std::string_view text, Value &value);
	// Admits parser workspace and owned payload before allocating; publication moves its charge.
	Status ReadValueText(
		std::string_view text, Value &value, EvaluationBudget &budget, AllocationReservation &charge
	);
}
