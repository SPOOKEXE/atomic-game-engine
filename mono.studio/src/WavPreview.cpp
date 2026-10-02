#include <engine/imagegraph/WavPreview.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <studio/WavPreview.hpp>

namespace studio {
	using namespace engine::imagegraph;
	namespace {
		bool Fail(Diagnostic &diagnostic, Status status, const char *message) {
			diagnostic = {status, {}, {}, message};
			return false;
		}
		uint64_t ClipBytes(std::span<const AudioClipSource> sources) {
			constexpr uint64_t budget = Limits::MaximumEvaluationBytes;
			uint64_t bytes = 0;
			const auto add = [&](uint64_t count, uint64_t stride) {
				if (count > (budget - bytes) / stride) return false;
				bytes += count * stride;
				return true;
			};
			if (sources.size() > Limits::MaximumNodes) return budget + 1;
			for (const auto &source : sources) {
				if (!add(1, sizeof(AudioClipSource)) || !add(source.SourceId.size(), 1) ||
					!add(source.Data.Samples.size(), sizeof(double)))
					return budget + 1;
				for (const auto &channel : source.Data.Channels)
					if (!add(1, sizeof(channel)) || !add(channel.size(), sizeof(double))) return budget + 1;
			}
			return bytes;
		}
	}
	bool SyncImageGraphWavTimeline(
		Document &document,
		const std::string &nodeId,
		const EvaluationRequest &request,
		const ImageGraphPlayback &playback,
		Diagnostic &diagnostic
	) {
		if (request.MaximumImageDimension == 0 || request.MaximumImageDimension > Limits::MaximumDimension)
			return Fail(
				diagnostic, Status::InvalidValue, "evaluation image dimension cap is outside native limits"
			);
		EvaluationRequest boundedRequest = request;
		boundedRequest.MaximumImageDimension =
			std::min(request.MaximumImageDimension, IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION);
		WavPreviewControls controls;
		if (ResolveWavPreviewControls(document, nodeId, boundedRequest, controls, diagnostic) != Status::Ok)
			return false;
		uint64_t frames = 0;
		if (WavPreviewSyncFrames(controls, playback.FramesPerSecond, frames, diagnostic) != Status::Ok)
			return false;
		TimelineSettings timeline{
			frames,
			std::min(playback.StartTick, frames - 1),
			std::min(playback.EndTick, frames - 1),
			playback.PingPong ? "pingpong"
			: playback.Loop	  ? "loop"
							  : "stop",
			playback.FramesPerSecond
		};
		return SetImageGraphTimeline(document, timeline, diagnostic);
	}

	struct ImageGraphWavPreview::Owner {
		struct Prepared {
			std::string SourceId;
			engine::audio::SoundRef Sound;
		};
		struct Voice {
			std::string NodeId;
			engine::audio::NodeId Player;
			WavPreviewControls Controls;
			uint64_t Generation = 0;
			bool Pending = false;
			bool Known = true;
			bool Playing = false;
			uint64_t DocumentRevision = 0;
			uint64_t InputRevision = 0;
			uint64_t Tick = std::numeric_limits<uint64_t>::max();
			double Subframe = 0;
			bool NegativeFrame = false;
			std::optional<FrameTime> LastStartFrame;
		};
		DeviceFactory Factory;
		std::unique_ptr<engine::audio::Device> Device;
		std::vector<Prepared> Sounds;
		std::vector<Voice> Voices;
		uint64_t NextGeneration = 1;
		uint64_t DroppedEvents = 0;
		bool Failed = false;
		uint64_t SoundBytes() const {
			uint64_t bytes = 0;
			for (const auto &sound : Sounds)
				bytes += sizeof(Prepared) + sizeof(engine::audio::SampleBuffer) + sound.SourceId.size() +
						 sound.Sound->Data().size_bytes();
			return bytes;
		}
		// This owner is the sole producer. Applying future commands here is intentional:
		// removal must also cancel a scheduled SetSound before releasing its final pin.
		bool Retire(Diagnostic &diagnostic) {
			if (!Device) {
				Voices.clear();
				Sounds.clear();
				return true;
			}
			if (Failed)
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"audio preview must be closed after an ownership failure"
				);
			const bool wasPaused = Device->Paused();
			if (!Device->SetPaused(true)) {
				Failed = true;
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"audio preview could not pause safely; sources are retained"
				);
			}
			auto &mixer = Device->Mixer();
			mixer.ApplyPending();
			for (const auto &voice : Voices) {
				engine::audio::Command command;
				command.Kind = engine::audio::CommandKind::RemoveNode;
				command.Target = voice.Player;
				command.PlaybackGeneration = 0;
				if (!mixer.Commands().Post(command)) {
					Failed = true;
					return Fail(diagnostic, Status::LimitExceeded, "audio preview removal queue is full");
				}
				if (mixer.Graph().Find(voice.Player)) {
					mixer.ApplyPending();
					if (mixer.Graph().Find(voice.Player)) {
						Failed = true;
						return Fail(
							diagnostic,
							Status::UnsupportedExecution,
							"audio preview could not unbind its source"
						);
					}
				}
			}
			mixer.ApplyPending();
			engine::audio::SampleBuffer empty(Device->Format(), 0);
			const auto report = mixer.Render(empty);
			if (!Device->Format().IsValid() || Device->Format() != mixer.Format() ||
				empty.Format() != Device->Format() || report.Frames != 0 ||
				report.BeginSample != report.EndSample || mixer.Commands().Pending() != 0) {
				Failed = true;
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"audio preview could not drain its pending samples"
				);
			}
			Voices.clear();
			Sounds.clear();
			std::array<engine::audio::PlaybackEvent, 64> events;
			while (mixer.PollPlaybackEvents(events) != 0) {}
			DroppedEvents = mixer.PlaybackEventsDropped();
			if (!wasPaused && !Device->SetPaused(false)) {
				Failed = true;
				return Fail(diagnostic, Status::UnsupportedExecution, "audio preview could not resume");
			}
			return true;
		}
		bool Poll(Diagnostic &diagnostic) {
			auto &mixer = Device->Mixer();
			std::array<engine::audio::PlaybackEvent, 64> events;
			bool refused = false;
			bool exhausted = false;
			for (size_t count; (count = mixer.PollPlaybackEvents(events)) != 0;) {
				for (const auto &event : std::span(events).first(count)) {
					const auto player =
						event.NaturalCompletion ? event.Finished.Source : event.Applied.Target;
					for (auto &voice : Voices) {
						if (!voice.Player.IsValid() || voice.Player != player) continue;
						if (event.NaturalCompletion) {
							if (voice.Generation != event.PlaybackGeneration) continue;
						} else {
							if (event.Applied.RequestedPlaybackGeneration != voice.Generation) continue;
							if (event.Status != engine::audio::PlaybackStatus::Applied ||
								voice.Generation != event.PlaybackGeneration) {
								voice.Known = false;
								if (event.PlaybackGeneration == std::numeric_limits<uint64_t>::max())
									exhausted = true;
								else
									NextGeneration = std::max(NextGeneration, event.PlaybackGeneration + 1);
								refused = true;
								continue;
							}
						}
						voice.Known = event.Present;
						voice.Playing = event.Playing;
						if (event.NaturalCompletion ||
							event.Applied.Kind == engine::audio::CommandKind::Play ||
							event.Applied.Kind == engine::audio::CommandKind::Stop)
							voice.Pending = false;
					}
				}
			}
			if (exhausted) {
				Failed = true;
				return Fail(diagnostic, Status::LimitExceeded, "audio preview generation budget exhausted");
			}
			if (mixer.PlaybackEventsDropped() != DroppedEvents || refused) return Retire(diagnostic);
			return true;
		}
		engine::audio::SoundRef Prepare(
			const AudioClipSource &source, uint64_t residentBytes, Diagnostic &diagnostic, bool reuse = true
		) {
			const auto found = std::find_if(Sounds.begin(), Sounds.end(), [&](const Prepared &item) {
				return item.SourceId == source.SourceId;
			});
			if (reuse && found != Sounds.end()) return found->Sound;
			const uint64_t bytes = source.Data.Channels.empty()
									   ? source.Data.Samples.size() * sizeof(float)
									   : source.Data.Channels.front().size() * sizeof(float);
			const uint64_t budget = Limits::MaximumEvaluationBytes;
			const uint64_t metadata =
				sizeof(Prepared) + sizeof(engine::audio::SampleBuffer) + source.SourceId.size();
			const uint64_t existingSounds = SoundBytes();
			if (residentBytes > budget || existingSounds > budget - residentBytes ||
				metadata > budget - residentBytes - existingSounds ||
				bytes > (budget - residentBytes - existingSounds - metadata) / 2)
				return Fail(
						   diagnostic, Status::LimitExceeded, "audio preview exceeds the resident byte budget"
					   ),
					   nullptr;
			std::vector<float> samples;
			if (BuildWavPreviewSamples(
					source.Data,
					budget - residentBytes - existingSounds - metadata - bytes,
					samples,
					diagnostic
				) != Status::Ok)
				return nullptr;
			return std::make_shared<engine::audio::SampleBuffer>(
				engine::audio::AudioFormat{static_cast<uint32_t>(source.Data.SampleRate), 1}, samples
			);
		}
	};
	ImageGraphWavPreview::ImageGraphWavPreview(DeviceFactory factory) : Audio(std::make_unique<Owner>()) {
		Audio->Factory = factory ? std::move(factory) : [](const engine::audio::DeviceSettings &settings) {
			return engine::audio::OpenDevice(settings);
		};
	}
	ImageGraphWavPreview::~ImageGraphWavPreview() {
		Close();
	}
	void ImageGraphWavPreview::Close() {
		if (Audio->Device) {
			Audio->Device->Close();
			Audio->Device.reset();
		}
		Audio->Voices.clear();
		Audio->Sounds.clear();
		Audio->Failed = false;
	}
	bool ImageGraphWavPreview::Enabled() const {
		return Audio->Device != nullptr;
	}
	bool ImageGraphWavPreview::Enable(Diagnostic &diagnostic) {
		diagnostic = {};
		if (Audio->Failed)
			return Fail(
				diagnostic,
				Status::UnsupportedExecution,
				"audio preview must be closed after an ownership failure"
			);
		if (Audio->Device) return true;
		engine::audio::DeviceSettings settings;
		settings.PlaybackEvents = true;
		Audio->Device = Audio->Factory(settings);
		if (!Audio->Device || !Audio->Device->Running()) {
			Audio->Device.reset();
			return Fail(diagnostic, Status::UnsupportedExecution, "audio output is unavailable");
		}
		Audio->DroppedEvents = Audio->Device->Mixer().PlaybackEventsDropped();
		return true;
	}
	bool ImageGraphWavPreview::Update(
		const Document &document,
		std::span<const AudioClipSource> sources,
		std::span<const AudioCaptureFrame> captures,
		const ImageGraphPlayback &playback,
		uint64_t documentRevision,
		uint64_t inputRevision,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		if (!Audio->Device) return true;
		if (Audio->Failed)
			return Fail(
				diagnostic,
				Status::UnsupportedExecution,
				"audio preview must be closed after an ownership failure"
			);
		if (!Audio->Poll(diagnostic)) return false;
		const bool removed =
			std::any_of(Audio->Voices.begin(), Audio->Voices.end(), [&](const Owner::Voice &voice) {
				return std::none_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == voice.NodeId && node.Type == "pc.wav_file_read";
				});
			});
		if (removed && !Audio->Retire(diagnostic)) return false;
		for (const auto &node : document.Nodes) {
			if (node.Type != "pc.wav_file_read") continue;
			auto found =
				std::find_if(Audio->Voices.begin(), Audio->Voices.end(), [&](const Owner::Voice &voice) {
					return voice.NodeId == node.Id;
				});
			const bool fresh = found == Audio->Voices.end();
			if (fresh) {
				if (Audio->Voices.size() >= engine::audio::AudioGraph::MAXIMUM_NODES - 1)
					return Fail(diagnostic, Status::LimitExceeded, "too many WAV preview voices");
				Audio->Voices.push_back({});
				found = std::prev(Audio->Voices.end());
				found->NodeId = node.Id;
			}
			auto &voice = *found;
			if (fresh || voice.DocumentRevision != documentRevision || voice.InputRevision != inputRevision ||
				voice.Tick != playback.CurrentTick || voice.Subframe != playback.Subframe ||
				voice.NegativeFrame != playback.NegativeFrame) {
				EvaluationRequest request;
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(playback));
				request.AudioClips = sources;
				request.AudioFrames = captures;
				request.MaximumImageDimension = IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
				WavPreviewControls candidate;
				if (ResolveWavPreviewControls(document, node.Id, request, candidate, diagnostic) !=
					Status::Ok)
					return false;
				if (voice.Player.IsValid() && candidate.SourceId != voice.Controls.SourceId) {
					if (!Audio->Retire(diagnostic)) return false;
					return Update(
						document, sources, captures, playback, documentRevision, inputRevision, diagnostic
					);
				}
				voice.Controls = std::move(candidate);
				voice.DocumentRevision = documentRevision;
				voice.InputRevision = inputRevision;
				voice.Tick = playback.CurrentTick;
				voice.Subframe = playback.Subframe;
				voice.NegativeFrame = playback.NegativeFrame;
			}
			if (voice.Pending) continue;
			const double frame =
				static_cast<double>(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(playback)));
			const auto selectedFrame = GetImageGraphFrame(playback);
			if (voice.LastStartFrame != selectedFrame) voice.LastStartFrame.reset();
			WavPreviewTimeline timeline{
				playback.Playing, frame, playback.StartTick, playback.FramesPerSecond
			};
			if (voice.Playing && voice.LastStartFrame == selectedFrame && playback.Playing &&
				voice.Controls.Play)
				continue;
			WavPreviewIntent intent;
			if (MakeWavPreviewIntent(
					voice.Controls, timeline, {voice.Known, voice.Playing}, intent, diagnostic
				) != Status::Ok)
				return false;
			if (!intent.Start && !intent.Stop) continue;
			if (!voice.Player.IsValid() && !intent.Start) continue;
			auto &queue = Audio->Device->Mixer().Commands();
			if (queue.Free() < (intent.Start ? (voice.Player.IsValid() ? 6 : 8) : 1))
				return Fail(diagnostic, Status::LimitExceeded, "audio preview command queue is full");
			engine::audio::SoundRef sound;
			if (intent.Start) {
				const auto source =
					std::find_if(sources.begin(), sources.end(), [&](const AudioClipSource &item) {
						return item.SourceId == voice.Controls.SourceId;
					});
				if (source == sources.end())
					return Fail(diagnostic, Status::InvalidValue, "WAV source is not loaded");
				sound = Audio->Prepare(*source, ClipBytes(sources), diagnostic);
				if (!sound) return false;
				if (Audio->NextGeneration == std::numeric_limits<uint64_t>::max())
					return Fail(
						diagnostic, Status::LimitExceeded, "audio preview generation budget exhausted"
					);
			}
			engine::audio::Command command;
			command.AtSample = Audio->Device->Rendered();
			const auto post = [&](engine::audio::CommandKind kind) {
				command.Kind = kind;
				if (!queue.Post(command)) {
					Audio->Failed = true;
					return false;
				}
				return true;
			};
			if (!voice.Player.IsValid()) {
				voice.Player = queue.Allocate();
				command.Target = voice.Player;
				command.Node = engine::audio::NodeKind::Player;
				if (!post(engine::audio::CommandKind::AddNode)) return false;
				command.Second = {engine::audio::AudioGraph::OUTPUT_ID};
				if (!post(engine::audio::CommandKind::Connect)) return false;
			}
			command.Target = voice.Player;
			command.PlaybackGeneration = voice.Generation;
			if (!post(engine::audio::CommandKind::Stop)) return false;
			if (intent.Start) {
				voice.Generation = Audio->NextGeneration++;
				command.PlaybackGeneration = voice.Generation;
				command.Sound = sound;
				if (!post(engine::audio::CommandKind::SetSound)) return false;
				command.Sound.reset();
				command.CursorFrames = intent.CursorFrames;
				if (!post(engine::audio::CommandKind::Seek)) return false;
				command.Value = static_cast<float>(intent.Gain);
				if (!post(engine::audio::CommandKind::SetGain)) return false;
				command.Flag = false;
				if (!post(engine::audio::CommandKind::SetLooping) || !post(engine::audio::CommandKind::Play))
					return false;
				voice.LastStartFrame = selectedFrame;
				if (std::none_of(
						Audio->Sounds.begin(), Audio->Sounds.end(), [&](const Owner::Prepared &item) {
							return item.SourceId == voice.Controls.SourceId;
						}
					))
					Audio->Sounds.push_back({voice.Controls.SourceId, std::move(sound)});
			}
			voice.Pending = true;
		}
		return true;
	}
	bool ImageGraphWavPreview::LoadSource(
		std::vector<AudioClipSource> &sources,
		ImageGraphPreviewCache &cache,
		std::string_view sourceId,
		const std::filesystem::path &path,
		Diagnostic &diagnostic
	) {
		const auto soundBytes = Audio->SoundBytes();
		AudioClipSource candidate;
		if (soundBytes > Limits::MaximumEvaluationBytes ||
			!ReadImageGraphWavSource(
				sources, sourceId, path, candidate, diagnostic, Limits::MaximumEvaluationBytes - soundBytes
			))
			return false;
		engine::audio::SoundRef prepared;
		if (Audio->Device &&
			(!candidate.Data.Samples.empty() ||
			 (!candidate.Data.Channels.empty() && !candidate.Data.Channels.front().empty()))) {
			prepared = Audio->Prepare(
				candidate, ClipBytes(sources) + ClipBytes(std::span(&candidate, 1)), diagnostic, false
			);
			if (!prepared) return false;
		}
		// The caller may borrow its name from the source being replaced.
		Owner::Prepared replacement;
		if (prepared) replacement = {candidate.SourceId, std::move(prepared)};
		if (!Audio->Retire(diagnostic)) return false;
		const auto found = std::find_if(sources.begin(), sources.end(), [&](const AudioClipSource &source) {
			return source.SourceId == sourceId;
		});
		if (found == sources.end())
			sources.push_back(std::move(candidate));
		else
			*found = std::move(candidate);
		if (replacement.Sound) Audio->Sounds.push_back(std::move(replacement));
		cache.Clear();
		return true;
	}
	bool ImageGraphWavPreview::RemoveSource(
		std::vector<AudioClipSource> &sources,
		ImageGraphPreviewCache &cache,
		std::string_view sourceId,
		Diagnostic &diagnostic
	) {
		if (!Audio->Retire(diagnostic)) return false;
		return RemoveImageGraphWavSource(sources, cache, sourceId);
	}
}
