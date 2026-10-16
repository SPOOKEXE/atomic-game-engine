#pragma once

#include "GpuQueueBatch.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(MONO_NGFX_SDK)
#include <NGFX_GPUTrace_Vulkan.h>
#endif

namespace {
	// The injected profiler starts after residency warm-up. It owns its trace files.
	class RecordingGpuCapture {
	  public:
		explicit RecordingGpuCapture(SDL_GPUDevice *device) {
			if (!std::getenv("MONO_RENDER_RECORDING_CAPTURE") || Captured) return;
#if defined(MONO_NGFX_SDK)
			const auto *api = static_cast<const AtomicGpuQueuesV1 *>(SDL_GetPointerProperty(
				SDL_GetGPUDeviceProperties(device), ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr
			));
			if (!api)
				throw std::runtime_error("GPU Trace capture requires the private Vulkan queue descriptor");
			Queue = api->Queue[ATOMIC_GPU_QUEUE_GRAPHICS];
			NGFX_GPUTrace_InitializeActivity_Vulkan_Params initialize{};
			initialize.version = NGFX_GPUTrace_InitializeActivity_Vulkan_Params_VER;
			Check(NGFX_GPUTrace_InitializeActivity_Vulkan(&initialize), "initialize");
			NGFX_GPUTrace_ActivateTrace_Vulkan_Params activate{};
			activate.version = NGFX_GPUTrace_ActivateTrace_Vulkan_Params_VER;
			activate.queue = Queue;
			Check(NGFX_GPUTrace_ActivateTrace_Vulkan(&activate), "activate");
			NGFX_GPUTrace_StartTrace_Vulkan_Params start{};
			start.version = NGFX_GPUTrace_StartTrace_Vulkan_Params_VER;
			Check(NGFX_GPUTrace_StartTrace_Vulkan(&start), "start");
			Active = true;
			Captured = true;
#else
			(void)device;
			throw std::runtime_error("GPU Trace capture requires MONO_NSIGHT_SDK_ROOT at configure time");
#endif
		}
		~RecordingGpuCapture() {
			try {
				Stop();
			} catch (const std::exception &error) {
				std::cerr << error.what() << '\n';
			}
		}
		void Stop() {
#if defined(MONO_NGFX_SDK)
			if (!Active) return;
			NGFX_GPUTrace_StopTrace_Vulkan_Params stop{};
			stop.version = NGFX_GPUTrace_StopTrace_Vulkan_Params_VER;
			stop.queue = Queue;
			stop.flags = NGFX_GPUTrace_StopTraceFlag_ImmediateCollection;
			Active = false;
			Check(NGFX_GPUTrace_StopTrace_Vulkan(&stop), "stop");
			std::cout << "render-recording-capture sdk=nsight status=collected "
						 "physical_overlap=inspect_gpu_trace\n";
#endif
		}

	  private:
		inline static bool Captured = false;
#if defined(MONO_NGFX_SDK)
		bool Active = false;
		VkQueue Queue = VK_NULL_HANDLE;
		static void Check(NGFX_Result result, const char *operation) {
			if (result != NGFX_Result_Success)
				throw std::runtime_error(
					std::string("GPU Trace ") + operation + " failed: " + std::to_string(result)
				);
		}
#endif
	};
}
