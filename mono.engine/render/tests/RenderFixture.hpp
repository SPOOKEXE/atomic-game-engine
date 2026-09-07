#pragma once

// Real-device capture for small deterministic fixtures. Kept with the tests so
// capture waits and failure encoding cannot enter a shipped frame.

#include "GpuHeap.hpp"
#include "RenderImageComparison.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Version.hpp>
#include <engine/render/Renderer.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace engine::render::test {

	struct CapturedImage {
		uint32_t Width = 0;
		uint32_t Height = 0;
		ImageFormat Format = ImageFormat::Rgba8Unorm;
		size_t RowStrideBytes = 0;
		std::vector<std::byte> Bytes;

		ImageView View() const {
			return {Width, Height, Format, Bytes, RowStrideBytes};
		}
	};

	struct FixtureDevice {
		bool VideoReady = SDL_Init(SDL_INIT_VIDEO);
		Renderer Render;

		~FixtureDevice() {
			Render.Shutdown();
			if (VideoReady) {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		}

		void Initialise() {
			REQUIRE(VideoReady);
			INFO(SDL_GetError());
			REQUIRE(Render.Initialise(nullptr));
			REQUIRE(Render.IsHeadless());
		}
	};

	// Copies one declared four-byte graph image after its producer submission.
	// The padded rows deliberately exercise transfer pitch at odd image widths.
	// The fixture must keep this resource live through the final graph write to prevent alias overwrite.
	inline CapturedImage CaptureResource(
		Renderer &renderer,
		core::Name resource,
		size_t slot,
		uint32_t width,
		uint32_t height,
		ImageFormat format
	) {
		REQUIRE(width > 0);
		REQUIRE(height > 0);
		REQUIRE(width <= 2048);
		REQUIRE(height <= 2048);
		REQUIRE(format != ImageFormat::Rgba32Float);
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(resource, slot));
		REQUIRE(device != nullptr);
		REQUIRE(texture != nullptr);

		CapturedImage image;
		image.Width = width;
		image.Height = height;
		image.Format = format;
		image.RowStrideBytes = (static_cast<size_t>(width) * 4 + 255) / 256 * 256;
		image.Bytes.resize(image.RowStrideBytes * height);
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		transferInfo.size = static_cast<uint32_t>(image.Bytes.size());
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
			gpu::ReleaseTransferBuffer(device, transfer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			gpu::CreateTransferBuffer(device, &transferInfo), releaseTransfer
		);
		REQUIRE(transfer != nullptr);
		const auto cancelCommand = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
			SDL_AcquireGPUCommandBuffer(device), cancelCommand
		);
		REQUIRE(command != nullptr);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy != nullptr);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer.get();
		destination.pixels_per_row = static_cast<uint32_t>(image.RowStrideBytes / 4);
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence != nullptr);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline) {
			SDL_Delay(1);
		}
		INFO("capture timeout for " << resource.Text());
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
		REQUIRE(mapped != nullptr);
		for (uint32_t row = 0; row < height; row++) {
			const size_t offset = row * image.RowStrideBytes;
			std::memcpy(
				image.Bytes.data() + offset, static_cast<const std::byte *>(mapped) + offset, width * 4
			);
		}
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return image;
	}

	inline void WriteImageBytes(const std::filesystem::path &path, std::span<const std::byte> bytes) {
		std::ofstream output(path, std::ios::binary);
		output.write(
			reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
		);
		REQUIRE(output.good());
	}

	// Raw files retain precision. PPM previews make byte-colour differences easy to inspect.
	inline void
	WriteImagePreview(const std::filesystem::path &path, const ImageView &image, double scale = 1) {
		const auto stride = image_comparison_detail::ValidStride(image);
		REQUIRE(stride.has_value());
		std::ofstream output(path, std::ios::binary);
		output << "P6\n" << image.Width << ' ' << image.Height << "\n255\n";
		for (uint32_t y = 0; y < image.Height; y++) {
			for (uint32_t x = 0; x < image.Width; x++) {
				const auto *pixel =
					image.Bytes.data() + y * *stride + x * image_comparison_detail::PixelBytes(image.Format);
				for (size_t channel = 0; channel < 3; channel++) {
					const size_t sampledChannel =
						image_comparison_detail::Channels(image.Format) == 1 ? 0 : channel;
					const double sample =
						image_comparison_detail::Sample(pixel, image.Format, sampledChannel);
					const uint8_t encoded = static_cast<uint8_t>(
						std::isfinite(sample) ? std::clamp(sample * scale, 0.0, 1.0) * 255 : 255
					);
					output.write(reinterpret_cast<const char *>(&encoded), 1);
				}
			}
		}
		REQUIRE(output.good());
	}

	inline void CheckImage(
		Renderer &renderer,
		std::string_view fixture,
		std::string_view resource,
		std::string_view inputs,
		const ImageView &expected,
		const ImageView &actual,
		const ImageTolerance &tolerance = {}
	) {
		const ImageComparison comparison = CompareImages(expected, actual, tolerance);
		INFO(
			fixture << '/' << resource << ": mismatched=" << comparison.MismatchedPixels << " max="
					<< comparison.MaximumAbsoluteError << " rmse=" << comparison.RootMeanSquareError
		);
		if (comparison.Passed()) {
			return;
		}
		const auto directory =
			std::filesystem::path(SDL_GetBasePath()) / "render-failures" / fixture / resource;
		std::filesystem::create_directories(directory);
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device);
		std::ofstream manifest(directory / "manifest.txt");
		manifest
			<< "fixture=" << fixture << "\nresource=" << resource << "\nversion=" << core::Version()
			<< "\nbackend=" << renderer.BackendName() << "\ndevice="
			<< SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable")
			<< "\ndriver="
			<< SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unavailable")
			<< "\nwidth=" << expected.Width << "\nheight=" << expected.Height
			<< "\nexpected_format=" << static_cast<int>(expected.Format)
			<< "\nactual_format=" << static_cast<int>(actual.Format)
			<< "\ncomparison_status=" << static_cast<int>(comparison.Status)
			<< "\nnonfinite_samples=" << comparison.NonFiniteSamples
			<< "\nmean_absolute_error=" << comparison.MeanAbsoluteError
			<< "\nallowed_outliers=" << tolerance.AllowedMismatchedPixels
			<< "\nallowed_rmse=" << tolerance.MaximumRootMeanSquareError
			<< "\nexpected_stride=" << expected.RowStrideBytes << "\nactual_stride=" << actual.RowStrideBytes
			<< "\nexpected_hash=" << assets::Hasher::Of(expected.Bytes).ToHex()
			<< "\nactual_hash=" << assets::Hasher::Of(actual.Bytes).ToHex()
			<< "\nabsolute_tolerance=" << tolerance.Absolute << "\nrelative_tolerance=" << tolerance.Relative
			<< "\nmax_error=" << comparison.MaximumAbsoluteError
			<< "\nrmse=" << comparison.RootMeanSquareError << "\nmismatched=" << comparison.MismatchedPixels
			<< "\ninputs:\n"
			<< inputs;
		if (const char *revision = std::getenv("MONO_RENDER_REVISION")) {
			manifest << "\nrevision=" << revision;
		}
		if (comparison.MismatchRegion) {
			const auto &region = *comparison.MismatchRegion;
			manifest << "\nmismatch_region=" << region.X << ',' << region.Y << ',' << region.Width << ','
					 << region.Height;
		}
		for (const char *name : {"opaque.vert.spv", "gbuffer.frag.spv", "depth-linearise.frag.spv"}) {
			const auto shaderPath = std::filesystem::path(SDL_GetBasePath()) / "shaders/resources" / name;
			std::ifstream shader(shaderPath, std::ios::binary);
			if (!shader) {
				manifest << "\nshader=" << name << " unavailable";
				continue;
			}
			const std::vector<char> bytes{
				std::istreambuf_iterator<char>(shader), std::istreambuf_iterator<char>()
			};
			manifest << "\nshader=" << name << ' '
					 << assets::Hasher::Of(std::as_bytes(std::span(bytes))).ToHex();
		}
		manifest.flush();
		REQUIRE(manifest.good());
		WriteImageBytes(directory / "expected.raw", expected.Bytes);
		WriteImageBytes(directory / "actual.raw", actual.Bytes);
		double scale = 1;
		if (expected.Format == ImageFormat::R32Float) {
			const auto stride = image_comparison_detail::ValidStride(expected);
			double maximum = 1;
			if (stride) {
				for (uint32_t y = 0; y < expected.Height; y++) {
					for (uint32_t x = 0; x < expected.Width; x++) {
						maximum = std::max(
							maximum,
							image_comparison_detail::Sample(
								expected.Bytes.data() + y * *stride + x * 4, expected.Format, 0
							)
						);
					}
				}
			}
			scale = 1 / maximum;
		}
		if (image_comparison_detail::ValidStride(expected)) {
			WriteImagePreview(directory / "expected.ppm", expected, scale);
		}
		if (image_comparison_detail::ValidStride(actual)) {
			WriteImagePreview(directory / "actual.ppm", actual, scale);
		}
		std::vector<uint32_t> differences(static_cast<size_t>(expected.Width) * expected.Height, 0xFF000000u);
		for (uint32_t y = 0; y < expected.Height; y++) {
			for (uint32_t x = 0; x < expected.Width; x++) {
				ImageTolerance probe = tolerance;
				probe.AllowedMismatchedPixels = 0;
				probe.Region = PixelRegion{x, y, 1, 1};
				if (!CompareImages(expected, actual, probe).Passed()) {
					differences[static_cast<size_t>(y) * expected.Width + x] = 0xFF0000FFu;
				}
			}
		}
		WriteImagePreview(
			directory / "difference.ppm",
			{expected.Width,
			 expected.Height,
			 ImageFormat::Rgba8Unorm,
			 std::as_bytes(std::span(differences)),
			 0}
		);
		INFO("failure artifacts: " << directory.string());
		CHECK(comparison.Passed());
	}
}
