#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>

#include <algorithm>
#include <limits>

namespace engine::imagegraphfont {
	namespace {
		using namespace imagegraph;
		bool FontInputSpend(uint64_t &bytes, uint64_t count, uint64_t unit = 1) {
			if (bytes > Limits::MaximumEvaluationBytes ||
				count > (Limits::MaximumEvaluationBytes - bytes) / unit)
				return false;
			bytes += count * unit;
			return true;
		}
		bool FontInputFail(Diagnostic &diagnostic, Status status, const char *message) {
			diagnostic = {status, {}, "font_inputs", message};
			return false;
		}
	}
	std::optional<uint64_t> GraphFontConfigurationRetainedBytes(const GraphFontConfiguration &configuration) {
		const auto context = SourceFontContextRetainedBytes(configuration.Context);
		if (!context || configuration.Observations.size() > Limits::MaximumNodes ||
			configuration.ReadGrants.size() > Limits::MaximumNodes)
			return {};
		uint64_t keyBytes = 0;
		for (const auto &grant : configuration.ReadGrants) {
			if (grant.NodeId.size() > Limits::MaximumTextBytes ||
				grant.Resource.size() > Limits::MaximumTextBytes ||
				grant.File.native().size() > Limits::MaximumTextBytes ||
				!FontInputSpend(keyBytes, grant.NodeId.size()) ||
				!FontInputSpend(keyBytes, grant.Resource.size()) ||
				!FontInputSpend(
					keyBytes, grant.File.native().size(), sizeof(std::filesystem::path::value_type)
				))
				return {};
		}
		if (!configuration.ReadGrants.empty() &&
			keyBytes > 16 * 1024 * 1024 / configuration.ReadGrants.size())
			return {};
		uint64_t bytes = sizeof(configuration);
		if (!FontInputSpend(bytes, *context) ||
			!FontInputSpend(bytes, configuration.Observations.capacity(), sizeof(SourceFontObservation)) ||
			!FontInputSpend(bytes, configuration.ReadGrants.capacity(), sizeof(GraphFontFileGrant)))
			return {};
		for (const auto &observation : configuration.Observations) {
			const auto amount = SourceFontObservationRetainedBytes(observation);
			if (!amount || !FontInputSpend(bytes, *amount)) return {};
		}
		for (size_t i = 0; i < configuration.ReadGrants.size(); ++i) {
			const auto &grant = configuration.ReadGrants[i];
			if (grant.Write || grant.NodeId.empty() || grant.File.empty() ||
				grant.NodeId.size() > Limits::MaximumTextBytes ||
				grant.File.native().size() > Limits::MaximumTextBytes ||
				grant.Resource.size() > Limits::MaximumTextBytes ||
				!FontInputSpend(bytes, grant.NodeId.capacity() + 1) ||
				!FontInputSpend(bytes, grant.Resource.capacity() + 1) ||
				!FontInputSpend(
					bytes, grant.File.native().capacity() + 1, sizeof(std::filesystem::path::value_type)
				))
				return {};
			for (size_t j = 0; j < i; ++j)
				if (grant.NodeId == configuration.ReadGrants[j].NodeId &&
					grant.File == configuration.ReadGrants[j].File &&
					grant.Resource == configuration.ReadGrants[j].Resource)
					return {};
		}
		return bytes;
	}
	bool GraphFontInputs::Replace(
		const GraphFontConfiguration &configuration,
		const assets::ContentPolicy &policy,
		uint64_t maximumBytes,
		imagegraph::Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraphfont.font_inputs_replace");
		const auto bytes = GraphFontConfigurationRetainedBytes(configuration);
		if (!bytes)
			return FontInputFail(
				diagnostic, imagegraph::Status::InvalidValue, "font configuration is malformed"
			);
		if (Version == std::numeric_limits<uint64_t>::max())
			return FontInputFail(
				diagnostic, imagegraph::Status::LimitExceeded, "font input revision exceeds bound"
			);
		const uint64_t maximum = std::min(maximumBytes, imagegraph::Limits::MaximumEvaluationBytes);
		const uint64_t resident = RetainedBytes();
		// Borrowed input, old owner, candidate clone and provider's independent exact grant table coexist.
		if (resident > maximum || *bytes > (maximum - resident) / 4)
			return FontInputFail(
				diagnostic,
				imagegraph::Status::LimitExceeded,
				"font configuration replacement exceeds coexistence budget"
			);
		if (imagegraph::ValidateSourceFontObservations(
				&configuration.Context, configuration.Observations, maximum, diagnostic
			) != imagegraph::Status::Ok)
			return false;
		GraphFontConfiguration candidate = configuration;
		const auto retained = GraphFontConfigurationRetainedBytes(candidate);
		if (!retained || *retained > maximum - resident - *bytes)
			return FontInputFail(
				diagnostic, imagegraph::Status::LimitExceeded, "font configuration candidate exceeds budget"
			);
		auto provider = std::make_unique<GraphFontHost>(
			candidate.ReadGrants, policy, maximum - resident - *bytes - *retained
		);
		if (provider->RetainedBytes() > maximum - resident - *bytes ||
			*retained > maximum - resident - *bytes - provider->RetainedBytes())
			return FontInputFail(
				diagnostic,
				imagegraph::Status::LimitExceeded,
				"font configuration retained capacities exceed budget"
			);
		const uint64_t retainedBytes = *retained;
		Owned = std::move(candidate);
		Provider = std::move(provider);
		Bytes = retainedBytes;
		++Version;
		core::Metrics::Count("imagegraphfont.font.configuration_payload_bytes", Bytes);
		diagnostic = {};
		return true;
	} catch (...) {
		return FontInputFail(
			diagnostic, imagegraph::Status::LimitExceeded, "font configuration allocation failed"
		);
	}
	bool GraphFontInputs::Bind(
		bool playing,
		imagegraph::SourceFontContext &heldContext,
		imagegraph::EvaluationRequest &request,
		uint64_t maximumBytes,
		imagegraph::Diagnostic &diagnostic
	) const try {
		ENGINE_PROFILE("imagegraphfont.font_inputs_bind");
		if (!Provider)
			return FontInputFail(
				diagnostic, imagegraph::Status::UnsupportedExecution, "font inputs are not configured"
			);
		const auto context = imagegraph::SourceFontContextRetainedBytes(Owned.Context);
		const auto prior = imagegraph::SourceFontContextRetainedBytes(heldContext);
		const uint64_t maximum = std::min(maximumBytes, imagegraph::Limits::MaximumEvaluationBytes);
		const uint64_t resident = RetainedBytes();
		if (!context || !prior || resident > maximum || *prior > maximum - resident ||
			*context > (maximum - resident - *prior) / 2)
			return FontInputFail(
				diagnostic,
				imagegraph::Status::LimitExceeded,
				"held font playback context exceeds coexistence budget"
			);
		auto candidate = Owned.Context;
		candidate.Playing = playing;
		const auto actual = imagegraph::SourceFontContextRetainedBytes(candidate);
		if (!actual || *actual > maximum - resident - *prior)
			return FontInputFail(
				diagnostic, imagegraph::Status::LimitExceeded, "held font context capacity exceeds budget"
			);
		core::Metrics::Count("imagegraphfont.font.held_context_payload_bytes", *actual);
		heldContext = std::move(candidate);
		request.SourceFonts = &heldContext;
		request.FontObservations = Owned.Observations;
		request.FontProvider = Provider.get();
		request.SourceFontHostResidentBytes = Bytes;
		diagnostic = {};
		return true;
	} catch (...) {
		return FontInputFail(
			diagnostic, imagegraph::Status::LimitExceeded, "font playback context allocation failed"
		);
	}
}
