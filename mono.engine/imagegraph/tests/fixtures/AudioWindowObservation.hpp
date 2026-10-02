#pragma once
#include <engine/imagegraph/AudioWindowPresentation.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
namespace engine::imagegraph::testing {
	// Fixed native visualization oracle. Source thumbnail raster parity is not asserted.
	struct AudioWindowObservationFixture {
		static constexpr size_t POINTS = 320;
		Document Doc;
		Plan Compiled;
		EvaluationRequest Request;
		std::array<AudioClipSource, 2> Sources;
		Diagnostic Error;
		size_t Packets;
		uint64_t InputHash = 0;
		static void Require(bool ok, const char *message) {
			if (!ok) throw std::runtime_error(message);
		}
		static double Amplitude(size_t index, size_t source) {
			return (double(index % 1024) - 512) / 512 + double(source) / 8;
		}
		static void Word(uint64_t &h, uint64_t w) {
			for (size_t i = 0; i < 8; ++i) {
				h ^= (w >> (i * 8)) & 255;
				h *= 1099511628211ULL;
			}
		}
		explicit AudioWindowObservationFixture(size_t packets) : Packets(packets) {
			Doc.FormatVersion = 9;
			Doc.Timeline = TimelineSettings{1024, 0, 1023, "loop", 128};
			Doc.Nodes = {
				{"path",
				 "pc.string_merge",
				 "",
				 {},
				 {},
				 {{"text_0", ValueType::Text, Value{std::string("clip")}}}},
				{"file", "pc.wav_file_read", "", {}, {{"mono", false}}},
				{"window",
				 "pc.audio_window",
				 "",
				 {},
				 {{"width", int64_t{1024}}, {"step", int64_t{16}}, {"cursor_location", EnumValue{0}}}}
			};
			Doc.Links = {{"path", "text", "file", "path"}, {"file", "data", "window", "audio_data"}};
			Doc.Outputs = {{"samples", "window", "bit_array"}};
			Document persisted;
			Require(Read(Write(Doc), persisted, Error) == Status::Ok, "audio observation persistence");
			Doc = std::move(persisted);
			Require(Compile(Doc, Compiled, Error) == Status::Ok, "audio observation compile");
			for (size_t s = 0; s < 2; ++s) {
				Sources[s].SourceId = "clip";
				Sources[s].Data.SampleRate = 32768;
				Sources[s].Data.Channels.resize(2);
				for (auto &plane : Sources[s].Data.Channels)
					plane.resize(Packets);
				for (size_t i = 0; i < Packets; ++i) {
					Sources[s].Data.Channels[0][i] = Amplitude(i, s);
					Sources[s].Data.Channels[1][i] = -Amplitude(i, s);
				}
			}
			Request.MaximumImageDimension = 128;
			Select(0, {64, .5, false});
			InputHash = HashInputs();
		}
		void Select(size_t source, FrameTime clock) {
			Require(source < 2, "audio observation source index");
			Request.AudioClips = std::span<const AudioClipSource>(&Sources[source], 1);
			Require(SetFrameTime(Request, clock), "audio observation signed clock");
		}
		uint64_t HashInputs() const {
			uint64_t h = 14695981039346656037ULL;
			for (unsigned char c : Write(Doc)) {
				h ^= c;
				h *= 1099511628211ULL;
			}
			Word(h, Packets);
			Word(h, 128);
			Word(h, 1024);
			Word(h, 16);
			Word(h, POINTS);
			Word(h, 128); // Point policy and request image cap.
			for (FrameTime clock :
				 std::array<FrameTime, 4>{{{64, .5, false}, {64, .5, true}, {0, .5, false}, {0, .5, true}}}) {
				Word(h, clock.Tick);
				Word(h, std::bit_cast<uint64_t>(clock.Subframe));
				Word(h, clock.NegativeFrame);
			}
			for (const auto &source : Sources) {
				Word(h, source.SourceId.size());
				for (unsigned char c : source.SourceId)
					Word(h, c);
				Word(h, std::bit_cast<uint64_t>(source.Data.SampleRate));
				Word(h, source.Data.Samples.size());
				for (double v : source.Data.Samples)
					Word(h, std::bit_cast<uint64_t>(v));
				Word(h, source.Data.Channels.size());
				for (const auto &plane : source.Data.Channels) {
					Word(h, plane.size());
					Word(h, plane.capacity());
					for (double v : plane)
						Word(h, std::bit_cast<uint64_t>(v));
				}
			}
			return h;
		}
		void VerifyInputs() const {
			Require(HashInputs() == InputHash, "audio observation immutable inputs changed");
		}
		uint64_t Verify(const AudioWindowPresentation &out, size_t source, FrameTime clock) const {
			Require(
				out.Points.size() == POINTS && out.Channels == 2 && out.Packets == Packets &&
					out.SampleRate == 32768,
				"audio observation shape"
			);
			uint64_t h = 14695981039346656037ULL;
			for (size_t i = 0; i < POINTS; ++i) {
				size_t index = i * (Packets - 1) / (POINTS - 1);
				Require(
					std::bit_cast<uint64_t>(out.Points[i].X) ==
							std::bit_cast<uint64_t>(double(index) / Packets) &&
						std::bit_cast<uint64_t>(out.Points[i].Y) ==
							std::bit_cast<uint64_t>(Amplitude(index, source)),
					"audio observation point words"
				);
				Word(h, std::bit_cast<uint64_t>(out.Points[i].X));
				Word(h, std::bit_cast<uint64_t>(out.Points[i].Y));
			}
			double offset = (double(clock.Tick) + clock.Subframe) * (clock.NegativeFrame ? -256 : 256);
			double start = std::clamp(offset, 0., double(Packets - 1));
			double end = std::clamp(start + 1024, 0., double(Packets - 1));
			start = std::clamp(end - 1024, 0., double(Packets - 1));
			Require(
				std::bit_cast<uint64_t>(out.Cursor) == std::bit_cast<uint64_t>(offset / Packets) &&
					std::bit_cast<uint64_t>(out.Start) == std::bit_cast<uint64_t>(start / Packets) &&
					std::bit_cast<uint64_t>(out.End) == std::bit_cast<uint64_t>(end / Packets),
				"audio observation marker words"
			);
			for (double v : {out.Cursor, out.Start, out.End, out.SampleRate})
				Word(h, std::bit_cast<uint64_t>(v));
			Word(h, out.Channels);
			Word(h, out.Packets);
			return h;
		}
	};
}
