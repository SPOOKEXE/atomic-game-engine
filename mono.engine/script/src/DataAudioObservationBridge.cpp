#include <engine/script/DataAudioObservationBridge.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <unordered_set>

namespace engine::script {
	namespace {
		bool Utf8(std::string_view value) {
			for (size_t offset = 0; offset < value.size();) {
				const uint8_t first = static_cast<uint8_t>(value[offset]);
				if (first < 0x80) {
					offset++;
					continue;
				}
				const size_t count = first >= 0xC2 && first <= 0xDF	  ? 2
									 : first >= 0xE0 && first <= 0xEF ? 3
									 : first >= 0xF0 && first <= 0xF4 ? 4
																	  : 0;
				if (count == 0 || offset + count > value.size()) return false;
				for (size_t index = 1; index < count; index++)
					if ((static_cast<uint8_t>(value[offset + index]) & 0xC0) != 0x80) return false;
				const uint32_t codepoint =
					count == 2 ? (first & 0x1F) << 6 | (static_cast<uint8_t>(value[offset + 1]) & 0x3F)
					: count == 3
						? (first & 0x0F) << 12 | (static_cast<uint8_t>(value[offset + 1]) & 0x3F) << 6 |
							  (static_cast<uint8_t>(value[offset + 2]) & 0x3F)
						: (first & 0x07) << 18 | (static_cast<uint8_t>(value[offset + 1]) & 0x3F) << 12 |
							  (static_cast<uint8_t>(value[offset + 2]) & 0x3F) << 6 |
							  (static_cast<uint8_t>(value[offset + 3]) & 0x3F);
				if ((count == 3 && codepoint < 0x800) || (count == 4 && codepoint < 0x10000) ||
					codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
					return false;
				offset += count;
			}
			return true;
		}

		bool Text(std::string_view value, size_t limit) {
			return !value.empty() && value.size() <= limit && value.find('\0') == std::string_view::npos &&
				   Utf8(value);
		}

		bool Digest(std::string_view value) {
			return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
					   return (character >= '0' && character <= '9') ||
							  (character >= 'a' && character <= 'f');
				   });
		}

		bool MissingField(std::string_view field) {
			return field == "waveform" || field == "source_state" || field == "events";
		}

		struct Preflight {
			size_t Nodes = 0;
			size_t Strings = 0;

			bool Node() {
				return Nodes++ < 100'000;
			}
			bool Text(std::string_view value) {
				return Node() && value.size() <= MAX_AUDIO_OBSERVATION_TOTAL_STRING_BYTES - Strings &&
					   (Strings += value.size(), true);
			}
			bool Scalar() {
				return Node();
			}
			bool Object(std::initializer_list<std::string_view> fields) {
				if (!Node() || fields.size() > 64) return false;
				for (const std::string_view field : fields)
					if (!Text(field)) return false;
				return true;
			}
			bool Array(size_t count) {
				return Node() && count <= MAX_AUDIO_OBSERVATION_EVENTS;
			}
		};

		bool PreflightObservation(const DataAudioObservation &observation) {
			Preflight preflight;
			auto scalar = [&] { return preflight.Scalar(); };
			auto text = [&](std::string_view value) { return preflight.Text(value); };
			if (!preflight.Object(
					{"schema",
					 "observation_id",
					 "world",
					 "sample_begin",
					 "sample_end",
					 "sample_rate_hz",
					 "channels",
					 "channel_layout",
					 "sample_type",
					 "interleaved",
					 "waveform",
					 "sources",
					 "events",
					 "missing"}
				) ||
				!text(AUDIO_OBSERVATION_SCHEMA) || !text(observation.ObservationId) ||
				!preflight.Object({"tick", "time_ns", "epoch", "version", "tick_interval"}) || !scalar() ||
				!scalar() || !scalar() || !text(observation.World.Version) ||
				!preflight.Object({"numerator_ns", "denominator"}) || !scalar() || !scalar() || !scalar() ||
				!scalar() || !scalar() || !scalar() || !text(observation.ChannelLayout) || !text("float32") ||
				!scalar() ||
				!preflight.Object(
					{"available", "unavailable_reason", "resource_id", "sha256", "byte_length", "chunks"}
				) ||
				!scalar() ||
				!(observation.Waveform.Available ? scalar() : text(observation.Waveform.UnavailableReason)) ||
				!(observation.Waveform.Available ? text(observation.Waveform.ResourceId) : scalar()) ||
				!(observation.Waveform.Available ? text(observation.Waveform.Sha256) : scalar()) ||
				!scalar() || !preflight.Array(observation.Waveform.Chunks.size()))
				return false;
			for (const auto &chunk : observation.Waveform.Chunks)
				if (!preflight.Object({"sha256", "byte_begin", "byte_end"}) || !text(chunk.Sha256) ||
					!scalar() || !scalar())
					return false;
			if (!preflight.Array(observation.Sources.size())) return false;
			for (const auto &source : observation.Sources)
				if (!preflight.Object(
						{"source_id", "kind", "gain", "pan", "muted", "playing", "looping", "present"}
					) ||
					!text(source.SourceId) || !text(source.Kind) || !scalar() || !scalar() || !scalar() ||
					!scalar() || !scalar() || !scalar())
					return false;
			if (!preflight.Array(observation.Events.size())) return false;
			for (const auto &event : observation.Events)
				if (!preflight.Object(
						{"event_id",
						 "kind",
						 "source_id",
						 "related_source_id",
						 "requested_sample",
						 "applied_sample",
						 "sample_offset_frames",
						 "timing",
						 "finish_provenance"}
					) ||
					!text(event.EventId) || !text(event.Kind) || !text(event.SourceId) ||
					!(event.RelatedSourceId.empty() ? scalar() : text(event.RelatedSourceId)) || !scalar() ||
					!scalar() || !scalar() || !text(event.Timing) ||
					!(event.FinishProvenance.empty() ? scalar() : text(event.FinishProvenance)))
					return false;
			if (!preflight.Array(observation.Missing.size())) return false;
			for (const auto &missing : observation.Missing)
				if (!preflight.Object({"field", "reason"}) || !text(missing.Field) || !text(missing.Reason))
					return false;
			return true;
		}
	}

	bool ValidateDataAudioObservation(const DataAudioObservation &observation, std::string &failure) {
		auto refuse = [&failure](std::string_view detail) {
			failure = std::string(detail);
			return false;
		};
		auto id = [](std::string_view value) { return Text(value, MAX_AUDIO_OBSERVATION_ID_BYTES); };
		auto checkedText = [](std::string_view value, size_t limit) { return Text(value, limit); };

		if (!id(observation.ObservationId)) return refuse("invalid observation_id");
		if (!checkedText(observation.World.Version, MAX_AUDIO_OBSERVATION_STRING_BYTES) ||
			observation.World.TickInterval.NumeratorNanoseconds == 0 ||
			observation.World.TickInterval.Denominator == 0)
			return refuse("invalid world clock");
		if (observation.SampleEnd <= observation.SampleBegin || observation.SampleRateHz == 0 ||
			observation.Channels == 0)
			return refuse("invalid sample shape");
		if ((observation.Channels == 1 && observation.ChannelLayout != "mono") ||
			(observation.Channels == 2 && observation.ChannelLayout != "stereo") ||
			(observation.Channels > 2 && observation.ChannelLayout != "discrete"))
			return refuse("channel layout does not match channel count");
		if (observation.Sources.size() > MAX_AUDIO_OBSERVATION_SOURCES ||
			observation.Events.size() > MAX_AUDIO_OBSERVATION_EVENTS ||
			observation.Missing.size() > MAX_AUDIO_OBSERVATION_MISSING ||
			observation.Waveform.Chunks.size() > MAX_AUDIO_OBSERVATION_CHUNKS)
			return refuse("audio observation exceeds a record bound");

		const auto &waveform = observation.Waveform;
		if (waveform.Available) {
			if (!waveform.UnavailableReason.empty() || !id(waveform.ResourceId) || !Digest(waveform.Sha256) ||
				waveform.Chunks.empty())
				return refuse("invalid available waveform");
			uint64_t cursor = 0;
			for (const auto &chunk : waveform.Chunks) {
				if (!Digest(chunk.Sha256) || chunk.ByteEnd <= chunk.ByteBegin || chunk.ByteBegin != cursor)
					return refuse("invalid waveform chunks");
				cursor = chunk.ByteEnd;
			}
			if (cursor != waveform.ByteLength ||
				observation.SampleEnd - observation.SampleBegin > UINT64_MAX / observation.Channels / 4 ||
				waveform.ByteLength !=
					(observation.SampleEnd - observation.SampleBegin) * observation.Channels * 4)
				return refuse("waveform byte length does not match sample shape");
		} else if (!checkedText(waveform.UnavailableReason, MAX_AUDIO_OBSERVATION_STRING_BYTES) ||
				   !waveform.ResourceId.empty() || !waveform.Sha256.empty() || waveform.ByteLength != 0 ||
				   !waveform.Chunks.empty()) {
			return refuse("invalid unavailable waveform");
		}

		std::unordered_set<std::string_view> sources;
		for (const auto &source : observation.Sources) {
			if (!id(source.SourceId) || !checkedText(source.SourceId, MAX_AUDIO_OBSERVATION_ID_BYTES) ||
				!checkedText(source.Kind, MAX_AUDIO_OBSERVATION_STRING_BYTES) ||
				!std::isfinite(source.Gain) || !std::isfinite(source.Pan) ||
				!sources.emplace(source.SourceId).second)
				return refuse("invalid audio source");
		}
		std::unordered_set<std::string_view> events;
		for (const auto &event : observation.Events) {
			const bool finished = event.Kind == "playback_finished";
			const char *timing = event.AppliedSample < event.RequestedSample
									 ? "early"
									 : (event.AppliedSample > event.RequestedSample ? "late" : "exact");
			if (!id(event.EventId) || !id(event.SourceId) ||
				!checkedText(event.EventId, MAX_AUDIO_OBSERVATION_ID_BYTES) ||
				!checkedText(event.Kind, MAX_AUDIO_OBSERVATION_STRING_BYTES) ||
				!checkedText(event.SourceId, MAX_AUDIO_OBSERVATION_ID_BYTES) ||
				(!event.RelatedSourceId.empty() &&
				 (!id(event.RelatedSourceId) ||
				  !checkedText(event.RelatedSourceId, MAX_AUDIO_OBSERVATION_ID_BYTES))) ||
				!checkedText(event.Timing, MAX_AUDIO_OBSERVATION_STRING_BYTES) || event.Timing != timing ||
				(finished != !event.FinishProvenance.empty()) ||
				(!event.FinishProvenance.empty() &&
				 (!checkedText(event.FinishProvenance, MAX_AUDIO_OBSERVATION_STRING_BYTES) ||
				  (event.FinishProvenance != "natural" && event.FinishProvenance != "explicit"))) ||
				!events.emplace(event.EventId).second || !sources.contains(event.SourceId) ||
				(!event.RelatedSourceId.empty() && !sources.contains(event.RelatedSourceId)) ||
				event.AppliedSample < observation.SampleBegin ||
				event.AppliedSample >= observation.SampleEnd ||
				event.SampleOffsetFrames != event.AppliedSample - observation.SampleBegin)
				return refuse("invalid audio event");
		}
		std::unordered_set<std::string_view> missing;
		for (const auto &item : observation.Missing) {
			if (!MissingField(item.Field) || !checkedText(item.Reason, MAX_AUDIO_OBSERVATION_STRING_BYTES) ||
				!missing.emplace(item.Field).second)
				return refuse("invalid missingness record");
		}
		if (waveform.Available == missing.contains("waveform") ||
			(!waveform.Available &&
			 waveform.UnavailableReason != std::find_if(
											   observation.Missing.begin(),
											   observation.Missing.end(),
											   [](const auto &item) { return item.Field == "waveform"; }
										   )->Reason) ||
			(missing.contains("source_state") && !observation.Sources.empty()) ||
			(missing.contains("events") && !observation.Events.empty()))
			return refuse("audio observation missingness does not match content");
		if (!PreflightObservation(observation)) return refuse("audio observation exceeds aggregate bounds");
		failure.clear();
		return true;
	}

	bool IsDataAudioObservationText(std::string_view value, size_t limit, bool nonempty) {
		return (!nonempty || !value.empty()) && value.size() <= limit &&
			   value.find('\0') == std::string_view::npos && Utf8(value);
	}

	bool IsDataAudioObservationDigest(std::string_view value) {
		return Digest(value);
	}
}
