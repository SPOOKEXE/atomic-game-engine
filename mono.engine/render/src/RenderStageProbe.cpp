#include "RenderStageProbe.hpp"

#include "GpuHeap.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <SDL3/SDL.h>
#include <glm/packing.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace engine::render {
	namespace {
		uint32_t PixelBytes(SDL_GPUTextureFormat format) {
			switch (format) {
			case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
				return 1;
			case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
				return 2;
			case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM:
			case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
			case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
			case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
			case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
			case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
				return 4;
			case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
				return 8;
			case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT:
				return 16;
			default:
				return 0;
			}
		}
		float Component(const std::byte *pixel, SDL_GPUTextureFormat format, size_t channel) {
			if (format == SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM) {
				uint32_t packed = 0;
				std::memcpy(&packed, pixel, 4);
				return float((packed >> (channel * 10)) & 1023) / 1023.f;
			}

			if (format == SDL_GPU_TEXTUREFORMAT_R16_FLOAT ||
				format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
				uint16_t half = 0;
				std::memcpy(&half, pixel + (format == SDL_GPU_TEXTUREFORMAT_R16_FLOAT ? 0 : channel * 2), 2);
				return glm::unpackHalf2x16(half).x;
			}
			if (format == SDL_GPU_TEXTUREFORMAT_R32_FLOAT || format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
				format == SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT) {
				float value = 0;
				std::memcpy(
					&value, pixel + (format == SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ? channel * 4 : 0), 4
				);
				return value;
			}
			if (format == SDL_GPU_TEXTUREFORMAT_R8_UNORM) channel = 0;
			if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
				format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
				channel = 2 - channel;
			return float(std::to_integer<uint8_t>(pixel[channel])) / 255.f;
		}
	}
	std::string RenderStageProbe::Quote(std::string_view value) {
		constexpr char hex[] = "0123456789abcdef";
		std::string result = "\"";
		for (const unsigned char byte : value) {
			if (byte < 32) {
				result += "\\u00";
				result += hex[byte >> 4];
				result += hex[byte & 15];
			} else {
				if (byte == '"' || byte == '\\') result += '\\';
				result += char(byte);
			}
		}
		return result + '"';
	}
	void RenderStageProbe::Configure() {
		Directory.clear();
		const char *directory = std::getenv("ATOMIC_RENDER_PROBE_DIR");
		if (!directory || !*directory) return;
		First = 0;
		Last = 8;
		ViewSlot = UINT64_MAX;
		for (const auto &[name, destination] :
			 {std::pair{"ATOMIC_RENDER_PROBE_FIRST", &First},
			  std::pair{"ATOMIC_RENDER_PROBE_LAST", &Last},
			  std::pair{"ATOMIC_RENDER_PROBE_VIEW", &ViewSlot}}) {
			if (const char *value = std::getenv(name)) {
				const auto parsed = std::from_chars(value, value + std::strlen(value), *destination);
				if (parsed.ec != std::errc{} || parsed.ptr != value + std::strlen(value)) {
					ENGINE_ERROR("invalid render probe frame range");
					return;
				}
			}
		}
		if (Last < First || Last - First > 1024) {
			ENGINE_ERROR("render probe range must span at most 1025 renderer frames");
			return;
		}
		const auto run = std::chrono::steady_clock::now().time_since_epoch().count();
		Directory = std::filesystem::path(directory) / std::to_string(run);
		std::error_code error;
		std::filesystem::create_directories(Directory, error);
		if (error) {
			ENGINE_ERROR("cannot create render probe directory: {}", error.message());
			Directory.clear();
		}
	}
	bool RenderStageProbe::Enabled(uint64_t frame, uint64_t view) const {
		return !Directory.empty() && frame >= First && frame <= Last &&
			   (view == UINT64_MAX || ViewSlot == UINT64_MAX || view == ViewSlot);
	}
	void RenderStageProbe::Record(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		uint64_t frame,
		const std::string &metadata,
		SDL_GPUTexture *texture,
		uint32_t width,
		uint32_t height,
		SDL_GPUTextureFormat format,
		std::string_view label
	) {
		if (!Enabled(frame)) return;
		ENGINE_PROFILE("render stage probe copy");
		const auto stem = Directory / (std::to_string(frame) + "-" + std::to_string(Sequence++));
		const auto pixelBytes = PixelBytes(format);
		const uint64_t pitch = (uint64_t(width) * pixelBytes + 255) / 256 * 256;
		const uint64_t bytes = pitch * height;
		std::ofstream manifest(stem.string() + ".json");
		manifest << "{" << metadata << ",\"renderer_frame\":" << frame << ",\"format\":" << int(format)
				 << ",\"width\":" << width << ",\"height\":" << height << ",\"row_pitch\":" << pitch;
		const char *reason = !texture	   ? "no texture output"
							 : !pixelBytes ? "unsupported format"
							 : !bytes || bytes > 256 * 1024 * 1024 - PendingBytes || Pending.size() >= 256
								 ? "capture budget"
								 : nullptr;
		if (reason) {
			manifest << ",\"status\":\"" << reason << "\"}\n";
			return;
		}
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = uint32_t(bytes);
		auto *transfer = gpu::CreateTransferBuffer(device, &info);
		if (!transfer) {
			manifest << ",\"status\":\"allocation failed\"}\n";
			return;
		}
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer;
		destination.pixels_per_row = uint32_t(pitch / pixelBytes);
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		Pending.push_back(
			{transfer, stem, std::string(label), width, height, uint32_t(pitch), pixelBytes, format}
		);
		PendingBytes += size_t(bytes);
		core::Metrics::Count("render.stage_probe.download_bytes", bytes);
		core::Metrics::Count("render.stage_probe.snapshots", 1);
		manifest << ",\"status\":\"recorded\",\"layer\":0,\"mip\":0}\n";
	}
	void RenderStageProbe::Flush(SDL_GPUDevice *device) {
		if (Pending.empty()) return;
		if (!std::filesystem::exists(Directory / "index.html")) {
			std::ofstream index(Directory / "index.html");
			index << "<!doctype html><meta charset=utf-8><title>Render stage probe</title>"
					 "<style>body{background:#000;color:#fff;font:14px monospace}a{color:#fff}"
					 "figure{display:inline-block;vertical-align:top;margin:8px}img{width:256px;image-"
					 "rendering:pixelated}</style>"
					 "<p>Render stage snapshots. Labels link to metadata. BMP previews clamp raw channels to "
					 "[0,1]; .bin files retain original values.</p>";
		}

		ENGINE_PROFILE("render stage probe save");
		if (!SDL_WaitForGPUIdle(device)) {
			ENGINE_ERROR("render probe GPU wait failed: {}", SDL_GetError());
			Clear(device);
			return;
		}
		for (auto &snapshot : Pending) {
			const auto *pixels =
				static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, snapshot.Transfer, false));
			if (!pixels) {
				ENGINE_ERROR("render probe map failed: {}", SDL_GetError());
				continue;
			}
			std::ofstream raw(snapshot.Stem.string() + ".bin", std::ios::binary);
			raw.write(reinterpret_cast<const char *>(pixels), size_t(snapshot.Pitch) * snapshot.Height);
			if (!raw) ENGINE_ERROR("render probe raw write failed at {}", snapshot.Stem.string());
			std::vector<uint8_t> displayed(size_t(snapshot.Width) * snapshot.Height * 4, 255);
			for (uint32_t y = 0; y < snapshot.Height; ++y)
				for (uint32_t x = 0; x < snapshot.Width; ++x)
					for (size_t channel = 0; channel < 3; ++channel) {
						const float value = Component(
							pixels + size_t(y) * snapshot.Pitch + size_t(x) * snapshot.PixelBytes,
							snapshot.Format,
							channel
						);
						displayed[(size_t(y) * snapshot.Width + x) * 4 + channel] =
							std::isfinite(value) ? uint8_t(std::clamp(value, 0.f, 1.f) * 255.f)
												 : (channel == 1 ? 0 : 255);
					}
			SDL_Surface *surface = SDL_CreateSurfaceFrom(
				snapshot.Width, snapshot.Height, SDL_PIXELFORMAT_RGBA32, displayed.data(), snapshot.Width * 4
			);
			if (surface) {
				if (!SDL_SaveBMP(surface, (snapshot.Stem.string() + ".bmp").c_str()))
					ENGINE_ERROR("render probe image write failed: {}", SDL_GetError());
				SDL_DestroySurface(surface);
				std::ofstream index(Directory / "index.html", std::ios::app);
				const auto name = snapshot.Stem.filename().string();
				std::string label;
				for (const char byte : snapshot.Label) {
					if (byte == '&')
						label += "&amp;";
					else if (byte == '<')
						label += "&lt;";
					else if (byte == '>')
						label += "&gt;";
					else
						label += byte;
				}
				index << "<figure><a href=\"" << name << ".json\">" << name << " " << label
					  << "</a><br><img loading=\"lazy\" src=\"" << name << ".bmp\"></figure>\n";
			}
			SDL_UnmapGPUTransferBuffer(device, snapshot.Transfer);
		}
		Clear(device);
	}
	void RenderStageProbe::Clear(SDL_GPUDevice *device) {
		for (auto &snapshot : Pending)
			gpu::ReleaseTransferBuffer(device, snapshot.Transfer);
		Pending.clear();
		PendingBytes = 0;
	}
}
