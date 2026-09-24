#pragma once

// CPU evaluation of typed value nodes after graph inputs have been resolved.

#include "TextOps.hpp"
#include "Utf8TextOps.hpp"
#include "ValueOps.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::imagegraph::detail {
	template <class T>
	T ValueInput(std::span<const AuthoredValue> inputs, std::string_view port, T fallback) {
		for (const AuthoredValue &input : inputs)
			if (input.Port == port) return std::get<T>(input.Data);
		return fallback;
	}

	inline Status EvaluateValueNode(
		const Node &node,
		std::span<const AuthoredValue> inputs,
		const EvaluationRequest &request,
		const TimelineSettings *timeline,
		std::vector<AuthoredValue> &outputs,
		std::string &failedPort,
		std::string &failureMessage
	) {
		const auto scalar = [&](std::string_view port, double fallback = 0.0) {
			return ValueInput<double>(inputs, port, fallback);
		};
		const auto integer = [&](std::string_view port, int64_t fallback = 0) {
			return ValueInput<int64_t>(inputs, port, fallback);
		};
		const auto boolean = [&](std::string_view port, bool fallback = false) {
			return ValueInput<bool>(inputs, port, fallback);
		};
		const auto vector = [&](std::string_view port, Vector2 fallback = {}) {
			return ValueInput<Vector2>(inputs, port, fallback);
		};
		const auto colour = [&](std::string_view port, Colour fallback = {}) {
			return ValueInput<Colour>(inputs, port, fallback);
		};
		const auto text = [&](std::string_view port, std::string fallback = {}) {
			return ValueInput<std::string>(inputs, port, std::move(fallback));
		};
		const auto valueInput = [&](std::string_view port) -> const Value * {
			for (const AuthoredValue &input : inputs)
				if (input.Port == port) return &input.Data;
			return nullptr;
		};
		outputs.clear();
		failureMessage = "typed value operation failed";
		if (node.Type == "image.audio_recording") {
			const std::string sourceId = text("source_id");
			const auto capture = std::find_if(
				request.AudioFrames.begin(), request.AudioFrames.end(), [&](const AudioCaptureFrame &frame) {
					return frame.SourceId == sourceId && frame.Tick == request.Tick;
				}
			);
			if (sourceId.empty() || capture == request.AudioFrames.end()) {
				failedPort = "source_id";
				failureMessage = "no recorded mono audio frame matches this source ID and exact tick";
				return Status::InvalidValue;
			}
			ArrayValue samples{ValueType::Scalar, {}};
			samples.Elements.reserve(capture->Samples.size());
			for (const double sample : capture->Samples)
				samples.Elements.emplace_back(sample);
			outputs.push_back({"samples", std::move(samples)});
			outputs.push_back({"audio", AudioBit{capture->Samples, capture->SampleRate}});
		} else if (node.Type == "image.audio_volume") {
			const Value *data = valueInput("samples");
			const auto *samples = data ? std::get_if<ArrayValue>(data) : nullptr;
			if (samples == nullptr || samples->ElementType != ValueType::Scalar ||
				samples->Elements.size() > Limits::MaximumArrayElements) {
				failedPort = "samples";
				failureMessage = "Audio Volume requires a bounded array of scalar mono samples";
				return Status::InvalidValue;
			}
			double squaredTotal = 0.0;
			for (const ElementValue &element : samples->Elements) {
				const auto *sample = std::get_if<double>(&element);
				if (sample == nullptr || !std::isfinite(*sample)) {
					failedPort = "samples";
					failureMessage = "Audio Volume samples must be finite scalar values";
					return Status::InvalidValue;
				}
				const double square = *sample * *sample;
				if (!std::isfinite(square) || !std::isfinite(squaredTotal + square)) {
					failedPort = "samples";
					failureMessage = "Audio Volume sample energy exceeds finite scalar range";
					return Status::InvalidValue;
				}
				squaredTotal += square;
			}
			if (samples->Elements.empty()) {
				outputs.push_back({"loudness", 0.0});
			} else {
				const double rms = std::sqrt(squaredTotal / samples->Elements.size());
				if (rms == 0.0) {
					failedPort = "samples";
					failureMessage = "nonempty silent audio yields a non-finite 10*log10(RMS) result; finite "
									 "scalar preview refused";
					return Status::InvalidValue;
				}
				const double loudness = 10.0 * std::log10(rms);
				if (!std::isfinite(loudness)) {
					failedPort = "samples";
					failureMessage = "Audio Volume produced a non-finite loudness value";
					return Status::InvalidValue;
				}
				outputs.push_back({"loudness", loudness});
			}
		} else if (node.Type == "image.audio_window") {
			const Value *audioData = valueInput("audio");
			const auto *audio = audioData ? std::get_if<AudioBit>(audioData) : nullptr;
			const Value *arrayData = valueInput("samples");
			const auto *samples = arrayData ? std::get_if<ArrayValue>(arrayData) : nullptr;
			if (audio != nullptr && samples != nullptr) {
				failedPort = "audio";
				failureMessage = "Audio Window accepts either typed audio or legacy scalar samples, not both";
				return Status::InvalidValue;
			}
			if (audio == nullptr && (samples == nullptr || samples->ElementType != ValueType::Scalar ||
									 samples->Elements.size() > Limits::MaximumArrayElements)) {
				failedPort = "samples";
				failureMessage = "Audio Window requires bounded typed mono audio or a scalar sample array";
				return Status::InvalidValue;
			}
			const int64_t width = integer("width", 4096);
			const int64_t step = integer("step", 16);
			const EnumValue cursor = ValueInput<EnumValue>(inputs, "cursor_location", {1});
			if (width <= 0 || width > static_cast<int64_t>(Limits::MaximumArrayElements) || step <= 0) {
				failedPort = width <= 0 || width > static_cast<int64_t>(Limits::MaximumArrayElements)
								 ? "width"
								 : "step";
				failureMessage =
					"Audio Window Width and Step must be positive within the native sample limit";
				return Status::InvalidValue;
			}
			if (cursor.Value < 0 || cursor.Value > 2) {
				failedPort = "cursor_location";
				failureMessage = "Audio Window Cursor Location must be Start, Middle, or End";
				return Status::InvalidValue;
			}
			std::vector<double> legacySamples;
			if (audio == nullptr) {
				legacySamples.reserve(samples->Elements.size());
				for (const ElementValue &element : samples->Elements) {
					const auto *sample = std::get_if<double>(&element);
					if (sample == nullptr || !std::isfinite(*sample)) {
						failedPort = "samples";
						failureMessage = "Audio Window samples must be finite scalar values";
						return Status::InvalidValue;
					}
					legacySamples.push_back(*sample);
				}
			} else if (!std::isfinite(audio->SampleRate) || audio->SampleRate <= 0.0 ||
					   audio->Samples.size() > Limits::MaximumAudioSamplesPerFrame ||
					   !std::all_of(audio->Samples.begin(), audio->Samples.end(), [](double sample) {
						   return std::isfinite(sample);
					   })) {
				failedPort = "audio";
				failureMessage =
					"Audio Window typed audio must have finite mono samples and a positive sample rate";
				return Status::InvalidValue;
			}
			const std::span<const double> source =
				audio != nullptr ? std::span(audio->Samples) : std::span(legacySamples);
			double location = scalar("location");
			if (boolean("match_timeline", true)) {
				if (audio == nullptr) {
					failedPort = "audio";
					failureMessage =
						"Audio Window Match Timeline requires typed audio with a captured sample rate";
					return Status::UnsupportedExecution;
				}
				if (timeline == nullptr) {
					failedPort = "match_timeline";
					failureMessage =
						"Audio Window Match Timeline requires declared timeline frames per second";
					return Status::UnsupportedExecution;
				}
				location = static_cast<double>(request.Tick) / timeline->FramesPerSecond * audio->SampleRate;
			}
			if (!std::isfinite(location)) {
				failedPort = "location";
				failureMessage = "Audio Window Location must be a finite sample index";
				return Status::InvalidValue;
			}
			const int64_t count = static_cast<int64_t>(source.size());
			const int64_t locationLimit = count + width;
			const int64_t roundedLocation = location <= -static_cast<double>(locationLimit) ? -locationLimit
											: location >= static_cast<double>(locationLimit)
												? locationLimit
												: static_cast<int64_t>(std::round(location));
			int64_t start = roundedLocation;
			if (cursor.Value == 1)
				start -= width / 2;
			else if (cursor.Value == 2)
				start -= width;
			const int64_t span = std::min(width, count);
			start = std::clamp(start, int64_t{0}, std::max(int64_t{0}, count - span));
			ArrayValue window{ValueType::Scalar, {}};
			window.Elements.reserve(static_cast<size_t>((span + step - 1) / step));
			for (int64_t index = start; index < start + span; index += step)
				window.Elements.emplace_back(source[static_cast<size_t>(index)]);
			outputs.push_back({"samples", std::move(window)});
		} else if (node.Type == "value.number")
			outputs.push_back({"number", scalar("value")});
		else if (node.Type == "value.boolean")
			outputs.push_back({"boolean", boolean("value")});
		else if (node.Type == "value.text")
			outputs.push_back({"text", text("value")});
		else if (node.Type == "value.vector2")
			outputs.push_back({"vector", Vector2{scalar("x"), scalar("y")}});
		else if (node.Type == "value.math") {
			const auto result = Math(
				scalar("a"),
				scalar("b"),
				scalar("amount"),
				vector("from", {0, 1}),
				vector("to", {0, 1}),
				integer("mode"),
				boolean("degrees", true)
			);
			if (!result) {
				failedPort = "mode";
				return Status::UnsupportedExecution;
			}
			outputs.push_back({"result", *result});
		} else if (node.Type == "value.compare") {
			const auto result = Compare(scalar("a"), scalar("b"), integer("mode"));
			if (!result) {
				failedPort = "mode";
				return Status::InvalidValue;
			}
			outputs.push_back({"result", *result});
		} else if (node.Type == "value.logic") {
			const auto result = Logic(boolean("a"), boolean("b"), integer("mode"));
			if (!result) {
				failedPort = "mode";
				return Status::InvalidValue;
			}
			outputs.push_back({"result", *result});
		} else if (node.Type == "value.color_rgb") {
			outputs.push_back(
				{"colour",
				 MakeRgbColour(
					 scalar("red", 1),
					 scalar("green", 1),
					 scalar("blue", 1),
					 scalar("alpha", 1),
					 boolean("normalized", true)
				 )}
			);
		} else if (node.Type == "value.color_hsv") {
			const bool normalized = boolean("normalized", true);
			const double scale = normalized ? 255.0 : 1.0;
			outputs.push_back(
				{"colour",
				 MakeHsvColour(
					 std::clamp(scalar("hue", 1), 0.0, 1.0) * scale,
					 std::clamp(scalar("saturation", 1), 0.0, 1.0) * scale,
					 std::clamp(scalar("value", 1), 0.0, 1.0) * scale,
					 std::clamp(scalar("alpha", 1), 0.0, 1.0) * scale
				 )}
			);
		} else if (node.Type == "value.color_data") {
			constexpr std::array<std::string_view, 8> names{
				"red", "green", "blue", "hue", "saturation", "value", "brightness", "alpha"
			};
			const auto data = ColorData(colour("colour", {255, 255, 255, 255}), boolean("normalized", true));
			for (size_t index = 0; index < names.size(); index++)
				outputs.push_back({std::string(names[index]), data[index]});
		} else if (node.Type == "value.color_mix") {
			const Colour from = colour("from", {255, 255, 255, 255});
			const Colour to = colour("to", {255, 255, 255, 255});
			const double amount = scalar("mix", 0.5);
			const int64_t space = integer("space");
			if (space == 0)
				outputs.push_back({"colour", MixRgbColour(from, to, amount)});
			else if (space == 1)
				outputs.push_back({"colour", MixHsvColour(from, to, amount)});
			else if (space == 2)
				outputs.push_back({"colour", MixOklabColour(from, to, amount)});
			else {
				failedPort = "space";
				return Status::UnsupportedExecution;
			}
		} else if (node.Type == "value.vector_magnitude")
			outputs.push_back({"result", VectorMagnitude(vector("vector"))});
		else if (node.Type == "value.vector_normalize")
			outputs.push_back({"result", Normalize(vector("vector"))});
		else if (node.Type == "value.vector_direction")
			outputs.push_back({"result", VectorDirection(vector("vector"), boolean("radians"))});
		else if (node.Type == "value.vector_dot")
			outputs.push_back({"result", Dot(vector("a"), vector("b"))});
		else if (node.Type == "value.vector_cross")
			outputs.push_back({"result", Cross(vector("a"), vector("b"))});
		else if (node.Type == "value.text_count")
			outputs.push_back({"count", static_cast<int64_t>(CountText(text("text"), text("find")))});
		else if (node.Type == "value.text_replace") {
			const auto result = ReplaceText(
				text("text"),
				text("find"),
				text("replacement"),
				boolean("all", true),
				Limits::MaximumTextBytes
			);
			if (!result) {
				failedPort = "find";
				return Status::InvalidValue;
			}
			outputs.push_back({"result", *result});
		} else if (node.Type == "value.text_combine") {
			std::vector<std::string> pieces;
			for (const DynamicInput &input : node.DynamicInputs) {
				if (input.Type != ValueType::Text) {
					failedPort = input.Id;
					return Status::TypeMismatch;
				}
				pieces.push_back(text(input.Id));
			}
			const auto result = CombineText(pieces, Limits::MaximumTextBytes);
			if (!result) {
				failedPort = "text";
				return Status::LimitExceeded;
			}
			outputs.push_back({"text", *result});
		} else if (node.Type == "value.text_split") {
			const auto result = SplitText(text("text"), text("delimiter", " "), Limits::MaximumArrayElements);
			if (!result) {
				failedPort = "delimiter";
				return Status::InvalidValue;
			}
			ArrayValue array;
			array.ElementType = ValueType::Text;
			for (const std::string &part : *result)
				array.Elements.push_back(part);
			outputs.push_back({"array", std::move(array)});
		} else if (node.Type == "value.text_length") {
			int64_t length = 0;
			const TextOpStatus status = TextLength(text("text"), integer("mode"), length);
			if (status != TextOpStatus::Ok) {
				failedPort = status == TextOpStatus::InvalidMode ? "mode" : "text";
				return status == TextOpStatus::LimitExceeded ? Status::LimitExceeded : Status::InvalidValue;
			}
			outputs.push_back({"length", length});
		} else if (node.Type == "value.text_get_char" || node.Type == "value.text_delete") {
			const int64_t index = integer("index", node.Type == "value.text_get_char" ? 1 : 0);
			const int64_t amount = integer("amount", 1);
			std::string result;
			const TextOpStatus status = node.Type == "value.text_get_char"
											? CopyText(text("text"), index, amount, result)
											: DeleteText(text("text"), index, amount, result);
			if (status != TextOpStatus::Ok) {
				failedPort = status == TextOpStatus::UnsupportedIndex
								 ? (node.Type == "value.text_get_char" && index > 0 ? "amount" : "index")
								 : "text";
				if (status == TextOpStatus::LimitExceeded) return Status::LimitExceeded;
				return status == TextOpStatus::UnsupportedIndex ? Status::UnsupportedExecution
																: Status::InvalidValue;
			}
			outputs.push_back({"text", std::move(result)});
		} else
			return Status::UnsupportedExecution;
		return Status::Ok;
	}
}
