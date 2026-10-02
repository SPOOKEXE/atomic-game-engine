#include "AudioPayload.hpp"
#include "EvaluationBudget.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/WavTimelinePresentation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
namespace engine::imagegraph {
	static Status ResolveImpl(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		double fps,
		uint64_t byteBudget,
		WavTimelinePresentation &output,
		Diagnostic &diagnostic
	) {
		const auto fail = [&](Status code, std::string_view port, const char *text) {
			diagnostic = {code, std::string(nodeId), std::string(port), text};
			return code;
		};
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &value) {
			return value.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.wav_file_read")
			return fail(Status::InvalidValue, {}, "WAV timeline observation needs a WAV File In node");
		if (!std::isfinite(fps) || fps <= 0)
			return fail(Status::InvalidValue, {}, "WAV timeline requires positive finite FPS");
		const uint64_t cap = std::min(byteBudget, Limits::MaximumEvaluationBytes);
		detail::EvaluationBudget ledger(cap);
		auto old = ledger.Reserve(output.Points.capacity() * sizeof(Vector2));
		if (!old) return fail(Status::LimitExceeded, {}, "retained waveform exceeds observation cap");
		EvaluationSnapshot snapshot;
		const auto status =
			EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic, ledger.Available());
		if (status != Status::Ok) return status;
		auto captured = ledger.Reserve(snapshot.RetainedBytes());
		if (!captured) return fail(Status::LimitExceeded, {}, "captured inputs exceed observation cap");
		const std::string *path = nullptr;
		for (const auto &value : snapshot.Values())
			if (value.Port == "path") path = std::get_if<std::string>(&value.Data);
		if (!path)
			return fail(Status::UnsupportedExecution, "path", "WAV observation needs resolved scalar path");
		if (request.AudioClips.size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "path", "WAV source count exceeds observation limit");
		const AudioBit *clip = nullptr;
		for (const auto &asset : request.AudioClips)
			if (asset.SourceId == *path) {
				if (clip) return fail(Status::DuplicateId, "path", "WAV source identity is duplicated");
				clip = &asset.Data;
			}
		if (!clip) return fail(Status::InvalidValue, "path", "WAV source asset is missing");
		return BuildWavTimelinePresentation(
			*clip, GetFrameTime(request), fps, cap - snapshot.RetainedBytes(), output, diagnostic
		);
	}
	static Status BuildImpl(
		const AudioBit &clip,
		FrameTime frame,
		double fps,
		uint64_t byteBudget,
		WavTimelinePresentation &output,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.wav_timeline.geometry");
		const auto fail = [&](Status status, std::string_view port, const char *message) {
			diagnostic = {status, {}, std::string(port), message};
			return status;
		};
		if (!ValidFrameTime(frame) || !std::isfinite(fps) || fps <= 0)
			return fail(Status::InvalidValue, {}, "waveform needs valid signed clock and FPS");
		detail::EvaluationBudget ledger(std::min(byteBudget, Limits::MaximumEvaluationBytes));
		auto old = ledger.Reserve(output.Points.capacity() * sizeof(Vector2));
		if (!old) return fail(Status::LimitExceeded, {}, "old waveform exceeds live observation cap");
		if (!std::isfinite(clip.SampleRate) || clip.SampleRate <= 0 ||
			clip.SampleRate > std::numeric_limits<uint32_t>::max() ||
			std::floor(clip.SampleRate) != clip.SampleRate ||
			!detail::ValidAudioPlanes(clip.Samples, clip.Channels, Limits::MaximumAudioClipSamples))
			return fail(Status::InvalidValue, "path", "WAV source clip has invalid planes");
		const auto channel = detail::AudioChannel(clip, 0);
		const double rawStep = clip.SampleRate / fps;
		const double base = std::floor(rawStep), fraction = rawStep - base;
		const double step = base + (fraction > .5 || (fraction == .5 && std::fmod(base, 2.) != 0.) ? 1 : 0);
		if (!channel.empty() && (!std::isfinite(step) || step < 1))
			return fail(Status::InvalidValue, {}, "source waveform sample stride is zero or unrepresentable");
		const size_t stride = channel.empty() || step > channel.size() ? channel.size() + 1 : size_t(step);
		size_t count = channel.empty() ? 0 : channel.size() / stride + 1;
		// GML <= uses the documented default epsilon; no override was found in the pinned scripts.
		// This is an inferred runtime default, not a licensed executable observation.
		if (!channel.empty() && std::abs(double(count) - double(channel.size()) / step) <= .00001) ++count;
		auto next = ledger.Reserve(count * sizeof(Vector2));
		if (!next)
			return fail(Status::LimitExceeded, {}, "waveform point storage exceeds live observation cap");
		WavTimelinePresentation candidate;
		candidate.Points.reserve(count);
		if (!next->Resize(candidate.Points.capacity() * sizeof(Vector2)))
			return fail(Status::LimitExceeded, {}, "waveform point capacity exceeds live observation cap");
		for (size_t i = 0; i < count; ++i)
			candidate.Points.push_back({double(i), channel[std::min(i * stride, channel.size() - 1)]});
		candidate.Channels = detail::AudioChannelCount(clip);
		candidate.Duration = double(channel.size()) / clip.SampleRate;
		candidate.Progress =
			channel.empty() ? 0
							: double(std::clamp(FrameTimeToReal(frame) / fps / candidate.Duration, 0.L, 1.L));
		core::Metrics::Count(
			"imagegraph.wav_timeline.allocated_payload_bytes", candidate.Points.capacity() * sizeof(Vector2)
		);
		core::Metrics::Count("imagegraph.wav_timeline.allocations", count ? 1 : 0);
		output = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}

	Status ResolveWavTimelinePresentation(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		double fps,
		uint64_t byteBudget,
		WavTimelinePresentation &output,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.wav_timeline.observe");
		try {
			return ResolveImpl(document, plan, nodeId, request, fps, byteBudget, output, diagnostic);
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "waveform allocation failed"};
		} catch (const std::length_error &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "waveform allocation length exceeded"};
		}
		return diagnostic.Code;
	}
	Status BuildWavTimelinePresentation(
		const AudioBit &clip,
		FrameTime frame,
		double fps,
		uint64_t byteBudget,
		WavTimelinePresentation &output,
		Diagnostic &diagnostic
	) {
		try {
			return BuildImpl(clip, frame, fps, byteBudget, output, diagnostic);
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "waveform allocation failed"};
		} catch (const std::length_error &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "waveform allocation length exceeded"};
		}
		return diagnostic.Code;
	}

} // namespace engine::imagegraph
