// Bounded asynchronous noise grids.
//
// This is a typed batch instead of an off-thread callback. A callback captures
// VM state and arbitrary world handles, none of which may be read by a worker
// thread or serialized into a child process. Six scalars and a context name are
// copied into `ComputeJobs`; the heartbeat later resumes with one flat array in
// row-major order.

#include <engine/script/ServiceSurface.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace engine::script {

	namespace {
		uint32_t Dimension(ScriptCall &call, size_t index, const char *message) {
			const double value = call.AsNumber(index);
			if (!std::isfinite(value) || value < 1.0 || std::floor(value) != value ||
				value > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
				call.Raise(message);
			}
			return static_cast<uint32_t>(value);
		}

		parallel::JobContext ContextOf(ScriptCall &call, size_t index) {
			if (call.IsNil(index)) {
				return parallel::JobContext::Threaded;
			}
			const std::string context = call.AsString(index);
			if (context == "Serial") {
				return parallel::JobContext::Serial;
			}
			if (context == "Threaded") {
				return parallel::JobContext::Threaded;
			}
			if (context == "Processed") {
				return parallel::JobContext::Processed;
			}
			call.Raise("compute context must be Serial, Threaded or Processed");
			return parallel::JobContext::Threaded;
		}

		void NoiseGridAsync(ScriptCall &call) {
			NoiseGridRequest request;
			request.Width = Dimension(call, 0, "noise grid width must be a positive integer");
			request.Depth = Dimension(call, 1, "noise grid depth must be a positive integer");
			request.OriginX = call.AsNumber(2);
			request.OriginZ = call.AsNumber(3);
			request.Step = call.AsNumber(4);
			request.OriginY = call.IsNil(6) ? 0.0 : call.AsNumber(6);

			ComputeJobs &jobs = call.Computations();
			const uint64_t ticket = jobs.SubmitNoise(request, ContextOf(call, 5));
			if (ticket == 0) {
				call.Raise(jobs.LastError().c_str());
			}
			call.AwaitCompute(ticket);
		}

		constexpr std::array<ServiceMethod, 1> METHODS{{
			{"NoiseGridAsync", NoiseGridAsync},
		}};
	}

	const ServiceSurface &ComputeServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "ComputeService";
			surface.Methods = METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
