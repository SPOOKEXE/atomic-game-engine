#include "WeatherCapture.hpp"

#include "GpuHeap.hpp"
#include "ViewRecording.hpp"
#include "WeatherFallback.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/weather/Clock.hpp>

#include <algorithm>
#include <array>
#include <bit>

namespace engine::render {
	void ReleaseWeatherCapture(SDL_GPUDevice *device, WeatherCaptureScratch &scratch) {
		for (auto *texture :
			 {scratch.Clouds, scratch.Depth, scratch.Resolved, scratch.Composite, scratch.HardwareDepth})
			if (texture) gpu::ReleaseTexture(device, texture);
		if (scratch.Bindings) gpu::ReleaseBuffer(device, scratch.Bindings);
		scratch = {};
	}

	namespace {
		bool EnsureScratch(
			SDL_GPUDevice *device,
			WeatherCaptureScratch &scratch,
			uint32_t width,
			uint32_t height,
			bool compute,
			SDL_GPUTextureFormat depthFormat,
			bool cropped
		) {
			if (scratch.Width == width && scratch.Height == height && scratch.Composite &&
				(!compute || (scratch.Clouds && scratch.Depth && scratch.Resolved && scratch.Bindings)) &&
				(!cropped || scratch.HardwareDepth))
				return true;
			WeatherCaptureScratch made;
			SDL_GPUTextureCreateInfo image{};
			image.type = SDL_GPU_TEXTURETYPE_2D;
			image.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
			image.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
			if (compute) image.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			image.width = (width + 1) / 2;
			image.height = (height + 1) / 2;
			image.layer_count_or_depth = image.num_levels = 1;
			image.sample_count = SDL_GPU_SAMPLECOUNT_1;
			const auto makeImage = [&] {
				auto *texture = gpu::CreateTexture(device, &image);
				if (texture) core::Metrics::Count("render.weather.capture_scratch_image_creations", 1);
				return texture;
			};
			if (compute) {
				made.Clouds = makeImage();
				made.Depth = makeImage();
				made.Resolved = makeImage();
			}
			image.width = width;
			image.height = height;
			made.Composite = makeImage();
			if (cropped) {
				image.format = depthFormat;
				image.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
				made.HardwareDepth = makeImage();
			}
			SDL_GPUBufferCreateInfo buffer{};
			buffer.size = 32;
			buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
			if (compute) made.Bindings = gpu::CreateBuffer(device, &buffer);
			if (!made.Composite || (cropped && !made.HardwareDepth) ||
				(compute && (!made.Clouds || !made.Depth || !made.Resolved || !made.Bindings))) {
				ReleaseWeatherCapture(device, made);
				return false;
			}
			made.Width = width;
			made.Height = height;
			ReleaseWeatherCapture(device, scratch);
			scratch = made;
			return true;
		}
	}

	bool ViewRecording::RecordCaptureWeather(
		const graph::RunContext &context,
		SDL_GPUTexture *colour,
		SDL_GPUTexture *depth,
		const glm::mat4 &viewProjection,
		const core::CFrame &camera,
		uint32_t width,
		uint32_t height,
		const scene::WorldLighting &lighting,
		uint32_t backingWidth,
		uint32_t backingHeight
	) {
		const auto &environment = lighting.EnvironmentState;
		if (!VisibleWeatherMedium(environment) || !scene::ValidWeatherCompute(environment.Weather) ||
			(environment.Weather.Mode == scene::WeatherMode::Procedural && environment.Weather.Coverage == 0))
			return true;
		if (!colour || !depth || !width || !height) return false;
		const bool cropped =
			(backingWidth && backingWidth != width) || (backingHeight && backingHeight != height);
		auto &scratch = Bank->WeatherCapture;
		const auto found = std::find_if(
			State->WeatherGenerations.begin(),
			State->WeatherGenerations.end(),
			[this](const auto &generation) {
				return generation.World == Request.World && generation.Pipeline == Pipeline->Name;
			}
		);
		const bool resident = found != State->WeatherGenerations.end() &&
							  WeatherProductsReady(&*found, Command) && &lighting == &CurrentLighting;
		std::optional<WeatherGpuInputs> canonical;
		const WeatherGpuInputs *inputs = resident ? found->InputsFor(Command) : nullptr;
		if (!inputs && environment.Weather.Mode == scene::WeatherMode::Procedural) {
			const auto compiled = weather::Compile(environment.Weather);
			const auto grid = WeatherGridOf(environment.Weather);
			const auto time = environment.HasWeatherClock ? weather::WeatherTimeOf(environment.Clock)
							  : compiled
								  ? weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 0)
								  : std::nullopt;
			if (compiled && grid && time) {
				canonical = WeatherGpuInputs{
					WeatherUniformsOf(compiled->Definition, *time, *grid), *compiled, *time, false
				};
				canonical->Uniforms.Origin.w = 0;
				canonical->Uniforms.Bricks = {};
				canonical->Uniforms.Phase.w = 0;
				inputs = &*canonical;
			}
		}
		const bool compute = State->Caps.HasCompute && State->WeatherRaysCompute &&
							 State->WeatherTemporalCompute && State->WeatherReconstructCompute && inputs;
		if (!EnsureScratch(State->Device, scratch, width, height, compute, State->DepthFormat, cropped))
			return false;
		if (cropped) {
			// Legacy portal targets may be larger than their rendered viewport. The
			// ray shader must see this capture's active depth rectangle, not the padding.
			auto *copy = SDL_BeginGPUCopyPass(Command);
			if (!copy) return false;
			const SDL_GPUTextureLocation from{depth, 0, 0, 0, 0, 0};
			const SDL_GPUTextureLocation to{scratch.HardwareDepth, 0, 0, 0, 0, 0};
			SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, true);
			SDL_EndGPUCopyPass(copy);
			depth = scratch.HardwareDepth;
		}
		if (compute) {
			auto *cells = resident ? found->Cells : scratch.Bindings;
			auto *summaries = resident ? found->Summaries : scratch.Bindings;
			auto *pages = resident ? found->Pages : scratch.Bindings;
			auto *previous = resident ? found->PreviousPages : scratch.Bindings;
			if (!resident) {
				// Zero page count selects canonical reconstruction. Initialize every bound
				// placeholder anyway so speculative device reads never see unwritten bytes.
				const std::array<glm::ivec4, 2> empty{};
				if (!RecordWeatherPages(State->Device, Command, scratch.Bindings, empty)) return false;
				core::Metrics::Count("render.weather.capture_canonical_fallbacks", 1);
			}
			const auto &controls = environment.Weather;
			const auto &appearance = environment.CloudLayer;
			const auto light = WeatherLightingOf(lighting);
			WeatherRayUniforms rays;
			rays.InverseViewProjection = glm::inverse(viewProjection);
			rays.ImageExtent = {width, height, 0, 0};
			rays.Eye = {camera.Position.X, camera.Position.Y, camera.Position.Z, 0};
			rays.Minimum = {
				-controls.ExtentMetres.X * .5f,
				controls.BaseHeightMetres,
				-controls.ExtentMetres.Z * .5f,
				controls.CellSizeMetres
			};
			rays.Maximum = {
				controls.ExtentMetres.X * .5f,
				controls.BaseHeightMetres + controls.ThicknessMetres,
				controls.ExtentMetres.Z * .5f,
				0
			};
			rays.Maximum.w = glm::length(glm::vec3(rays.Maximum - rays.Minimum)) +
							 glm::length(glm::vec3(rays.Eye - rays.Minimum));
			rays.ColourExtinction = {
				appearance.Colour.R,
				appearance.Colour.G,
				appearance.Colour.B,
				weather::ExtinctionPerMetre(controls.CondensateScale) * appearance.Density *
					std::clamp(appearance.Cover, 0.0f, 1.0f)
			};
			rays.Sun = light.Direction;
			rays.SunColour = light.Direct;
			rays.Ambient = light.Ambient;
			rays.Quality = {
				std::clamp(environment.CloudVolume.Steps, 2u, 64u) * 4u,
				std::bit_cast<uint32_t>(controls.LodError),
				resident ? found->PreviousPageCount : 0,
				std::bit_cast<uint32_t>(resident ? found->TransitionAlpha : 1.0f)
			};
			rays.Field = inputs->Uniforms;
			if (!RecordWeatherRays(
					Command,
					State->WeatherRaysCompute,
					cells,
					summaries,
					pages,
					previous,
					depth,
					scratch.Clouds,
					scratch.Depth,
					State->OverlaySampler,
					(width + 1) / 2,
					(height + 1) / 2,
					rays,
					resident ? found->Clock.Buffer : nullptr
				))
				return false;
			WeatherReconstructionUniforms reconstruction;
			reconstruction.InverseViewProjection = rays.InverseViewProjection;
			reconstruction.Eye = rays.Eye;
			reconstruction.Limits.x = 0;
			reconstruction.Extents = {width, height, (width + 1) / 2, (height + 1) / 2};
			WeatherTemporalHistory noHistory;
			if (!RecordWeatherReconstruction(
					Command,
					State->WeatherTemporalCompute,
					State->WeatherReconstructCompute,
					colour,
					depth,
					scratch.Clouds,
					scratch.Depth,
					scratch.Resolved,
					scratch.Composite,
					noHistory,
					State->OverlaySampler,
					reconstruction,
					false
				))
				return false;
			Result.ComputeDispatches += 3;
		} else {
			PbrUniforms capture = Uniforms;
			capture.InverseViewProjection = glm::inverse(viewProjection);
			capture.Eye = {camera.Position.X, camera.Position.Y, camera.Position.Z, 0};
			capture.Target = {
				float(width),
				float(height),
				backingWidth ? float(width) / float(backingWidth) : 1,
				backingHeight ? float(height) / float(backingHeight) : 1
			};
			const auto forward = camera.LookVector();
			capture.CameraDepth = {forward.X, forward.Y, forward.Z, -forward.Dot(camera.Position)};
			if (!RecordWeatherFallback(
					context,
					{colour, width, height, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT},
					{depth, width, height, State->DepthFormat},
					{scratch.Composite, width, height, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT},
					{},
					false,
					true,
					false,
					&capture,
					&lighting
				))
				return false;
		}
		auto *copy = SDL_BeginGPUCopyPass(Command);
		if (!copy) return false;
		const SDL_GPUTextureLocation source{scratch.Composite, 0, 0, 0, 0, 0};
		const SDL_GPUTextureLocation destination{colour, 0, 0, 0, 0, 0};
		SDL_CopyGPUTextureToTexture(copy, &source, &destination, width, height, 1, false);
		SDL_EndGPUCopyPass(copy);
		core::Metrics::Count("render.weather.capture_views", 1);
		StageHistoryWrites(context, Command);
		return true;
	}
}
