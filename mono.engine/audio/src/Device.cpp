#include <engine/audio/Device.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Log.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include <algorithm>
#include <atomic>
#include <vector>

// The two devices, and the one file in this module that knows what SDL is.
//
// **The callback does one thing.** It renders a block and copies it out.
// Everything that could allocate, block or take a lock has been moved off this
// path by the design above it: the graph belongs to the mixer and only this
// thread touches it, commands arrive lock-free, scratch is sized when the graph
// changes, and a `SoundRef` is copied on the tick side so the callback neither
// allocates one nor drops the last reference to one.
//
// A missed buffer is audible, which is why that list is worth keeping short.

namespace engine::audio {
	namespace {

		class SilentDevice final : public NullDevice {
		  public:
			explicit SilentDevice(const DeviceSettings &settings)
				: Shape(settings.Format.IsValid() ? settings.Format : AudioFormat{}),
				  Engine(Shape, settings.BlockFrames),
				  Block(Shape, settings.BlockFrames == 0 ? DEFAULT_BLOCK_FRAMES : settings.BlockFrames) {}

			AudioMixer &Mixer() override {
				return Engine;
			}

			const AudioFormat &Format() const override {
				return Shape;
			}

			bool Running() const override {
				// Always: there is nothing to fail and nothing to stop.
				return true;
			}

			uint64_t Rendered() const override {
				return Engine.Clock();
			}

			void Close() override {}

			size_t Advance(size_t blocks) override {
				size_t frames = 0;
				for (size_t index = 0; index < blocks; ++index) {
					Engine.Render(Block);
					frames += Block.Frames();
				}
				return frames;
			}

			const SampleBuffer &LastBlock() const override {
				return Block;
			}

		  private:
			AudioFormat Shape;
			AudioMixer Engine;
			SampleBuffer Block;
		};

		class SdlDevice final : public Device {
		  public:
			SdlDevice(const AudioFormat &format, size_t blockFrames)
				: Shape(format), Engine(format, blockFrames), Block(format, blockFrames) {}

			~SdlDevice() override {
				Close();
			}

			AudioMixer &Mixer() override {
				return Engine;
			}

			const AudioFormat &Format() const override {
				return Shape;
			}

			bool Running() const override {
				return Stream != nullptr;
			}

			uint64_t Rendered() const override {
				return Frames.load(std::memory_order_relaxed);
			}

			void Close() override {
				if (Stream == nullptr) {
					return;
				}
				// Paused and *then* destroyed. Destroying a running stream races
				// the callback that is inside this object, and the crash lands
				// in SDL rather than anywhere that names this file.
				SDL_PauseAudioStreamDevice(Stream);
				SDL_DestroyAudioStream(Stream);
				Stream = nullptr;
			}

			bool Open(const DeviceSettings &settings) {
				SDL_AudioSpec spec{};
				spec.format = SDL_AUDIO_F32;
				spec.channels = static_cast<int>(Shape.Channels);
				spec.freq = static_cast<int>(Shape.SampleRate);

				Stream = SDL_OpenAudioDeviceStream(
					SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlDevice::Feed, this
				);
				if (Stream == nullptr) {
					return false;
				}

				(void)settings;
				SDL_ResumeAudioStreamDevice(Stream);
				return true;
			}

		  private:
			// SDL asks for more bytes; this renders them.
			//
			// `additional` is what SDL wants beyond what is already queued, in
			// bytes. Rendering in whole blocks and letting SDL buffer the
			// remainder is simpler and cheaper than honouring an arbitrary
			// byte count, and the stream is what smooths the difference.
			static void SDLCALL Feed(void *self, SDL_AudioStream *stream, int additional, int /*total*/) {
				if (additional <= 0) {
					return;
				}
				// SDL's own thread, which no frame scope reaches. Without this
				// the mixer's buffers are untagged and a leak in them looks like
				// a leak in the program's static initialisers.
				ENGINE_HEAP_SCOPE("audio.feed");

				auto *device = static_cast<SdlDevice *>(self);

				const int frameBytes =
					static_cast<int>(sizeof(float)) * static_cast<int>(device->Shape.Channels);
				int remaining = additional;

				while (remaining > 0) {
					device->Engine.Render(device->Block);
					const std::span<const float> samples = device->Block.Data();
					const int bytes = static_cast<int>(samples.size() * sizeof(float));

					SDL_PutAudioStreamData(stream, samples.data(), bytes);
					device->Frames.fetch_add(device->Block.Frames(), std::memory_order_relaxed);
					remaining -= bytes;

					if (frameBytes <= 0) {
						break;
					}
				}
			}

			AudioFormat Shape;
			AudioMixer Engine;
			SampleBuffer Block;

			SDL_AudioStream *Stream = nullptr;

			// Written by the callback and read by anyone. Relaxed: it is a
			// counter for a readout, and nothing branches on it.
			std::atomic<uint64_t> Frames{0};
		};
	}

	std::unique_ptr<NullDevice> OpenNullDevice(const DeviceSettings &settings) {
		return std::make_unique<SilentDevice>(settings);
	}

	std::unique_ptr<Device> OpenDevice(const DeviceSettings &settings) {
		const AudioFormat shape = settings.Format.IsValid() ? settings.Format : AudioFormat{};
		const size_t block = settings.BlockFrames == 0 ? DEFAULT_BLOCK_FRAMES : settings.BlockFrames;

		if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
			// A container with no sound server, a machine with audio disabled.
			// Ordinary, and not an error: a game that refused to start because
			// it could not make a noise would be worse than one that is quiet.
			ENGINE_INFO("audio: no output available ({}) - running silently", SDL_GetError());
			return nullptr;
		}

		auto device = std::make_unique<SdlDevice>(shape, block);
		if (!device->Open(settings)) {
			ENGINE_INFO("audio: could not open an output ({}) - running silently", SDL_GetError());
			return nullptr;
		}

		ENGINE_INFO("audio: {} Hz, {} channels, {} frames a block", shape.SampleRate, shape.Channels, block);
		return device;
	}
}
