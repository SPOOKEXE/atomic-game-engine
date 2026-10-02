#include <engine/imagegraphexport/GraphDeviceHost.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &invocation, std::string_view name) {
			const auto found =
				std::find_if(invocation.Inputs.begin(), invocation.Inputs.end(), [&](const auto &item) {
					return item.Port == name;
				});
			return found == invocation.Inputs.end() ? nullptr : &found->Data;
		}
		HostNodeCapture Bind(const HostNodeInvocation &invocation) {
			HostNodeCapture capture;
			capture.Authored = invocation.Authored;
			capture.Tick = invocation.Request.Tick;
			capture.Subframe = invocation.Request.Subframe;
			capture.NegativeFrame = invocation.Request.NegativeFrame;
			capture.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
			for (const auto &image : invocation.Images)
				if (image.Data)
					capture.InputImages.push_back({std::string(image.Port), SurfaceHash(*image.Data)});
			return capture;
		}
	}
	GraphDeviceHost::GraphDeviceHost(
		std::span<const GraphMidiFrame> midi,
		std::span<const GraphSpoutFrame> spout,
		std::span<const GraphDateTimeFrame> calendar
	)
		: Midi(midi), Spout(spout), Calendar(calendar) {}
	bool GraphDeviceHost::Capture(
		const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
	) {
		auto capture = Bind(invocation);
		if (invocation.Authored.Type == "pc.datetime_get") {
			const auto *value = Input(invocation, "format");
			const auto *format = value ? std::get_if<std::string>(value) : nullptr;
			const GraphDateTimeFrame *selected = nullptr;
			if (Calendar.size() > 4096 || !format || format->size() > invocation.MaximumOperationBytes / 16) {
				failure = "DateTime requires a bounded format and explicit calendar observation";
				return false;
			}
			for (const auto &frame : Calendar) {
				if (frame.NodeId != invocation.Authored.Id || frame.Tick != invocation.Request.Tick ||
					frame.Subframe != invocation.Request.Subframe ||
					frame.NegativeFrame != invocation.Request.NegativeFrame)
					continue;
				if (selected) {
					failure = "DateTime has duplicate calendar observations";
					return false;
				}
				selected = &frame;
			}
			if (!selected || selected->Year < 0 || selected->Year > 9999 || selected->Month < 1 ||
				selected->Month > 12 || selected->Day < 1 || selected->Day > 31 || selected->Weekday < 0 ||
				selected->Weekday > 6 || selected->Hour < 0 || selected->Hour > 23 || selected->Minute < 0 ||
				selected->Minute > 59 || selected->Second < 0 || selected->Second > 59 ||
				!std::chrono::year_month_day{
					std::chrono::year{selected->Year},
					std::chrono::month{static_cast<unsigned>(selected->Month)},
					std::chrono::day{static_cast<unsigned>(selected->Day)}
				}.ok()) {
				failure = "DateTime observation is absent or outside native calendar domains";
				return false;
			}
			auto result = *format;
			const auto padded = [](int number) { return (number < 10 ? "0" : "") + std::to_string(number); };
			const std::array<std::pair<std::string_view, std::string>, 8> replacements{
				{{"%s", padded(selected->Second)},
				 {"%n", padded(selected->Minute)},
				 {"%h", padded(selected->Hour)},
				 {"%d", padded(selected->Day)},
				 {"%w", std::to_string(selected->Weekday)},
				 {"%m", padded(selected->Month)},
				 {"%y", std::to_string(selected->Year)},
				 {"%tm", std::to_string(selected->TimerMicroseconds)}}
			};
			for (const auto &[token, replacement] : replacements) {
				size_t position = 0;
				while ((position = result.find(token, position)) != std::string::npos) {
					if (result.size() + replacement.size() > invocation.MaximumOperationBytes / 2) {
						failure = "DateTime formatted output exceeds its operation budget";
						return false;
					}
					result.replace(position, token.size(), replacement);
					position += replacement.size();
				}
			}
			capture.Outputs.push_back({"data", std::move(result)});
			output = std::move(capture);
			failure.clear();
			return true;
		}
		if (invocation.Authored.Type == "pc.midi_in") {
			const auto *input = Input(invocation, "input");
			const auto *device = input ? std::get_if<EnumValue>(input) : nullptr;
			if (!device || Midi.size() > 4096 || invocation.Request.NegativeFrame ||
				invocation.Request.Subframe != 0) {
				failure =
					"MIDI replay requires an exact nonnegative integer frame and a bounded device recording";
				return false;
			}
			std::array<std::optional<uint8_t>, 128> direct{};
			std::vector<uint8_t> pressing;
			ArrayValue raw;
			raw.ElementType = ValueType::Scalar;
			bool sawCurrent = false;
			std::optional<uint64_t> previous;
			uint64_t used = 0;
			for (const auto &frame : Midi) {
				if (frame.NodeId != invocation.Authored.Id || frame.Device != device->Value) continue;
				if (previous && frame.Tick <= *previous) {
					failure = "MIDI observations must have unique increasing ticks";
					return false;
				}
				previous = frame.Tick;
				if (frame.Messages.size() % 3 != 0 || frame.Messages.size() > Limits::MaximumArrayElements ||
					frame.Messages.size() > invocation.MaximumOperationBytes - used) {
					failure = "MIDI message bytes exceed their bounds or omit a triple";
					return false;
				}
				used += frame.Messages.size();
				if (frame.Tick > invocation.Request.Tick) continue;
				for (size_t index = 0; index < frame.Messages.size(); index += 3) {
					const auto status = frame.Messages[index], key = frame.Messages[index + 1],
							   value = frame.Messages[index + 2];
					if (status < 128 || status > 239 || key > 127 || value > 127) {
						failure = "MIDI recording contains an invalid three-byte channel message";
						return false;
					}
					if (status <= 159) {
						auto found = std::find(pressing.begin(), pressing.end(), key);
						if (status <= 143) {
							if (found != pressing.end()) pressing.erase(found);
						} else if (found == pressing.end())
							pressing.push_back(key);
					}
					direct[key] = value;
				}
				if (frame.Tick == invocation.Request.Tick) {
					const uint64_t maximumOutputBytes =
						frame.Messages.size() * sizeof(ElementValue) +
						128 * (sizeof(ElementValue) + sizeof(std::pair<std::string, Value>) + 3) +
						invocation.Authored.DynamicOutputs.size() * sizeof(AuthoredValue);
					if (maximumOutputBytes > invocation.MaximumOperationBytes / 2) {
						failure = "MIDI decoded outputs exceed their operation budget";
						return false;
					}
					raw.Elements.reserve(frame.Messages.size());
					sawCurrent = true;
					for (auto byte : frame.Messages)
						raw.Elements.emplace_back(static_cast<double>(byte));
				}
			}
			if (!sawCurrent) {
				failure = "MIDI recording has no observation at this exact frame and device";
				return false;
			}
			ArrayValue notes;
			notes.ElementType = ValueType::Scalar;
			for (auto key : pressing)
				notes.Elements.emplace_back(static_cast<double>(key));
			StructValue values;
			values.Data.emplace();
			for (size_t key = 0; key < direct.size(); key++)
				if (direct[key])
					values.Data->Fields.emplace_back(std::to_string(key), static_cast<double>(*direct[key]));
			capture.Outputs = {
				{"raw_message", std::move(raw)},
				{"pressing_notes", std::move(notes)},
				{"direct_values", std::move(values)}
			};
			if (invocation.Authored.DynamicInputs.size() != invocation.Authored.DynamicOutputs.size() * 2) {
				failure = "MIDI watcher inputs and outputs are not paired";
				return false;
			}
			for (size_t watcher = 0; watcher < invocation.Authored.DynamicOutputs.size(); watcher++) {
				const auto *indexValue = Input(invocation, invocation.Authored.DynamicInputs[watcher * 2].Id),
						   *normalizeValue =
							   Input(invocation, invocation.Authored.DynamicInputs[watcher * 2 + 1].Id);
				const auto *key = indexValue ? std::get_if<int64_t>(indexValue) : nullptr;
				const auto *normalize = normalizeValue ? std::get_if<bool>(normalizeValue) : nullptr;
				if (!key || !normalize || *key < -1 || *key > 127) {
					failure = "MIDI watcher controls have invalid types or bounds";
					return false;
				}
				double result =
					*key >= 0 && direct[static_cast<size_t>(*key)] ? *direct[static_cast<size_t>(*key)] : 0;
				if (*normalize) result /= 127;
				capture.Outputs.push_back({invocation.Authored.DynamicOutputs[watcher].Id, result});
			}
		} else if (invocation.Authored.Type == "pc.spout_receive" ||
				   invocation.Authored.Type == "pc.spout_send") {
			const bool sending = invocation.Authored.Type == "pc.spout_send";
			const auto *nameValue = Input(invocation, sending ? "sender_name" : "receiver_name");
			const auto *name = nameValue ? std::get_if<std::string>(nameValue) : nullptr;
			const GraphSpoutFrame *selected = nullptr;
			if (!name || Spout.size() > 4096 || invocation.Request.NegativeFrame ||
				invocation.Request.Subframe != 0) {
				failure = "Spout needs a bounded exact frame recording and name";
				return false;
			}
			for (const auto &frame : Spout)
				if (frame.NodeId == invocation.Authored.Id && frame.Tick == invocation.Request.Tick &&
					frame.Name == *name && frame.Sending == sending) {
					if (selected) {
						failure = "Spout observation node, name and tick are duplicated";
						return false;
					}
					selected = &frame;
				}
			if (!selected || !selected->Accepted) {
				failure = "Spout host did not record an accepted observation for this node, name and tick";
				return false;
			}
			if (!ValidSurfaceLayout(
					selected->Surface,
					invocation.Request.MaximumImageDimension,
					invocation.MaximumOperationBytes
				) ||
				!FiniteSurfaceSamples(selected->Surface)) {
				failure = "Spout recording surface exceeds its bounds";
				return false;
			}
			if (sending) {
				const auto source =
					std::find_if(invocation.Images.begin(), invocation.Images.end(), [](const auto &image) {
						return image.Port == "surface";
					});
				if (source == invocation.Images.end() || !source->Data ||
					*source->Data != selected->Surface) {
					failure = "Spout send receipt does not bind the actual upstream surface";
					return false;
				}
			}
			capture.Images.push_back({"surface", selected->Surface});
		} else {
			failure = "device host does not support this node type";
			return false;
		}
		output = std::move(capture);
		return true;
	}
}
