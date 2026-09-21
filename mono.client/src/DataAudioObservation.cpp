#include <engine/net/quic/Crypto.hpp>

#include <algorithm>
#include <bit>
#include <client/DataAudioObservation.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

namespace client {
	namespace {
		constexpr size_t MAXIMUM_FRAMES = engine::audio::DEFAULT_BLOCK_FRAMES;
		constexpr size_t MAXIMUM_WAVEFORM_BYTES = MAXIMUM_FRAMES * 8 * sizeof(float);
		constexpr size_t MAXIMUM_ARTIFACTS = 8;
		constexpr size_t MAXIMUM_RETAINED_BYTES = MAXIMUM_ARTIFACTS * MAXIMUM_WAVEFORM_BYTES;

		uint64_t MultiplyModulo(uint64_t left, uint64_t right, uint64_t modulus) {
			left %= modulus;
			uint64_t result = 0;
			while (right != 0) {
				if ((right & 1u) != 0)
					result = result >= modulus - left ? result - (modulus - left) : result + left;
				right >>= 1u;
				if (right != 0) left = left >= modulus - left ? left - (modulus - left) : left + left;
			}
			return result;
		}

		std::string Hex(std::span<const std::byte> bytes) {
			static constexpr char DIGITS[] = "0123456789abcdef";
			const auto digest = engine::net::quic::Digest(bytes);
			std::string text;
			text.reserve(digest.size() * 2);
			for (const std::byte value : digest) {
				const unsigned byte = std::to_integer<unsigned char>(value);
				text.push_back(DIGITS[byte >> 4]);
				text.push_back(DIGITS[byte & 15]);
			}
			return text;
		}

		std::vector<std::byte> FloatBytes(std::span<const float> samples) {
			static_assert(sizeof(float) == sizeof(uint32_t));
			std::vector<std::byte> bytes(samples.size() * sizeof(uint32_t));
			for (size_t index = 0; index < samples.size(); ++index) {
				const uint32_t bits = std::bit_cast<uint32_t>(samples[index]);
				for (size_t byte = 0; byte < sizeof(bits); ++byte)
					bytes[index * sizeof(bits) + byte] = static_cast<std::byte>((bits >> (byte * 8)) & 0xffu);
			}
			return bytes;
		}

		std::string ObservationId(
			const engine::world::DataFactoryReply &clock, const engine::audio::AudioObservation &audio
		) {
			return "audio/" + clock.InstanceId + "/" + std::to_string(clock.WorldEpoch) + "/" +
				   std::to_string(clock.WorldVersion) + "/" + std::to_string(clock.Clock.Tick) + "/" +
				   std::to_string(audio.BeginSample) + "-" + std::to_string(audio.EndSample);
		}

		bool ValidClock(const engine::world::DataFactoryReply &clock, std::string &detail) {
			if (clock.Status != engine::world::DataFactoryStatus::Ok || clock.InstanceId.empty()) {
				detail = "data-factory lifecycle clock is unavailable";
				return false;
			}
			if (!clock.Clock.RationalTimeAvailable || clock.Clock.Interval.NumeratorNanoseconds == 0 ||
				clock.Clock.Interval.Denominator == 0) {
				detail = "data-factory world has no exact rational tick interval";
				return false;
			}
			return true;
		}
	}

	std::string AnswerDataFactoryControlLine(
		engine::world::DataFactorySession &session,
		std::string_view instanceId,
		const std::function<std::string()> &answer,
		const std::function<void(const engine::world::DataFactoryReply &)> &observe
	) {
		const engine::world::DataFactoryReply before = session.Inspect(instanceId);
		std::string reply = answer();
		if (before.Status != engine::world::DataFactoryStatus::Ok) return reply;

		const engine::world::DataFactoryReply after = session.Inspect(before.InstanceId);
		if (after.Status == engine::world::DataFactoryStatus::Ok &&
			(after.Clock.Tick != before.Clock.Tick || after.WorldEpoch != before.WorldEpoch))
			observe(after);
		return reply;
	}

	DataAudioObservationHost::DataAudioObservationHost(Inspect inspect) : InspectClock(std::move(inspect)) {}

	engine::script::DataAudioObservationBridgeCapabilities DataAudioObservationHost::Capabilities() const {
		return {
			.Available = true,
			.Detail = "null-device deterministic retained capture",
			.MaximumFrames = MAXIMUM_FRAMES,
		};
	}

	bool DataAudioObservationHost::Publish(
		const engine::world::DataFactoryReply &clock,
		const engine::audio::AudioMixer &mixer,
		const engine::audio::SampleBuffer &waveform,
		const engine::audio::MixReport &report,
		const std::vector<AudioObservationSourceBinding> &sources,
		std::string &detail,
		bool complete
	) {
		if (!ValidClock(clock, detail)) return false;
		std::vector<engine::audio::AudioSourceBinding> bindings;
		bindings.reserve(sources.size());
		for (const auto &source : sources)
			bindings.push_back({source.SourceId, source.Player});
		const auto captured = engine::audio::CaptureAudioObservation(
			mixer, waveform, report, bindings, {.MaximumFrames = MAXIMUM_FRAMES}
		);
		if (captured.Status != engine::audio::AudioObservationStatus::Ready || !captured.Value) {
			detail = "audio capture refused the completed null-device block";
			return false;
		}
		const engine::audio::AudioObservation &audio = *captured.Value;
		const std::vector<std::byte> bytes = FloatBytes(audio.Waveform);
		if (bytes.empty() || bytes.size() > MAXIMUM_WAVEFORM_BYTES) {
			detail = "audio waveform exceeds the retained capture bound";
			return false;
		}
		const std::string observationId = ObservationId(clock, audio);
		if (!engine::script::IsDataAudioObservationText(
				observationId, engine::script::MAX_AUDIO_OBSERVATION_ID_BYTES
			)) {
			detail = "audio observation identifier exceeds its bound";
			return false;
		}

		engine::script::DataAudioObservation observation;
		observation.ObservationId = observationId;
		observation.World = {
			.Tick = clock.Clock.Tick,
			.TimeNanoseconds = clock.Clock.TimeNanoseconds,
			.Epoch = clock.WorldEpoch,
			.Version = std::to_string(clock.WorldVersion),
			.TickInterval = {clock.Clock.Interval.NumeratorNanoseconds, clock.Clock.Interval.Denominator},
		};
		observation.SampleBegin = audio.BeginSample;
		observation.SampleEnd = audio.EndSample;
		observation.SampleRateHz = audio.SampleRate;
		observation.Channels = audio.Channels;
		observation.ChannelLayout = audio.ChannelLayout;
		const std::string digest = Hex(bytes);
		observation.Waveform = {
			.Available = true,
			.UnavailableReason = {},
			.ResourceId = observationId + "/waveform",
			.Sha256 = digest,
			.ByteLength = bytes.size(),
			.Chunks = {{digest, 0, bytes.size()}},
		};
		const bool incompleteSources =
			std::any_of(audio.Sources.begin(), audio.Sources.end(), [](const auto &source) {
				return !source.Present;
			});
		if (!incompleteSources) {
			for (const auto &source : audio.Sources) {
				observation.Sources.push_back(
					{source.SourceId,
					 source.Kind,
					 source.Gain,
					 source.Pan,
					 source.Muted,
					 source.Playing,
					 source.Looping,
					 source.Present}
				);
			}
		} else {
			observation.Missing.push_back(
				{"source_state", "a retained voice ended before this completed mixer block"}
			);
		}
		bool omittedEvents = false;
		for (const auto &event : audio.Events) {
			if (incompleteSources) {
				omittedEvents = true;
				break;
			}
			// Commands for intermediate graph nodes have no authored source id.
			// Do not invent one from a node number for a durable data record.
			if (event.SourceId.empty()) {
				omittedEvents = true;
				continue;
			}
			observation.Events.push_back(
				{observationId + "/event/" + std::to_string(observation.Events.size()),
				 event.Kind,
				 event.SourceId,
				 event.RelatedSourceId,
				 event.RequestedSample,
				 event.AppliedSample,
				 event.OffsetFrames,
				 event.Timing,
				 event.Kind == "playback_finished" ? "natural" : ""}
			);
		}
		if (omittedEvents) {
			// The record contract represents event completeness as all or missing.
			// Retaining only the named subset would look complete to a trainer.
			observation.Events.clear();
			observation.Missing.push_back(
				{"events", "unidentified internal graph command was omitted from the durable record"}
			);
		}
		if (!engine::script::ValidateDataAudioObservation(observation, detail)) return false;
		std::scoped_lock lock(Mutex);
		SceneRecords &scene = Records[clock.InstanceId];
		if (scene.Epoch != clock.WorldEpoch || scene.Version != clock.WorldVersion) {
			scene = {
				.Epoch = clock.WorldEpoch,
				.Version = clock.WorldVersion,
				.Latest = {},
				.Artifacts = {},
				.Bytes = 0,
				.Pending = {},
				.PendingWaveform = {},
			};
		}
		scene.Pending.push_back(std::move(observation));
		scene.PendingWaveform.insert(scene.PendingWaveform.end(), bytes.begin(), bytes.end());
		if (scene.Pending.size() > MAXIMUM_ARTIFACTS ||
			scene.PendingWaveform.size() > MAXIMUM_RETAINED_BYTES) {
			scene.Pending.clear();
			scene.PendingWaveform.clear();
			detail = "audio tick exceeds the bounded pending capture budget";
			return false;
		}
		if (!complete) {
			detail.clear();
			return true;
		}
		if (scene.Pending.empty() || scene.PendingWaveform.empty()) {
			detail = "audio tick has no completed waveform";
			return false;
		}
		engine::script::DataAudioObservation combined = scene.Pending.front();
		const auto &last = scene.Pending.back();
		combined.SampleEnd = last.SampleEnd;
		combined.Sources = last.Sources;
		combined.Events.clear();
		combined.Missing.clear();
		for (const auto &piece : scene.Pending) {
			for (const auto &event : piece.Events) {
				auto joined = event;
				joined.EventId = combined.ObservationId + "/event/" + std::to_string(combined.Events.size());
				joined.SampleOffsetFrames = joined.AppliedSample - combined.SampleBegin;
				combined.Events.push_back(std::move(joined));
			}
			for (const auto &missing : piece.Missing) {
				if (std::find_if(combined.Missing.begin(), combined.Missing.end(), [&](const auto &existing) {
						return existing.Field == missing.Field;
					}) == combined.Missing.end())
					combined.Missing.push_back(missing);
			}
		}
		if (std::any_of(combined.Missing.begin(), combined.Missing.end(), [](const auto &missing) {
				return missing.Field == "events";
			}))
			combined.Events.clear();
		const std::string combinedDigest = Hex(scene.PendingWaveform);
		combined.Waveform = {
			.Available = true,
			.UnavailableReason = {},
			.ResourceId = combined.ObservationId + "/waveform",
			.Sha256 = combinedDigest,
			.ByteLength = scene.PendingWaveform.size(),
			.Chunks = {{combinedDigest, 0, scene.PendingWaveform.size()}},
		};
		if (!engine::script::ValidateDataAudioObservation(combined, detail)) return false;
		scene.Latest = combined;
		scene.Artifacts.push_back({
			.Epoch = clock.WorldEpoch,
			.Version = clock.WorldVersion,
			.Observation = std::move(combined),
			.Waveform = std::move(scene.PendingWaveform),
		});
		scene.Pending.clear();
		scene.Bytes += scene.Artifacts.back().Waveform.size();
		while (scene.Artifacts.size() > MAXIMUM_ARTIFACTS || scene.Bytes > MAXIMUM_RETAINED_BYTES) {
			scene.Bytes -= scene.Artifacts.front().Waveform.size();
			scene.Artifacts.pop_front();
		}
		detail.clear();
		return true;
	}

	bool DataAudioObservationHost::Capture(
		std::string_view instanceId,
		engine::script::DataAudioObservationBridgeResult &result,
		std::string &detail
	) {
		const engine::world::DataFactoryReply clock =
			InspectClock ? InspectClock(instanceId) : engine::world::DataFactoryReply{};
		std::scoped_lock lock(Mutex);
		const auto found = Records.find(std::string(instanceId));
		const bool stale = InspectClock && (!ValidClock(clock, detail) || found == Records.end() ||
											found->second.Epoch != clock.WorldEpoch ||
											found->second.Version != clock.WorldVersion);
		if (found == Records.end() || stale) {
			if (found != Records.end()) Records.erase(found);
			result = {
				.Status = "unavailable",
				.Detail = "no completed audio block for this scene",
				.Observation = {},
			};
			detail.clear();
			return true;
		}
		result = {.Status = "ok", .Detail = {}, .Observation = found->second.Latest};
		detail.clear();
		return true;
	}

	bool DataAudioObservationHost::ReadWaveform(
		std::string_view instanceId,
		std::string_view resourceId,
		std::string_view sha256,
		uint64_t byteBegin,
		size_t maximumBytes,
		std::vector<std::byte> &bytes,
		std::string &detail
	) {
		const engine::world::DataFactoryReply clock =
			InspectClock ? InspectClock(instanceId) : engine::world::DataFactoryReply{};
		std::scoped_lock lock(Mutex);
		const auto found = Records.find(std::string(instanceId));
		const bool stale = InspectClock && (!ValidClock(clock, detail) || found == Records.end() ||
											found->second.Epoch != clock.WorldEpoch ||
											found->second.Version != clock.WorldVersion);
		if (found == Records.end() || stale) {
			if (found != Records.end()) Records.erase(found);
			detail = "waveform resource is stale or unknown";
			return false;
		}
		const auto artifact = std::find_if(
			found->second.Artifacts.begin(), found->second.Artifacts.end(), [&](const Retained &candidate) {
				return candidate.Observation.Waveform.ResourceId == resourceId &&
					   candidate.Observation.Waveform.Sha256 == sha256;
			}
		);
		if (artifact == found->second.Artifacts.end()) {
			detail = "waveform resource is stale or unknown";
			return false;
		}
		if (byteBegin > artifact->Waveform.size() || maximumBytes > artifact->Waveform.size() - byteBegin) {
			detail = "waveform range is outside the retained resource";
			return false;
		}
		bytes.assign(
			artifact->Waveform.begin() + static_cast<ptrdiff_t>(byteBegin),
			artifact->Waveform.begin() + static_cast<ptrdiff_t>(byteBegin + maximumBytes)
		);
		detail.clear();
		return true;
	}

	void DataAudioObservationHost::Clear(std::string_view instanceId) {
		std::scoped_lock lock(Mutex);
		Records.erase(std::string(instanceId));
	}

	void DataAudioObservationHost::InvalidateUnless(const engine::world::DataFactoryReply &clock) {
		std::scoped_lock lock(Mutex);
		const auto found = Records.find(clock.InstanceId);
		if (found != Records.end() &&
			(found->second.Epoch != clock.WorldEpoch || found->second.Version != clock.WorldVersion))
			Records.erase(found);
	}

	void DataAudioObservationHost::ResetTickClock(
		const engine::world::DataFactoryReply &clock, uint32_t sampleRate
	) {
		std::string detail;
		if (!ValidClock(clock, detail) || sampleRate == 0 ||
			clock.Clock.Interval.Denominator > UINT64_MAX / 1'000'000'000ull ||
			sampleRate > UINT64_MAX / clock.Clock.Interval.NumeratorNanoseconds) {
			TickClockReady = false;
			return;
		}
		const uint64_t denominator =
			static_cast<uint64_t>(clock.Clock.Interval.Denominator) * 1'000'000'000ull;
		const uint64_t numerator =
			static_cast<uint64_t>(sampleRate) * clock.Clock.Interval.NumeratorNanoseconds;
		TickClockReady = true;
		TickEpoch = clock.WorldEpoch;
		Tick = clock.Clock.Tick;
		TickRemainder = MultiplyModulo(Tick, numerator, denominator);
		// A reset is a discontinuity. No mixer state survives it, so an older
		// waveform must not remain observable until a newly completed tick arrives.
		Clear(clock.InstanceId);
	}

	std::vector<size_t> DataAudioObservationHost::AdvanceFrames(
		const engine::world::DataFactoryReply &clock, uint32_t sampleRate
	) {
		std::vector<size_t> frames;
		std::string detail;
		if (!ValidClock(clock, detail) || sampleRate == 0) return frames;
		if (!TickClockReady || TickEpoch != clock.WorldEpoch || clock.Clock.Tick < Tick) {
			// A restored clock has no mixer history in this client. Taking its current
			// boundary avoids fabricating every tick that existed before the restore.
			ResetTickClock(clock, sampleRate);
			return frames;
		}
		const uint64_t denominator =
			static_cast<uint64_t>(clock.Clock.Interval.Denominator) * 1'000'000'000ull;
		if (denominator == 0 || sampleRate > UINT64_MAX / clock.Clock.Interval.NumeratorNanoseconds)
			return frames;
		const uint64_t numerator =
			static_cast<uint64_t>(sampleRate) * clock.Clock.Interval.NumeratorNanoseconds;
		while (Tick < clock.Clock.Tick) {
			if (TickRemainder > UINT64_MAX - numerator) return {};
			const uint64_t total = TickRemainder + numerator;
			const uint64_t count = total / denominator;
			TickRemainder = total % denominator;
			Tick++;
			if (count > 0 && count <= MAXIMUM_FRAMES)
				frames.push_back(static_cast<size_t>(count));
			else if (count > MAXIMUM_FRAMES) {
				uint64_t remaining = count;
				while (remaining > 0) {
					const size_t slice = static_cast<size_t>(std::min<uint64_t>(remaining, MAXIMUM_FRAMES));
					frames.push_back(slice);
					remaining -= slice;
				}
			}
		}
		return frames;
	}
}
