#include <engine/audio/Observation.hpp>
#include <engine/audio/Spatial.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace engine::audio {

	namespace {
		std::string CommandName(CommandKind kind) {
			switch (kind) {
			case CommandKind::None:
				return "none";
			case CommandKind::AddNode:
				return "add_node";
			case CommandKind::RemoveNode:
				return "remove_node";
			case CommandKind::Connect:
				return "connect";
			case CommandKind::Disconnect:
				return "disconnect";
			case CommandKind::SetSound:
				return "set_sound";
			case CommandKind::Play:
				return "play";
			case CommandKind::Stop:
				return "stop";
			case CommandKind::Rewind:
				return "rewind";
			case CommandKind::SetGain:
				return "set_gain";
			case CommandKind::SetPan:
				return "set_pan";
			case CommandKind::SetMuted:
				return "set_muted";
			case CommandKind::SetLooping:
				return "set_looping";
			case CommandKind::SetPlacement:
				return "set_placement";
			case CommandKind::SetListener:
				return "set_listener";
			}
			return "unknown";
		}

		std::string StableId(NodeId node, std::span<const AudioSourceBinding> sources) {
			const auto found = std::find_if(sources.begin(), sources.end(), [node](const auto &source) {
				return source.Node == node;
			});
			return found == sources.end() ? std::string{} : std::string(found->SourceId);
		}

		std::string Timing(uint64_t requested, uint64_t applied) {
			if (applied < requested) {
				return "early";
			}
			if (applied > requested) {
				return "late";
			}
			return "exact";
		}

		std::string ChannelLayout(uint16_t channels) {
			if (channels == 1) {
				return "mono";
			}
			if (channels == 2) {
				return "stereo";
			}
			return "discrete";
		}
	}

	AudioObservationResult CaptureAudioObservation(
		const AudioMixer &mixer,
		const SampleBuffer &waveform,
		const MixReport &report,
		std::span<const AudioSourceBinding> sources,
		AudioObservationLimits limits
	) {
		if (waveform.Format() != mixer.Format()) {
			return {.Status = AudioObservationStatus::InvalidFormat, .Value = std::nullopt};
		}
		if (report.ObservationSerial == 0 || report.ObservationSerial != mixer.ObservationSerial() ||
			report.EndSample != mixer.Clock() || report.EndSample < report.BeginSample ||
			report.EndSample - report.BeginSample != report.Frames || report.Frames > waveform.Frames()) {
			return {.Status = AudioObservationStatus::StaleMixReport, .Value = std::nullopt};
		}
		if (report.Frames > limits.MaximumFrames) {
			return {.Status = AudioObservationStatus::WaveformLimitExceeded, .Value = std::nullopt};
		}
		if (sources.size() > limits.MaximumSources) {
			return {.Status = AudioObservationStatus::SourceLimitExceeded, .Value = std::nullopt};
		}

		for (size_t index = 0; index < sources.size(); ++index) {
			if (sources[index].SourceId.empty() ||
				sources[index].SourceId.find('\0') != std::string_view::npos ||
				!sources[index].Node.IsValid()) {
				return {.Status = AudioObservationStatus::InvalidSourceBinding, .Value = std::nullopt};
			}
			for (size_t other = 0; other < index; ++other) {
				if (sources[index].SourceId == sources[other].SourceId ||
					sources[index].Node == sources[other].Node) {
					return {.Status = AudioObservationStatus::DuplicateSourceBinding, .Value = std::nullopt};
				}
			}
		}

		std::vector<AudioSourceObservation> capturedSources;
		capturedSources.reserve(sources.size());
		for (const AudioSourceBinding &binding : sources) {
			AudioSourceObservation source;
			source.SourceId = binding.SourceId;
			const Node *node = mixer.Graph().Find(binding.Node);
			if (node != nullptr) {
				source.Present = true;
				source.Kind = Describe(node->Kind);
				source.Gain = node->Gain;
				source.Pan = node->Pan;
				source.Muted = node->Muted;
				source.Playing = node->Playing;
				source.Looping = node->Looping;
				if (node->Kind == NodeKind::Emitter) {
					source.HasEmitterPlacement = true;
					source.Placement = node->Placement;
					source.ListenerDistance = DistanceBetween(mixer.Graph().Listener(), node->Placement);
					source.SpatialGain = Place(mixer.Graph().Listener(), node->Placement);
				}
			}
			capturedSources.push_back(std::move(source));
		}

		std::vector<AudioEventObservation> events;
		events.reserve(mixer.LastAppliedCommands().size() + mixer.LastFinishedSources().size());
		for (const AppliedAudioCommand &applied : mixer.LastAppliedCommands()) {
			const std::string sourceId = StableId(applied.Target, sources);
			const std::string relatedSourceId = StableId(applied.Related, sources);
			events.push_back(
				AudioEventObservation{
					.Kind = CommandName(applied.Kind),
					.SourceId = sourceId,
					.RelatedSourceId = relatedSourceId,
					.SourceIdentified = !sourceId.empty(),
					.RelatedSourceIdentified = !relatedSourceId.empty(),
					.RequestedSample = applied.RequestedSample,
					.AppliedSample = applied.AppliedSample,
					.OffsetFrames = applied.OffsetFrames,
					.Timing = Timing(applied.RequestedSample, applied.AppliedSample),
				}
			);
		}
		for (const FinishedAudioSource &finished : mixer.LastFinishedSources()) {
			const std::string sourceId = StableId(finished.Source, sources);
			events.push_back(
				AudioEventObservation{
					.Kind = "playback_finished",
					.SourceId = sourceId,
					.RelatedSourceId = {},
					.SourceIdentified = !sourceId.empty(),
					.RelatedSourceIdentified = false,
					.RequestedSample = finished.AtSample,
					.AppliedSample = finished.AtSample,
					.OffsetFrames = finished.OffsetFrames,
					.Timing = "exact",
				}
			);
		}
		std::stable_sort(events.begin(), events.end(), [](const auto &left, const auto &right) {
			return left.AppliedSample < right.AppliedSample;
		});

		const size_t sampleCount = report.Frames * static_cast<size_t>(waveform.Format().Channels);
		const std::span<const float> samples = waveform.Data().first(sampleCount);
		AudioObservation observation{
			.BeginSample = report.BeginSample,
			.EndSample = report.EndSample,
			.SampleRate = waveform.Format().SampleRate,
			.Channels = waveform.Format().Channels,
			.ChannelLayout = ChannelLayout(waveform.Format().Channels),
			.SampleType = "float32",
			.Interleaved = true,
			.Waveform = std::vector<float>(samples.begin(), samples.end()),
			.Sources = std::move(capturedSources),
			.Events = std::move(events),
			.Listener = mixer.Graph().Listener(),
			.WaveformProvenance = {.Available = true, .Method = "audio_mixer_post_clip_output", .Reason = {}},
			.TimingProvenance = {.Available = true, .Method = "audio_mixer_sample_clock", .Reason = {}},
			.AttenuationProvenance =
				{
					.Available = true,
					.Method = "inverse_square_distance_equal_power_stereo_pan",
					.Reason = {},
				},
			.OcclusionProvenance = {
				.Available = false,
				.Method = {},
				.Reason = "audio graph has no occlusion or filter node",
			},
		};
		return {.Status = AudioObservationStatus::Ready, .Value = std::move(observation)};
	}
}
