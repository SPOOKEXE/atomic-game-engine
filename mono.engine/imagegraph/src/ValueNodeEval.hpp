#pragma once

// CPU evaluation of typed value nodes after graph inputs have been resolved.

#include "EvaluationBudget.hpp"
#include "TextOps.hpp"
#include "Utf8TextOps.hpp"
#include "ValueNodeSchemas.hpp"
#include "ValueOps.hpp"
#include "ValuePayload.hpp"

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
	struct ValueInputView {
		std::string_view Port;
		const Value *Data = nullptr;
	};

	template <class T>
	T ValueInput(std::span<const ValueInputView> inputs, std::string_view port, T fallback) {
		for (const ValueInputView &input : inputs)
			if (input.Port == port && input.Data) return std::get<T>(*input.Data);
		return fallback;
	}

	inline bool
	ReserveLegacyValueOutput(EvaluationBudget &budget, AllocationReservation &charge, uint64_t bytes) {
		auto reservation = budget.Reserve(bytes);
		return reservation && charge.Merge(std::move(*reservation));
	}

	inline Status EvaluateValueNode(
		const Node &node,
		std::span<const ValueInputView> inputs,
		const EvaluationRequest &request,
		const TimelineSettings *timeline,
		std::vector<AuthoredValue> &outputs,
		EvaluationBudget &budget,
		AllocationReservation &outputCharge,
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
		const auto text = [&](std::string_view port, std::string_view fallback = {}) {
			for (const ValueInputView &input : inputs)
				if (input.Port == port && input.Data)
					if (const auto *value = std::get_if<std::string>(input.Data))
						return std::string_view(*value);
			return fallback;
		};
		const auto valueInput = [&](std::string_view port) -> const Value * {
			for (const ValueInputView &input : inputs)
				if (input.Port == port) return input.Data;
			return nullptr;
		};
		outputs.clear();
		failureMessage = "typed value operation failed";
		const NodeSchema *schema = FindValueNodeSchema(node.Type);
		if (!schema) return Status::UnsupportedExecution;
		size_t outputCount = 0;
		uint64_t outputBaseBytes = 0;
		std::string_view firstOutput;
		for (const PortSchema &port : schema->Ports) {
			if (port.Direction != PortDirection::Output) continue;
			if (firstOutput.empty()) firstOutput = port.Id;
			outputCount++;
			const uint64_t nameBytes = std::max<uint64_t>(port.Id.size(), std::string{}.capacity());
			if (nameBytes > Limits::MaximumEvaluationBytes - outputBaseBytes) return Status::LimitExceeded;
			outputBaseBytes += nameBytes;
		}
		if (outputCount > (Limits::MaximumEvaluationBytes - outputBaseBytes) / sizeof(AuthoredValue))
			return Status::LimitExceeded;
		outputBaseBytes += outputCount * sizeof(AuthoredValue);
		if (!ReserveLegacyValueOutput(budget, outputCharge, outputBaseBytes)) {
			failedPort = std::string(firstOutput);
			failureMessage = "typed value output exceeds the live byte budget";
			return Status::LimitExceeded;
		}
		outputs.reserve(outputCount);
		const auto reservePayload = [&](uint64_t bytes, std::string_view port) {
			if (bytes > Limits::MaximumEvaluationBytes ||
				!ReserveLegacyValueOutput(budget, outputCharge, bytes)) {
				failedPort = std::string(port);
				return false;
			}
			return true;
		};
		const auto publishText = [&](std::string_view port, std::string_view source) {
			if (!reservePayload(std::max<uint64_t>(source.size(), std::string{}.capacity()), port))
				return false;
			std::string result(source.size(), '\0');
			std::copy(source.begin(), source.end(), result.begin());
			outputs.push_back({std::string(port), std::move(result)});
			return true;
		};
		if (node.Type == "image.audio_recording") {
			const std::string_view sourceId = text("source_id");
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
			if (capture->Samples.size() > Limits::MaximumEvaluationBytes / sizeof(ElementValue) ||
				capture->Samples.size() > Limits::MaximumEvaluationBytes / sizeof(double) ||
				capture->Channels.size() >
					Limits::MaximumEvaluationBytes / sizeof(std::vector<ElementValue>) ||
				capture->Channels.size() > Limits::MaximumEvaluationBytes / sizeof(std::vector<double>)) {
				failedPort = "source_id";
				return Status::LimitExceeded;
			}
			uint64_t copiedBytes = capture->Samples.size() * sizeof(ElementValue) +
								   capture->Channels.size() * sizeof(std::vector<ElementValue>);
			uint64_t audioBytes = capture->Samples.size() * sizeof(double) +
								  capture->Channels.size() * sizeof(std::vector<double>);
			if (copiedBytes > Limits::MaximumEvaluationBytes || audioBytes > Limits::MaximumEvaluationBytes) {
				failedPort = "source_id";
				return Status::LimitExceeded;
			}
			for (const auto &channel : capture->Channels) {
				if (channel.size() > (Limits::MaximumEvaluationBytes - copiedBytes) / sizeof(ElementValue) ||
					channel.size() > (Limits::MaximumEvaluationBytes - audioBytes) / sizeof(double)) {
					failedPort = "source_id";
					return Status::LimitExceeded;
				}
				copiedBytes += channel.size() * sizeof(ElementValue);
				audioBytes += channel.size() * sizeof(double);
			}
			if (copiedBytes > Limits::MaximumEvaluationBytes - audioBytes ||
				!reservePayload(copiedBytes + audioBytes, "source_id"))
				return Status::LimitExceeded;
			ArrayValue samples{ValueType::Scalar, {}};
			samples.Elements.reserve(capture->Samples.size());
			for (const double sample : capture->Samples)
				samples.Elements.emplace_back(sample);
			if (!capture->Channels.empty()) {
				samples.Nested.reserve(capture->Channels.size());
				for (const auto &channel : capture->Channels) {
					auto &row = samples.Nested.emplace_back();
					row.reserve(channel.size());
					for (double sample : channel)
						row.emplace_back(sample);
				}
			}
			outputs.push_back({"samples", std::move(samples)});
			outputs.push_back({"audio", AudioBit{capture->Samples, capture->SampleRate, capture->Channels}});
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
			AllocationReservation legacySamplesCharge;
			std::vector<double> legacySamples;
			if (audio == nullptr) {
				const uint64_t scratchBytes = samples->Elements.size() * sizeof(double);
				if (!ReserveLegacyValueOutput(budget, legacySamplesCharge, scratchBytes)) {
					failedPort = "samples";
					return Status::LimitExceeded;
				}
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
				// Legacy slicing still ignores Subframe; the authoring sign applies to its whole tick.
				location = (request.NegativeFrame ? -static_cast<double>(request.Tick)
												  : static_cast<double>(request.Tick)) /
						   timeline->FramesPerSecond * audio->SampleRate;
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
			const size_t windowCount = static_cast<size_t>((span + step - 1) / step);
			const uint64_t windowBytes = windowCount * sizeof(ElementValue);
			if (!reservePayload(windowBytes, "samples")) return Status::LimitExceeded;
			ArrayValue window{ValueType::Scalar, {}};
			window.Elements.reserve(windowCount);
			for (int64_t index = start; index < start + span; index += step)
				window.Elements.emplace_back(source[static_cast<size_t>(index)]);
			outputs.push_back({"samples", std::move(window)});
		} else if (node.Type == "value.number")
			outputs.push_back({"number", scalar("value")});
		else if (node.Type == "value.boolean")
			outputs.push_back({"boolean", boolean("value")});
		else if (node.Type == "value.text") {
			if (!publishText("text", text("value"))) return Status::LimitExceeded;
		} else if (node.Type == "value.vector2")
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
			const std::string_view source = text("text");
			const std::string_view find = text("find");
			const std::string_view replacement = text("replacement");
			const bool replaceAll = boolean("all", true);
			const auto resultBytes =
				ReplacementTextSize(source, find, replacement, replaceAll, Limits::MaximumTextBytes);
			if (!resultBytes) {
				failedPort = "find";
				return Status::InvalidValue;
			}
			if (!reservePayload(std::max<uint64_t>(*resultBytes, std::string{}.capacity()), "result"))
				return Status::LimitExceeded;
			auto result = BuildReplacementText(
				source, find, replacement, replaceAll, Limits::MaximumTextBytes, *resultBytes
			);
			if (!result) return Status::InvalidValue;
			outputs.push_back({"result", std::move(*result)});
		} else if (node.Type == "value.text_combine") {
			AllocationReservation piecesCharge;
			const size_t pieceCount = node.DynamicInputs.size();
			if (pieceCount > Limits::MaximumEvaluationBytes / sizeof(std::string_view) ||
				!ReserveLegacyValueOutput(budget, piecesCharge, pieceCount * sizeof(std::string_view))) {
				failedPort = "inputs";
				return Status::LimitExceeded;
			}
			std::vector<std::string_view> pieces;
			pieces.reserve(pieceCount);
			size_t outputBytes = 0;
			for (const DynamicInput &input : node.DynamicInputs) {
				if (input.Type != ValueType::Text) {
					failedPort = input.Id;
					return Status::TypeMismatch;
				}
				const std::string_view piece = text(input.Id);
				if (piece.size() > Limits::MaximumTextBytes - outputBytes) {
					failedPort = "text";
					return Status::LimitExceeded;
				}
				outputBytes += piece.size();
				pieces.push_back(piece);
			}
			if (!reservePayload(std::max<uint64_t>(outputBytes, std::string{}.capacity()), "text"))
				return Status::LimitExceeded;
			std::string result(outputBytes, '\0');
			size_t destination = 0;
			for (const std::string_view piece : pieces) {
				std::copy(piece.begin(), piece.end(), result.begin() + destination);
				destination += piece.size();
			}
			outputs.push_back({"text", std::move(result)});
		} else if (node.Type == "value.text_split") {
			const std::string_view source = text("text");
			const std::string_view delimiter = text("delimiter", " ");
			if (delimiter.empty()) {
				failedPort = "delimiter";
				return Status::InvalidValue;
			}
			size_t partCount = 1;
			for (size_t offset = 0; (offset = source.find(delimiter, offset)) != std::string_view::npos;
				 offset += delimiter.size())
				partCount++;
			if (partCount > Limits::MaximumArrayElements) {
				failedPort = "delimiter";
				return Status::InvalidValue;
			}
			if (partCount > Limits::MaximumEvaluationBytes / sizeof(ElementValue)) {
				failedPort = "array";
				return Status::LimitExceeded;
			}
			uint64_t payloadBytes = partCount * sizeof(ElementValue);
			size_t offset = 0;
			for (size_t part = 0; part < partCount; part++) {
				const size_t match = source.find(delimiter, offset);
				const size_t end = match == std::string_view::npos ? source.size() : match;
				const uint64_t retained = std::max<uint64_t>(end - offset, std::string{}.capacity());
				if (retained > Limits::MaximumEvaluationBytes - payloadBytes) {
					failedPort = "array";
					return Status::LimitExceeded;
				}
				payloadBytes += retained;
				offset = match == std::string_view::npos ? source.size() : match + delimiter.size();
			}
			if (!reservePayload(payloadBytes, "array")) return Status::LimitExceeded;
			ArrayValue array;
			array.ElementType = ValueType::Text;
			array.Elements.reserve(partCount);
			offset = 0;
			for (size_t part = 0; part < partCount; part++) {
				const size_t match = source.find(delimiter, offset);
				const size_t end = match == std::string_view::npos ? source.size() : match;
				array.Elements.emplace_back(std::string(source.substr(offset, end - offset)));
				offset = match == std::string_view::npos ? source.size() : match + delimiter.size();
			}
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
			const std::string_view source = text("text");
			if (!reservePayload(std::max<uint64_t>(source.size(), std::string{}.capacity()), "text"))
				return Status::LimitExceeded;
			std::string result(source.size(), '\0');
			const TextOpStatus status = node.Type == "value.text_get_char"
											? CopyText(source, index, amount, result)
											: DeleteText(source, index, amount, result);
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
