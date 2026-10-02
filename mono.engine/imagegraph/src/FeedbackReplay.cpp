#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FeedbackReplay.hpp>

#include <algorithm>
#include <limits>
#include <new>

namespace engine::imagegraph {
	namespace {
		bool AddBytes(uint64_t &bytes, uint64_t amount, uint64_t maximum) {
			if (bytes > maximum || amount > maximum - bytes) return false;
			bytes += amount;
			return true;
		}
		bool SourceBytes(std::span<const RequestImageSource> sources, uint64_t &bytes, uint64_t maximum) {
			if (sources.size() > Limits::MaximumOutputs ||
				!AddBytes(bytes, sources.size() * sizeof(RequestImageSource), maximum))
				return false;
			for (const auto &source : sources)
				if (source.SourceId.empty() || source.SourceId.size() > Limits::MaximumTextBytes ||
					!ValidSurfaceLayout(source.Data, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
					!AddBytes(bytes, source.SourceId.capacity(), maximum) ||
					!AddBytes(bytes, source.Data.Pixels.capacity(), maximum) ||
					!FiniteSurfaceSamples(source.Data))
					return false;
			return true;
		}
		bool BindingBytes(std::span<const FeedbackBinding> bindings, uint64_t &bytes, uint64_t maximum) {
			if (bindings.empty() || bindings.size() > Limits::MaximumOutputs ||
				!AddBytes(bytes, bindings.size() * sizeof(FeedbackBinding), maximum))
				return false;
			for (const auto &binding : bindings)
				if (binding.SourceId.empty() || binding.OutputId.empty() ||
					binding.SourceId.size() > Limits::MaximumTextBytes ||
					binding.OutputId.size() > Limits::MaximumTextBytes ||
					!AddBytes(bytes, binding.SourceId.capacity() + binding.OutputId.capacity(), maximum))
					return false;
			return true;
		}
	}
	Status ReplayFeedbackFrame(
		const Document &document,
		const Plan &plan,
		std::span<const FeedbackBinding> bindings,
		std::span<const RequestImageSource> seeds,
		const EvaluationRequest &request,
		const FeedbackReplayState &previous,
		uint64_t revision,
		bool reset,
		FeedbackReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.feedback");
		const auto fail = [&](Status status, const char *message) {
			diagnostic = {status, {}, {}, message};
			return status;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "feedback byte cap is outside native bounds");
		if (request.Subframe != 0 || request.NegativeFrame || !request.ImageSources.empty() ||
			(reset ? request.Tick != 0
				   : !previous.Initialized || previous.Tick >= Limits::MaximumTick ||
						 request.Tick != previous.Tick + 1 || previous.AuthoringRevision != revision))
			return fail(
				Status::InvalidValue,
				"feedback requires reset at zero or contiguous fixed ticks at the same revision"
			);
		uint64_t heldBytes = sizeof(FeedbackReplayState) * 2;
		if (!BindingBytes(bindings, heldBytes, maximumBytes) ||
			!SourceBytes(seeds, heldBytes, maximumBytes) ||
			!SourceBytes(previous.Sources, heldBytes, maximumBytes) ||
			(!previous.Bindings.empty() && !BindingBytes(previous.Bindings, heldBytes, maximumBytes)) ||
			(&previous != &result &&
			 (!SourceBytes(result.Sources, heldBytes, maximumBytes) ||
			  (!result.Bindings.empty() && !BindingBytes(result.Bindings, heldBytes, maximumBytes)))))
			return fail(
				Status::LimitExceeded, "feedback retained inputs or replacement overlap exceed bounds"
			);
		if (!reset && (previous.Bindings.size() != bindings.size() ||
					   !std::equal(bindings.begin(), bindings.end(), previous.Bindings.begin())))
			return fail(Status::InvalidValue, "feedback bindings changed without reset");
		const auto sources = reset ? seeds : std::span<const RequestImageSource>(previous.Sources);
		if (sources.size() != bindings.size())
			return fail(Status::InvalidValue, "feedback requires one seed per binding");
		for (size_t index = 0; index < bindings.size(); index++) {
			if (std::count_if(bindings.begin(), bindings.end(), [&](const auto &binding) {
					return binding.SourceId == bindings[index].SourceId;
				}) != 1)
				return fail(Status::DuplicateId, "feedback source IDs must be unique");
			if (std::count_if(sources.begin(), sources.end(), [&](const auto &source) {
					return source.SourceId == bindings[index].SourceId;
				}) != 1)
				return fail(Status::InvalidValue, "feedback seed IDs must match bindings");
		}
		if (!AddBytes(heldBytes, bindings.size() * sizeof(RequestImageSource), maximumBytes))
			return fail(Status::LimitExceeded, "feedback output slots exceed bounds");
		for (const auto &binding : bindings)
			if (!AddBytes(
					heldBytes, std::max(binding.SourceId.size(), std::string{}.capacity()), maximumBytes
				))
				return fail(Status::LimitExceeded, "feedback output names exceed bounds");
		if (!BindingBytes(bindings, heldBytes, maximumBytes) ||
			!AddBytes(
				heldBytes,
				(previous.Sources.capacity() - previous.Sources.size()) * sizeof(RequestImageSource),
				maximumBytes
			) ||
			!AddBytes(
				heldBytes,
				(previous.Bindings.capacity() - previous.Bindings.size()) * sizeof(FeedbackBinding),
				maximumBytes
			) ||
			(&previous != &result &&
			 (!AddBytes(
				  heldBytes,
				  (result.Sources.capacity() - result.Sources.size()) * sizeof(RequestImageSource),
				  maximumBytes
			  ) ||
			  !AddBytes(
				  heldBytes,
				  (result.Bindings.capacity() - result.Bindings.size()) * sizeof(FeedbackBinding),
				  maximumBytes
			  ))))
			return fail(Status::LimitExceeded, "feedback replacement metadata exceeds bounds");
		FeedbackReplayState candidate;
		candidate.Tick = request.Tick;
		candidate.AuthoringRevision = revision;
		candidate.Initialized = true;
		candidate.Bindings.assign(bindings.begin(), bindings.end());
		candidate.Sources.reserve(bindings.size());
		EvaluationRequest clock = request;
		clock.ImageSources = sources;
		for (const auto &binding : bindings) {
			if (heldBytes >= maximumBytes)
				return fail(Status::LimitExceeded, "feedback evaluation has no free workspace");
			Image image;
			const Status evaluated = Evaluate(
				document, plan, binding.OutputId, clock, image, diagnostic, maximumBytes - heldBytes
			);
			if (evaluated != Status::Ok) return evaluated;
			if (!AddBytes(heldBytes, image.Pixels.capacity(), maximumBytes))
				return fail(Status::LimitExceeded, "feedback outputs exceed retained byte bounds");
			candidate.Sources.push_back({binding.SourceId, std::move(image)});
		}
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "feedback allocation was refused"};
		return diagnostic.Code;
	}
	Status SeekFeedbackReplay(
		const Document &document,
		const Plan &plan,
		std::span<const FeedbackBinding> bindings,
		std::span<const RequestImageSource> seeds,
		const EvaluationRequest &target,
		uint64_t revision,
		FeedbackReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumSteps,
		uint64_t maximumBytes
	) {
		if (!maximumSteps || maximumSteps > 4096 || target.Tick >= maximumSteps || target.Subframe != 0 ||
			target.NegativeFrame || !maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
			diagnostic = {Status::LimitExceeded, {}, {}, "feedback seek exceeds fixed-step or byte bounds"};
			return diagnostic.Code;
		}
		uint64_t retainedBytes = sizeof(FeedbackReplayState);
		if (!SourceBytes(result.Sources, retainedBytes, maximumBytes) ||
			(!result.Bindings.empty() && !BindingBytes(result.Bindings, retainedBytes, maximumBytes)) ||
			!AddBytes(
				retainedBytes,
				(result.Sources.capacity() - result.Sources.size()) * sizeof(RequestImageSource),
				maximumBytes
			) ||
			!AddBytes(
				retainedBytes,
				(result.Bindings.capacity() - result.Bindings.size()) * sizeof(FeedbackBinding),
				maximumBytes
			) ||
			retainedBytes >= maximumBytes) {
			diagnostic = {Status::LimitExceeded, {}, {}, "feedback seek replacement overlap exceeds bounds"};
			return diagnostic.Code;
		}
		FeedbackReplayState candidate;
		EvaluationRequest clock = target;
		for (uint64_t tick = 0; tick <= target.Tick; tick++) {
			clock.Tick = tick;
			const Status status = ReplayFeedbackFrame(
				document,
				plan,
				bindings,
				seeds,
				clock,
				candidate,
				revision,
				tick == 0,
				candidate,
				diagnostic,
				maximumBytes - retainedBytes
			);
			if (status != Status::Ok) return status;
		}
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}

}
