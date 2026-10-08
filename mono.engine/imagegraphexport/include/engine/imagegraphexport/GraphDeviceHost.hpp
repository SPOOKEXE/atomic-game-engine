#pragma once
#include <engine/imagegraph/HostCapture.hpp>

#include <span>
#include <string>
#include <vector>

namespace engine::imagegraphexport {
	// MIDI observations are complete source triples, recorded at explicit project ticks.
	struct GraphMidiFrame {
		std::string NodeId;
		int64_t Device = 0;
		uint64_t Tick = 0;
		std::vector<uint8_t> Messages;
	};
	struct GraphSpoutFrame {
		std::string NodeId;
		std::string Name;
		uint64_t Tick = 0;
		engine::imagegraph::Image Surface;
		bool Sending = false;
		bool Accepted = true;
		// Recorded final receive callback state, including its guarded assignment.
		std::optional<bool> SourceUpdateOnFrame{};
	};
	struct GraphDateTimeFrame {
		std::string NodeId;
		uint64_t Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		int Year = 1970, Month = 1, Day = 1, Weekday = 0;
		int Hour = 0, Minute = 0, Second = 0;
		uint64_t TimerMicroseconds = 0;
	};
	// Converts recorded native device data into bound node observations. No ambient device is opened.
	class GraphDeviceHost final : public engine::imagegraph::HostNodeProvider {
	  public:
		GraphDeviceHost(
			std::span<const GraphMidiFrame> midi,
			std::span<const GraphSpoutFrame> spout,
			std::span<const GraphDateTimeFrame> calendar = {}
		);
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override;

	  private:
		std::span<const GraphMidiFrame> Midi;
		std::span<const GraphSpoutFrame> Spout;
		std::span<const GraphDateTimeFrame> Calendar;
	};
}
