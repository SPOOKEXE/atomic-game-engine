#pragma once
#include <engine/imagegraph/SourceArgumentHost.hpp>

namespace studio::detail {
	// Parsing owns the candidate table before touching any observation generation.
	// The acceptance callback retires only caller-owned runtime observations.
	template <class Accepted>
	bool PrepareImageGraphArguments(
		engine::imagegraph::SourceArgumentHost &host,
		const engine::imagegraph::SourceArgumentOptions &options,
		uint64_t &inputRevision,
		const Accepted &accepted,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		if (inputRevision == UINT64_MAX) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				"arguments",
				"Composer input generation limit reached"
			};
			return false;
		}
		if (host.PrepareOptions(options, maximumBytes, diagnostic) != engine::imagegraph::Status::Ok)
			return false;
		accepted();
		++inputRevision;
		return true;
	}
}
