#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxCollectionFiles.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <miniz.h>
#include <new>

namespace engine::imagegraphexport {
	namespace {
		using namespace imagegraph;
		constexpr uint64_t CodecBytes = 1024 * 1024;
		constexpr std::array<std::byte, 8> PngSignature{
			std::byte{137},
			std::byte{80},
			std::byte{78},
			std::byte{71},
			std::byte{13},
			std::byte{10},
			std::byte{26},
			std::byte{10}
		};
		bool Fail(Diagnostic &diagnostic, Status status, std::string_view message) {
			diagnostic = {status, {}, {}, std::string(message)};
			return false;
		}
		// grug vendor heap shares one fixed reservation, including transient realloc copies.
		struct CodecHeap {
			struct alignas(std::max_align_t) Block {
				size_t Bytes;
			};
			size_t Remaining = CodecBytes;
			static void *Allocate(void *opaque, size_t count, size_t width) noexcept {
				auto &heap = *static_cast<CodecHeap *>(opaque);
				if (!width || count > (SIZE_MAX - sizeof(Block)) / width) return nullptr;
				const size_t bytes = count * width + sizeof(Block);
				if (bytes > heap.Remaining) return nullptr;
				auto *block = static_cast<Block *>(std::malloc(bytes));
				if (!block) return nullptr;
				block->Bytes = bytes;
				heap.Remaining -= bytes;
				return block + 1;
			}
			static void Release(void *opaque, void *address) noexcept {
				if (!address) return;
				auto *block = static_cast<Block *>(address) - 1;
				static_cast<CodecHeap *>(opaque)->Remaining += block->Bytes;
				std::free(block);
			}
			static void *Resize(void *opaque, void *address, size_t count, size_t width) noexcept {
				void *replacement = Allocate(opaque, count, width);
				if (!replacement) return nullptr;
				if (address) {
					const auto *block = static_cast<Block *>(address) - 1;
					std::memcpy(replacement, address, std::min(block->Bytes - sizeof(Block), count * width));
					Release(opaque, address);
				}
				return replacement;
			}
		};
		bool Admit(uint64_t &remaining, uint64_t bytes) {
			if (bytes > remaining) return false;
			remaining -= bytes;
			return true;
		}
		void Word(std::vector<std::byte> &bytes, uint32_t word) {
			for (int shift = 24; shift >= 0; shift -= 8)
				bytes.push_back(std::byte(word >> shift));
		}
		void Chunk(std::vector<std::byte> &bytes, std::string_view type, std::span<const std::byte> payload) {
			Word(bytes, uint32_t(payload.size()));
			const size_t start = bytes.size();
			for (char c : type)
				bytes.push_back(std::byte(c));
			bytes.insert(bytes.end(), payload.begin(), payload.end());
			Word(
				bytes,
				uint32_t(mz_crc32(
					0, reinterpret_cast<const unsigned char *>(bytes.data() + start), payload.size() + 4
				))
			);
		}
		struct Deflater {
			mz_stream Stream{};
			bool Open = false;
			~Deflater() {
				if (Open) mz_deflateEnd(&Stream);
			}
		};
		struct ZipWriter {
			mz_zip_archive Archive{};
			bool Open = false;
			~ZipWriter() {
				if (Open) mz_zip_writer_end(&Archive);
			}
			static size_t Write(void *opaque, mz_uint64 offset, const void *data, size_t size) noexcept {
				auto &bytes = *static_cast<std::vector<std::byte> *>(opaque);
				if (offset > bytes.size() || size > bytes.size() - offset) return 0;
				std::memcpy(bytes.data() + size_t(offset), data, size);
				return size;
			}
		};
	}
	bool WritePxcxCollectionPreview(
		const Image &preview, std::vector<std::byte> &out, Diagnostic &diagnostic, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph collection preview encode");
		diagnostic = {};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return Fail(diagnostic, Status::LimitExceeded, "collection preview allowance is outside bounds");
		if (!ValidSurfaceLayout(preview, Limits::MaximumDimension, Limits::MaximumOutputBytes))
			return Fail(diagnostic, Status::InvalidValue, "collection preview layout is invalid");
		const uint64_t rowBytes = uint64_t(preview.Width) * 4 + 1;
		const uint64_t rawBytes = rowBytes * preview.Height;
		if (rawBytes > UINT32_MAX)
			return Fail(diagnostic, Status::LimitExceeded, "collection PNG scanlines exceed codec bounds");
		const uint64_t encodedBound = mz_compressBound(mz_ulong(rawBytes));
		uint64_t remaining = maximumBytes;
		if (!Admit(remaining, preview.Pixels.capacity()) || !Admit(remaining, out.capacity()) ||
			!Admit(remaining, rawBytes) || !Admit(remaining, encodedBound * 2 + 57) ||
			!Admit(remaining, CodecBytes + 1024))
			return Fail(diagnostic, Status::LimitExceeded, "collection PNG staging exceeds allowance");
		if (!FiniteSurfaceSamples(preview))
			return Fail(diagnostic, Status::InvalidValue, "collection preview contains nonfinite samples");
		std::vector<std::byte> rows(size_t(rawBytes), std::byte{});
		Image pixel{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::RGBA8Unorm};
		for (uint32_t y = 0; y < preview.Height; ++y)
			for (uint32_t x = 0; x < preview.Width; ++x) {
				SurfacePixel sample{};
				if (!LoadSurfacePixel(preview, x, y, sample) || !StoreSurfacePixel(pixel, 0, 0, sample))
					return Fail(diagnostic, Status::InvalidValue, "collection preview conversion failed");
				std::memcpy(rows.data() + size_t(y * rowBytes + 1 + uint64_t(x) * 4), pixel.Pixels.data(), 4);
			}
		std::vector<std::byte> compressed(size_t(encodedBound), std::byte{});
		CodecHeap heap;
		Deflater compressor;
		compressor.Stream.zalloc = CodecHeap::Allocate;
		compressor.Stream.zfree = CodecHeap::Release;
		compressor.Stream.opaque = &heap;
		if (mz_deflateInit(&compressor.Stream, MZ_NO_COMPRESSION) != MZ_OK)
			return Fail(diagnostic, Status::LimitExceeded, "collection PNG codec allocation failed");
		compressor.Open = true;
		compressor.Stream.next_in = reinterpret_cast<const unsigned char *>(rows.data());
		compressor.Stream.avail_in = unsigned(rawBytes);
		compressor.Stream.next_out = reinterpret_cast<unsigned char *>(compressed.data());
		compressor.Stream.avail_out = unsigned(encodedBound);
		if (mz_deflate(&compressor.Stream, MZ_FINISH) != MZ_STREAM_END)
			return Fail(diagnostic, Status::LimitExceeded, "collection PNG codec exceeded staging");
		compressed.resize(compressor.Stream.total_out);
		std::vector<std::byte> candidate;
		candidate.reserve(size_t(encodedBound + 57));
		candidate.insert(candidate.end(), PngSignature.begin(), PngSignature.end());
		std::vector<std::byte> header;
		Word(header, preview.Width);
		Word(header, preview.Height);
		header.insert(header.end(), {std::byte{8}, std::byte{6}, std::byte{}, std::byte{}, std::byte{}});
		Chunk(candidate, "IHDR", header);
		Chunk(candidate, "IDAT", compressed);
		Chunk(candidate, "IEND", {});
		if (rows.capacity() > rawBytes || compressed.capacity() > encodedBound ||
			candidate.capacity() > encodedBound + 57)
			return Fail(diagnostic, Status::LimitExceeded, "collection PNG backing exceeds admission");
		core::Metrics::Count("imagegraph.collection_preview.encoded_bytes", double(candidate.size()));
		out = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		return Fail(diagnostic, Status::LimitExceeded, "collection preview allocation failed");
	}
	bool WritePxcxCollectionPackage(
		std::string_view baseName,
		const imagegraphio::PxcxCollectionSave &collection,
		std::span<const std::byte> previewPng,
		std::vector<std::byte> &out,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph collection package encode");
		diagnostic = {};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return Fail(diagnostic, Status::LimitExceeded, "collection package allowance is outside bounds");
		if (baseName.empty() || baseName.size() > 1024 || baseName == "." || baseName == ".." ||
			std::any_of(baseName.begin(), baseName.end(), [](unsigned char c) {
				return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':';
			}))
			return Fail(diagnostic, Status::InvalidValue, "collection package basename is invalid");
		if (collection.GraphJson.empty() ||
			collection.GraphJson.size() > bake::PxcxLimits::MaximumGraphJsonBytes ||
			(collection.MetadataJson &&
			 collection.MetadataJson->size() > bake::PxcxLimits::MaximumMetadataBytes) ||
			previewPng.size() > Limits::MaximumOutputBytes)
			return Fail(diagnostic, Status::LimitExceeded, "collection package input exceeds format bounds");
		if (!previewPng.empty() &&
			(previewPng.size() < PngSignature.size() ||
			 !std::equal(PngSignature.begin(), PngSignature.end(), previewPng.begin())))
			return Fail(diagnostic, Status::InvalidValue, "collection package preview is not encoded PNG");
		const uint64_t payload = collection.GraphJson.size() +
								 (collection.MetadataJson ? collection.MetadataJson->size() : 0) +
								 previewPng.size();
		const uint64_t encodedBound = payload + 3 * (256 + 2 * (baseName.size() + 6)) + 22;
		uint64_t remaining = maximumBytes;
		if (!Admit(remaining, collection.GraphJson.capacity()) ||
			!Admit(remaining, collection.MetadataJson ? collection.MetadataJson->capacity() : 0) ||
			!Admit(remaining, previewPng.size()) || !Admit(remaining, out.capacity()) ||
			!Admit(remaining, encodedBound) || !Admit(remaining, 6 * (baseName.size() + 6)) ||
			!Admit(remaining, CodecBytes))
			return Fail(diagnostic, Status::LimitExceeded, "collection ZIP staging exceeds allowance");
		std::vector<std::byte> candidate(size_t(encodedBound), std::byte{});
		CodecHeap heap;
		ZipWriter writer;
		writer.Archive.m_pAlloc = CodecHeap::Allocate;
		writer.Archive.m_pFree = CodecHeap::Release;
		writer.Archive.m_pRealloc = CodecHeap::Resize;
		writer.Archive.m_pAlloc_opaque = &heap;
		writer.Archive.m_pWrite = ZipWriter::Write;
		writer.Archive.m_pIO_opaque = &candidate;
		if (!mz_zip_writer_init(&writer.Archive, 0))
			return Fail(diagnostic, Status::LimitExceeded, "collection ZIP allocation failed");
		writer.Open = true;
		const auto add = [&](std::string_view extension, const void *data, size_t size) {
			const std::string name = std::string(baseName) + std::string(extension);
			return mz_zip_writer_add_mem(&writer.Archive, name.c_str(), data, size, MZ_NO_COMPRESSION) != 0;
		};
		if ((!previewPng.empty() && !add(".png", previewPng.data(), previewPng.size())) ||
			!add(".pxcc", collection.GraphJson.data(), collection.GraphJson.size()) ||
			(collection.MetadataJson &&
			 !add(".meta", collection.MetadataJson->data(), collection.MetadataJson->size())) ||
			!mz_zip_writer_finalize_archive(&writer.Archive))
			return Fail(diagnostic, Status::LimitExceeded, "collection ZIP encoding exceeded staging");
		candidate.resize(size_t(writer.Archive.m_archive_size));
		if (candidate.capacity() > encodedBound)
			return Fail(diagnostic, Status::LimitExceeded, "collection ZIP backing exceeds admission");
		core::Metrics::Count("imagegraph.collection_package.encoded_bytes", double(candidate.size()));
		out = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		return Fail(diagnostic, Status::LimitExceeded, "collection package allocation failed");
	}
}
