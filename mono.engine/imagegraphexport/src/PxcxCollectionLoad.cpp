#include "PxcxCodecHeap.hpp"

#include <engine/bake/Image.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxCollectionLoad.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <miniz.h>
#include <new>
#include <stdexcept>

namespace engine::imagegraphexport {
	namespace {
		using namespace imagegraph;
		bool Fail(Diagnostic &diagnostic, Status status, std::string_view message) {
			diagnostic = {status, {}, "path", std::string(message)};
			return false;
		}
		bool Charge(uint64_t &remaining, uint64_t bytes) {
			if (bytes > remaining) return false;
			remaining -= bytes;
			return true;
		}
		std::optional<uint64_t> ArchiveBytes(const bake::PxcxArchive &archive) {
			uint64_t bytes = sizeof(archive);
			const auto add = [&](uint64_t amount) {
				if (amount > UINT64_MAX - bytes) return false;
				bytes += amount;
				return true;
			};
			if (!add(archive.OriginalBytes.capacity()) || !add(archive.GraphJson.capacity()) ||
				!add(archive.MetadataText.capacity()) || !add(archive.MetadataPayload.capacity()) ||
				!add(archive.ThumbnailRgba.capacity()) ||
				archive.Nodes.capacity() > SIZE_MAX / sizeof(bake::PxcxNodeFact) ||
				archive.Links.capacity() > SIZE_MAX / sizeof(bake::PxcxLinkFact) ||
				!add(archive.Nodes.capacity() * sizeof(bake::PxcxNodeFact)) ||
				!add(archive.Links.capacity() * sizeof(bake::PxcxLinkFact)))
				return std::nullopt;
			for (const auto &node : archive.Nodes)
				if (!add(node.Id.capacity()) || !add(node.Type.capacity())) return std::nullopt;
			for (const auto &link : archive.Links)
				if (!add(link.FromNode.capacity()) || !add(link.ToNode.capacity())) return std::nullopt;
			return bytes;
		}
		bool ReadFile(
			const std::filesystem::path &path,
			uint64_t limit,
			uint64_t &remaining,
			std::vector<std::byte> &bytes,
			Diagnostic &diagnostic,
			bool optional = false,
			bool *present = nullptr
		) {
			if (present) *present = false;
			std::error_code error;
			const auto status = std::filesystem::status(path, error);
			if (optional && (error == std::errc::no_such_file_or_directory ||
							 (!error && !std::filesystem::exists(status))))
				return true;
			if (error || !std::filesystem::is_regular_file(status))
				return Fail(diagnostic, Status::InvalidValue, "source file is not readable regular content");
			if (present) *present = true;
			const auto count = std::filesystem::file_size(path, error);
			if (error || count > limit || !Charge(remaining, count))
				return Fail(diagnostic, Status::LimitExceeded, "source file exceeds granted byte allowance");
			std::ifstream file(path, std::ios::binary);
			if (!file) return Fail(diagnostic, Status::InvalidValue, "could not open source file");
			bytes.resize(size_t(count));
			if (bytes.capacity() > count && !Charge(remaining, bytes.capacity() - count))
				return Fail(diagnostic, Status::LimitExceeded, "source file backing exceeds allowance");
			if (count) file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(count));
			if (!file || file.peek() != std::char_traits<char>::eof())
				return Fail(diagnostic, Status::InvalidValue, "source file changed during bounded read");
			core::Metrics::Count("imagegraph.source_read.bytes", double(count));
			return true;
		}
		struct ZipReader {
			mz_zip_archive Archive{};
			bool Open = false;
			~ZipReader() {
				if (Open) mz_zip_reader_end(&Archive);
			}
		};
		bool DecodePreview(
			std::span<const std::byte> bytes, Image &preview, uint64_t &remaining, Diagnostic &diagnostic
		) {
			if (bytes.empty()) return Fail(diagnostic, Status::InvalidValue, "collection preview is empty");
			constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
			if (bytes.size() < 33 ||
				!std::equal(signature.begin(), signature.end(), bytes.begin(), [](uint8_t a, std::byte b) {
					return a == std::to_integer<uint8_t>(b);
				}))
				return Fail(diagnostic, Status::InvalidValue, "collection preview is not PNG");
			const auto word = [&](size_t at) {
				uint32_t n = 0;
				for (size_t i = 0; i < 4; ++i)
					n = (n << 8) | std::to_integer<uint8_t>(bytes[at + i]);
				return n;
			};
			const uint32_t width = word(16), height = word(20);
			if (!width || !height || width > Limits::MaximumDimension || height > Limits::MaximumDimension)
				return Fail(
					diagnostic, Status::LimitExceeded, "collection preview dimensions exceed native bounds"
				);
			const uint64_t pixels = uint64_t(width) * height * 4;
			if (pixels > Limits::MaximumOutputBytes ||
				!Charge(remaining, pixels * 4 + bytes.size() * 2 + 1024 * 1024))
				return Fail(
					diagnostic, Status::LimitExceeded, "collection preview decode staging exceeds allowance"
				);
			engine::assets::TextureData decoded;
			std::string failure;
			if (!bake::ReadImage(bytes, decoded, failure) || decoded.Width != width ||
				decoded.Height != height || decoded.Pixels.size() != pixels) {
				diagnostic = {
					Status::InvalidValue,
					{},
					"path",
					failure.empty() ? "collection preview decoded dimensions are invalid" : std::move(failure)
				};
				return false;
			}
			preview = {width, height, std::vector<uint8_t>(pixels), 0, SurfaceFormat::RGBA8Unorm};
			for (size_t i = 0; i < pixels; ++i)
				preview.Pixels[i] = std::to_integer<uint8_t>(decoded.Pixels[i]);
			preview.Hash = SurfaceHash(preview);
			return true;
		}
	}
	bool ReadPxcxSourceFile(
		const std::filesystem::path &path,
		const TimelineSettings &timeline,
		PxcxSourceRead &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		bool readPreview
	) try {
		ENGINE_PROFILE("imagegraph source file read");
		diagnostic = {};
		if (path.empty()) return Fail(diagnostic, Status::InvalidValue, "enter a source file path");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || path.native().size() > 4096)
			return Fail(diagnostic, Status::LimitExceeded, "source read path or allowance exceeds bounds");
		uint64_t remaining = maximumBytes;
		const auto held = ArchiveBytes(result.Archive);
		if (!held || !Charge(remaining, *held) || !Charge(remaining, result.Preview.Pixels.capacity()) ||
			(result.Collection && (!Charge(remaining, result.Collection->GraphJson.capacity()) ||
								   (result.Collection->MetadataJson &&
									!Charge(remaining, result.Collection->MetadataJson->capacity())))))
			return Fail(diagnostic, Status::LimitExceeded, "source read prior result exceeds allowance");
		std::vector<std::byte> bytes;
		if (!ReadFile(path, bake::PxcxLimits::MaximumArchiveBytes, remaining, bytes, diagnostic))
			return false;
		PxcxSourceRead candidate;
		if (bytes.size() >= 4 && std::memcmp(bytes.data(), "PXCX", 4) == 0) {
			if (!Charge(remaining, 2ull * bake::PxcxLimits::MaximumGraphJsonBytes + 2 * 1024 * 1024))
				return Fail(diagnostic, Status::LimitExceeded, "PXCX readback staging exceeds allowance");
			std::string failure;
			if (!bake::ReadPxcx(bytes, candidate.Archive, failure)) {
				diagnostic = {Status::InvalidValue, {}, "path", std::move(failure)};
				return false;
			}
			const auto decodedBytes = ArchiveBytes(candidate.Archive);
			if (!decodedBytes || !Charge(remaining, *decodedBytes))
				return Fail(diagnostic, Status::LimitExceeded, "PXCX decoded backing exceeds allowance");
			result = std::move(candidate);
			return true;
		}
		candidate.Collection.emplace();
		auto &files = *candidate.Collection;
		std::vector<std::byte> png;
		bool previewPresent = false;
		const auto extension = path.extension();
		if (extension == ".pxcc") {
			if (bytes.empty() || bytes.size() > bake::PxcxLimits::MaximumGraphJsonBytes ||
				!Charge(remaining, bytes.size() + 1))
				return Fail(diagnostic, Status::LimitExceeded, "collection text exceeds allowance");
			files.GraphJson.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			auto sidecar = path;
			sidecar.replace_extension(".meta");
			std::vector<std::byte> metadata;
			bool metadataPresent = false;
			if (!ReadFile(
					sidecar,
					bake::PxcxLimits::MaximumMetadataBytes,
					remaining,
					metadata,
					diagnostic,
					true,
					&metadataPresent
				))
				return false;
			if (metadataPresent) {
				if (metadata.empty())
					return Fail(diagnostic, Status::InvalidValue, "collection metadata is empty");
				if (!Charge(remaining, metadata.size() + 1))
					return Fail(diagnostic, Status::LimitExceeded, "collection metadata exceeds allowance");
				files.MetadataJson.emplace(reinterpret_cast<const char *>(metadata.data()), metadata.size());
			}
			if (readPreview) {
				sidecar.replace_extension(".png");
				if (!ReadFile(
						sidecar, Limits::MaximumOutputBytes, remaining, png, diagnostic, true, &previewPresent
					))
					return false;
			}
		} else if (extension == ".pxz") {
			const auto base = path.stem().string();
			if (base.empty() || base.size() > 1024 ||
				!Charge(remaining, detail::PxcxCodecHeap::MaximumBytes + 16 * 1024))
				return Fail(
					diagnostic, Status::LimitExceeded, "collection ZIP names or staging exceed allowance"
				);
			detail::PxcxCodecHeap heap;
			ZipReader reader;
			reader.Archive.m_pAlloc = detail::PxcxCodecHeap::Allocate;
			reader.Archive.m_pFree = detail::PxcxCodecHeap::Release;
			reader.Archive.m_pRealloc = detail::PxcxCodecHeap::Resize;
			reader.Archive.m_pAlloc_opaque = &heap;
			if (!mz_zip_reader_init_mem(&reader.Archive, bytes.data(), bytes.size(), 0))
				return Fail(
					diagnostic,
					Status::InvalidValue,
					"collection ZIP directory is invalid or exceeds codec bounds"
				);
			reader.Open = true;
			const auto count = mz_zip_reader_get_num_files(&reader.Archive);
			if (!count || count > 3)
				return Fail(diagnostic, Status::LimitExceeded, "collection ZIP entry count exceeds bounds");
			std::array<bool, 3> seen{};
			for (mz_uint index = 0; index < count; ++index) {
				mz_zip_archive_file_stat stat{};
				const auto nameBytes = mz_zip_reader_get_filename(&reader.Archive, index, nullptr, 0);
				if (!nameBytes || nameBytes > 1031 ||
					!mz_zip_reader_file_stat(&reader.Archive, index, &stat) || stat.m_is_directory ||
					stat.m_is_encrypted || (stat.m_method != 0 && stat.m_method != 8))
					return Fail(diagnostic, Status::InvalidValue, "collection ZIP entry is unsupported");
				std::array<char, 1031> name{};
				if (mz_zip_reader_get_filename(&reader.Archive, index, name.data(), name.size()) != nameBytes)
					return Fail(diagnostic, Status::InvalidValue, "collection ZIP name is invalid");
				const std::string_view stored(name.data(), nameBytes - 1);
				const size_t kind = stored == base + ".pxcc"   ? 0
									: stored == base + ".meta" ? 1
									: stored == base + ".png"  ? 2
															   : 3;
				if (kind == 3 || seen[kind])
					return Fail(
						diagnostic, Status::InvalidValue, "collection ZIP has unexpected or duplicate entries"
					);
				seen[kind] = true;
				const uint64_t limit = kind == 0   ? bake::PxcxLimits::MaximumGraphJsonBytes
									   : kind == 1 ? bake::PxcxLimits::MaximumMetadataBytes
												   : Limits::MaximumOutputBytes;
				if (stat.m_comp_size > bake::PxcxLimits::MaximumArchiveBytes || stat.m_uncomp_size > limit)
					return Fail(
						diagnostic, Status::LimitExceeded, "collection ZIP entry exceeds format bounds"
					);
				if (kind == 2 && !readPreview) continue;
				if (!stat.m_uncomp_size)
					return Fail(diagnostic, Status::InvalidValue, "collection ZIP entry is empty");
				if (!Charge(remaining, stat.m_uncomp_size * 2 + 1))
					return Fail(
						diagnostic, Status::LimitExceeded, "collection ZIP payload exceeds allowance"
					);
				std::vector<std::byte> entry(size_t(stat.m_uncomp_size));
				if (!mz_zip_reader_extract_to_mem(&reader.Archive, index, entry.data(), entry.size(), 0))
					return Fail(
						diagnostic,
						Status::InvalidValue,
						"collection ZIP entry checksum or payload is invalid"
					);
				if (kind == 0)
					files.GraphJson.assign(reinterpret_cast<const char *>(entry.data()), entry.size());
				else if (kind == 1)
					files.MetadataJson.emplace(reinterpret_cast<const char *>(entry.data()), entry.size());
				else {
					png = std::move(entry);
					previewPresent = true;
				}
			}
			if (!seen[0])
				return Fail(diagnostic, Status::InvalidValue, "collection ZIP has no matching graph entry");
		} else
			return Fail(diagnostic, Status::InvalidValue, "source file must be PXCX, .pxcc or .pxz");
		if (!imagegraphio::PreparePxcxCollectionLoad(
				files, timeline, candidate.Archive, diagnostic, remaining
			))
			return false;
		const auto archiveHeld = ArchiveBytes(candidate.Archive);
		if (!archiveHeld || !Charge(remaining, *archiveHeld))
			return Fail(diagnostic, Status::LimitExceeded, "checked collection backing exceeds allowance");
		if (readPreview && previewPresent && !DecodePreview(png, candidate.Preview, remaining, diagnostic))
			return false;
		result = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		return Fail(diagnostic, imagegraph::Status::LimitExceeded, "source read allocation failed");
	} catch (const std::length_error &) {
		return Fail(
			diagnostic, imagegraph::Status::LimitExceeded, "source read backing exceeds container bounds"
		);
	} catch (const std::filesystem::filesystem_error &) {
		return Fail(diagnostic, imagegraph::Status::InvalidValue, "source read path is invalid");
	}
}
