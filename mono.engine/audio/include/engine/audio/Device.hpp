#pragma once

// Where the samples go, and the null device that means everything above this
// can be tested without a sound card.
//
// **The null device is the point of the abstraction**, not a convenience. Every
// suite in this module runs against one: `Renderer::Initialise(nullptr)` made
// the same choice for graphics, and `AGENTS.md` is explicit that a header
// needing a GPU has no unit suite. Audio would have the same problem - CI has
// no sound card and a developer's is in use - except that a mixer's output is
// *data*, so the only part that genuinely needs hardware is the handover. That
// part is small and is the only part left uncovered.
//
// **The real device owns a thread with a hard deadline, and a missed buffer
// is audible.** So the callback does exactly one
// thing - render a block into the buffer SDL asked for - and everything that
// could allocate, block or take a lock happens elsewhere:
//
// - the graph is the mixer's and only the callback touches it
// - commands arrive through a lock-free queue
// - scratch buffers are sized when the graph changes, never during a render
// - a sound is a `shared_ptr` copied on the tick side, so the callback never
//   allocates one and never frees the last reference to one either
//
// **A machine with no audio output is not an error.** It is a laptop with its
// output muted at the driver level, a CI container, a server that linked the
// client library. `Open` answers null and the caller carries on - a game that
// refused to start because it could not make a noise would be worse than one
// that is quiet.
//
// @tier L12 · client

#include <engine/audio/Mixer.hpp>
#include <engine/audio/Sample.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::audio {

	// How a device is set up.
	//
	// @since v0.9
	struct DeviceSettings {
		// What to ask the hardware for.
		AudioFormat Format;

		// How many frames the mixer renders at once.
		//
		// **The latency/robustness trade, and it is the only one here.**
		// Smaller means a queued sound is heard sooner and the callback has
		// less time to finish; larger means the opposite. 512 at 48 kHz is
		// 10.7 ms and is a compromise rather than a measurement.
		size_t BlockFrames = DEFAULT_BLOCK_FRAMES;
	};

	// A sink for a mixer's output.
	//
	// @since v0.9
	class Device {
	  public:
		virtual ~Device() = default;

		// The mixer feeding this device.
		//
		// **A tick talks to `Mixer::Commands()` and to nothing else on it.**
		// Reaching for `Graph()` from the tick thread while a real device is
		// running is a data race with the callback - the one mistake this whole
		// arrangement is shaped to prevent, and the one the type system cannot
		// stop.
		virtual AudioMixer &Mixer() = 0;

		// The format actually in use.
		//
		// Not necessarily what was asked for: a device may hand back another
		// rate, and the mixer is built for what was granted rather than what
		// was wanted.
		virtual const AudioFormat &Format() const = 0;

		// Whether audio is actually being produced.
		virtual bool Running() const = 0;

		// How many frames have been rendered over this device's life.
		virtual uint64_t Rendered() const = 0;

		// Stops producing audio. Safe to call twice.
		virtual void Close() = 0;
	};

	// A device that renders only when asked.
	//
	// **What every test in this module runs against**, and what a headless
	// build gets. It has no thread and no hardware: `Advance` renders blocks
	// synchronously, so a suite states how much time passed rather than waiting
	// for it - the same discipline `net` applies to timeouts.
	//
	// @since v0.9
	class NullDevice : public Device {
	  public:
		virtual ~NullDevice() = default;

		// Renders `blocks` blocks immediately.
		//
		// @param blocks How many to render.
		// @return How many frames were produced.
		virtual size_t Advance(size_t blocks = 1) = 0;

		// The most recently rendered block.
		//
		// What lets a test assert on what *would* have been heard. A real
		// device has nothing like this, deliberately: the samples are gone.
		virtual const SampleBuffer &LastBlock() const = 0;
	};

	// Opens the system's audio output.
	//
	// @param settings What to ask for.
	// @return The device, or nothing when there is no audio output - which is
	//         an ordinary outcome on a CI container or a machine with its
	//         output disabled, and **not an error**. A caller runs quietly.
	// @since v0.9
	std::unique_ptr<Device> OpenDevice(const DeviceSettings &settings = {});

	// Builds a device that renders on demand and plays nothing.
	//
	// @param settings What to render.
	// @return The device. Never null: there is no hardware to fail.
	// @since v0.9
	std::unique_ptr<NullDevice> OpenNullDevice(const DeviceSettings &settings = {});
}
