#pragma once

#include "ComputeGraph.hpp"

#include <engine/script/ComputeBridge.hpp>

#include <memory>
#include <vector>

namespace engine::render {
	struct ScriptComputeJobProfile {
		computegraph::Owner Owner;
		uint64_t Ticket = 0;
		uint64_t QueuedAt = 0;
		uint64_t QueueNanoseconds = 0;
		uint64_t SubmitToReadyNanoseconds = 0;
		uint64_t SubmitToReleaseNanoseconds = 0;
		uint64_t ReadCalls = 0;
		uint64_t ReadBytes = 0;
		bool Released = false;
		bool Retired = false;
		bool Cancelled = false;
		bool DeviceCountsAvailable = false;
		ComputeGraphTiming Device;
	};
	// Opt-in diagnostic accounting for the real neutral endpoints. Records are
	// bounded to sixteen jobs and consumed explicitly, independently of VM release.
	struct ScriptComputeProfile {
		bool Enabled = false;
		uint64_t Definitions = 0, Submitted = 0, Completed = 0, Released = 0, Cancelled = 0, Refused = 0;
		uint64_t RetiredJobs = 0;
		uint64_t PollCalls = 0, ReadCalls = 0, ReadBytes = 0, DroppedRecords = 0;
		uint64_t GpuTimed = 0, GpuDropped = 0, GpuUnavailable = 0;
		uint64_t CurrentEntries = 0, PeakEntries = 0;
		uint64_t CurrentUploadBytes = 0, PeakUploadBytes = 0;
		uint64_t CurrentReadbackBytes = 0, PeakReadbackBytes = 0;
		uint64_t CurrentResultBytes = 0, PeakResultBytes = 0;
		uint64_t CurrentDeviceBytes = 0, PeakDeviceBytes = 0;
		std::vector<ScriptComputeJobProfile> Jobs;
	};
	// One renderer-wide copied inbox and device executor pool, shared by all its VMs.
	class ScriptComputeBridge {
	  public:
		// Borrows the existing renderer device; construction does not create a device.
		explicit ScriptComputeBridge(void *device);
		~ScriptComputeBridge();
		ScriptComputeBridge(const ScriptComputeBridge &) = delete;
		ScriptComputeBridge &operator=(const ScriptComputeBridge &) = delete;
		// Creates a factory whose endpoints are scoped to one stable world and view.
		script::ComputeBridgeFactory ForWorld(std::string world, uint32_t view = UINT32_MAX);
		// Device owner thread only. Outside bytes are ordinary renderer graph buffers.
		void Pump(uint64_t outsideBytes);
		// Includes every installed and retired executor until its GPU jobs complete.
		uint64_t ReservedDeviceBytes() const;
		// Owner-only private instrumentation on an actual endpoint returned by ForWorld.
		// Enable before graph definition; taking a snapshot consumes only completed, released job records.
		static bool EnableProfiling(script::ComputeBridge &endpoint, bool enabled = true);
		static ScriptComputeProfile TakeProfile(script::ComputeBridge &endpoint);
		// Called by Renderer before destroying its device. May wait only at teardown.
		void Shutdown();

	  private:
		struct State;
		std::shared_ptr<State> Held;
	};
}
