#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <server/PortalJournal.hpp>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace server {

	namespace {
		constexpr uint32_t MAGIC = 0x504A4E4Cu; // PJNL
		constexpr uint16_t VERSION = 1;
		constexpr size_t MAXIMUM_RECORDS = 4096;
		constexpr size_t MAXIMUM_PAYLOAD = engine::scene::MAXIMUM_PORTAL_BODY_BYTES + 4096;
		constexpr size_t MAXIMUM_TEXT = 256;

		void WriteText(engine::core::ByteWriter &writer, std::string_view text) {
			writer.WriteString(text);
		}
		bool ReadText(engine::core::ByteReader &reader, std::string &out, bool empty = false) {
			const std::string_view text = reader.ReadString();
			if (reader.Failed() || text.size() > MAXIMUM_TEXT || (!empty && text.empty()) ||
				text.find('\0') != std::string_view::npos) {
				return false;
			}
			out.assign(text);
			return true;
		}
		bool ValidFence(const engine::script::PortalTransferFence &fence) {
			return fence.TopologyRevision != 0 && fence.AuthorityEpoch != 0 && fence.PrepareRevision != 0 &&
				   fence.BaselineId != 0 && !fence.BaselineHash.IsZero() && !fence.H.Domain.empty() &&
				   fence.H.Domain.size() <= MAXIMUM_TEXT && fence.H.SourceTick != 0 &&
				   fence.H.DestinationTick != 0;
		}
		void WriteFence(engine::core::ByteWriter &writer, const engine::script::PortalTransferFence &fence) {
			writer.WriteUInt64(fence.TopologyRevision);
			writer.WriteUInt64(fence.AuthorityEpoch);
			writer.WriteUInt64(fence.PrepareRevision);
			writer.WriteUInt64(fence.BaselineId);
			writer.WriteRaw(fence.BaselineHash.Digest.data(), fence.BaselineHash.Digest.size());
			WriteText(writer, fence.H.Domain);
			writer.WriteUInt64(fence.H.SourceTick);
			writer.WriteUInt64(fence.H.DestinationTick);
		}
		bool ReadFence(engine::core::ByteReader &reader, engine::script::PortalTransferFence &fence) {
			fence.TopologyRevision = reader.ReadUInt64();
			fence.AuthorityEpoch = reader.ReadUInt64();
			fence.PrepareRevision = reader.ReadUInt64();
			fence.BaselineId = reader.ReadUInt64();
			if (!reader.ReadRaw(fence.BaselineHash.Digest.data(), fence.BaselineHash.Digest.size()) ||
				!ReadText(reader, fence.H.Domain) || reader.Failed())
				return false;
			fence.H.SourceTick = reader.ReadUInt64();
			fence.H.DestinationTick = reader.ReadUInt64();
			return !reader.Failed() && ValidFence(fence);
		}
		bool ValidDecision(const engine::script::PortalTransferDecision &decision) {
			const auto &receipt = decision.Receipt;
			return !receipt.Id.SourceWorld.empty() && receipt.Id.SourceWorld.size() <= MAXIMUM_TEXT &&
				   receipt.Id.SourceIncarnation != 0 && receipt.Id.Sequence != 0 &&
				   !receipt.DestinationWorld.empty() && receipt.DestinationWorld.size() <= MAXIMUM_TEXT &&
				   receipt.Diagnostic.size() <= MAXIMUM_TEXT &&
				   receipt.Stage == engine::script::PortalTransferStage::Prepared &&
				   ValidFence(receipt.Fence) && decision.Body.Key.IsValid() &&
				   decision.Body.Generation != 0 && !decision.Baseline.empty() &&
				   decision.Baseline.size() <= engine::scene::MAXIMUM_PORTAL_BODY_BYTES &&
				   engine::assets::Hasher::Of(decision.Baseline) == receipt.Fence.BaselineHash;
		}
		bool SameDecision(
			const engine::script::PortalTransferDecision &left,
			const engine::script::PortalTransferDecision &right
		) {
			return left.Receipt.Id == right.Receipt.Id && left.Receipt.Fence == right.Receipt.Fence &&
				   left.Body.Key == right.Body.Key && left.Body.Generation == right.Body.Generation;
		}
		bool Encode(const PortalJournalRecord &record, std::vector<std::byte> &out) {
			if ((record.Outcome != PortalJournalOutcome::Commit &&
				 record.Outcome != PortalJournalOutcome::Completed) ||
				!ValidDecision(record.Decision))
				return false;
			const auto &receipt = record.Decision.Receipt;
			engine::core::ByteWriter writer(0, MAXIMUM_PAYLOAD);
			writer.WriteUInt8(static_cast<uint8_t>(record.Outcome));
			WriteText(writer, receipt.Id.SourceWorld);
			writer.WriteUInt64(receipt.Id.SourceIncarnation);
			writer.WriteUInt64(receipt.Id.Sequence);
			WriteText(writer, receipt.DestinationWorld);
			writer.WriteUInt8(static_cast<uint8_t>(receipt.Stage));
			writer.WriteRaw(&receipt.Through.Frame, sizeof(receipt.Through.Frame));
			writer.WriteRaw(&receipt.Through.Origin, sizeof(receipt.Through.Origin));
			writer.WriteFloat(receipt.Through.Scale);
			WriteText(writer, receipt.Diagnostic);
			writer.WriteUInt8(static_cast<uint8_t>(receipt.Kind));
			WriteFence(writer, receipt.Fence);
			writer.WriteUInt64(receipt.AcknowledgedInputTick);
			writer.WriteBool(receipt.Motion.has_value());
			if (receipt.Motion && !engine::script::WritePortalTransferMotion(writer, *receipt.Motion))
				return false;
			writer.WriteUInt64(record.Decision.Body.Key.High);
			writer.WriteUInt64(record.Decision.Body.Key.Low);
			writer.WriteUInt64(record.Decision.Body.Generation);
			writer.WriteUInt32(static_cast<uint32_t>(record.Decision.Baseline.size()));
			writer.WriteRaw(record.Decision.Baseline.data(), record.Decision.Baseline.size());
			out.assign(writer.Bytes().begin(), writer.Bytes().end());
			return true;
		}
		bool Decode(std::span<const std::byte> bytes, PortalJournalRecord &record) {
			engine::core::ByteReader reader(bytes);
			const uint8_t outcome = reader.ReadUInt8();
			if (outcome != static_cast<uint8_t>(PortalJournalOutcome::Commit) &&
				outcome != static_cast<uint8_t>(PortalJournalOutcome::Completed))
				return false;
			auto &receipt = record.Decision.Receipt;
			if (!ReadText(reader, receipt.Id.SourceWorld)) return false;
			receipt.Id.SourceIncarnation = reader.ReadUInt64();
			receipt.Id.Sequence = reader.ReadUInt64();
			if (!ReadText(reader, receipt.DestinationWorld)) return false;
			const uint8_t stage = reader.ReadUInt8();
			if (stage > static_cast<uint8_t>(engine::script::PortalTransferStage::Cancelling)) return false;
			receipt.Stage = static_cast<engine::script::PortalTransferStage>(stage);
			if (!reader.ReadRaw(&receipt.Through.Frame, sizeof(receipt.Through.Frame)) ||
				!reader.ReadRaw(&receipt.Through.Origin, sizeof(receipt.Through.Origin)))
				return false;
			receipt.Through.Scale = reader.ReadFloat();
			if (!ReadText(reader, receipt.Diagnostic, true)) return false;
			const uint8_t kind = reader.ReadUInt8();
			if (kind > static_cast<uint8_t>(engine::scene::PortalBodyKind::Object)) return false;
			receipt.Kind = static_cast<engine::scene::PortalBodyKind>(kind);
			if (!ReadFence(reader, receipt.Fence)) return false;
			receipt.AcknowledgedInputTick = reader.ReadUInt64();
			const uint8_t hasMotion = reader.ReadUInt8();
			if (hasMotion > 1) return false;
			if (hasMotion != 0) {
				engine::script::PortalTransferMotion motion;
				if (!engine::script::ReadPortalTransferMotion(reader, motion)) return false;
				receipt.Motion = motion;
			}
			record.Decision.Body.Key.High = reader.ReadUInt64();
			record.Decision.Body.Key.Low = reader.ReadUInt64();
			record.Decision.Body.Generation = reader.ReadUInt64();
			const uint32_t baselineSize = reader.ReadUInt32();
			if (baselineSize == 0 || baselineSize > engine::scene::MAXIMUM_PORTAL_BODY_BYTES ||
				baselineSize > reader.Remaining())
				return false;
			record.Decision.Baseline.resize(baselineSize);
			if (!reader.ReadRaw(record.Decision.Baseline.data(), baselineSize) || !reader.AtEnd())
				return false;
			record.Outcome = static_cast<PortalJournalOutcome>(outcome);
			return ValidDecision(record.Decision);
		}
		bool WriteAll(int file, std::span<const std::byte> bytes) {
			size_t offset = 0;
			while (offset < bytes.size()) {
#if defined(_WIN32)
				const unsigned chunk = static_cast<unsigned>(std::min<size_t>(
					bytes.size() - offset, static_cast<size_t>(std::numeric_limits<int>::max())
				));
				const int wrote = _write(file, bytes.data() + offset, chunk);
#else
				const ssize_t wrote = ::write(file, bytes.data() + offset, bytes.size() - offset);
#endif
				if (wrote <= 0) return false;
				offset += static_cast<size_t>(wrote);
			}
			return true;
		}
		bool Flush(int file) {
#if defined(_WIN32)
			return _commit(file) == 0;
#else
			return ::fsync(file) == 0;
#endif
		}
		void Close(int file) {
#if defined(_WIN32)
			(void)_close(file);
#else
			(void)::close(file);
#endif
		}
	}

	bool LoadPortalJournal(const std::filesystem::path &path, std::vector<PortalJournalRecord> &records) {
		records.clear();
		std::ifstream input(path, std::ios::binary);
		if (!input) return !std::filesystem::exists(path);
		const std::vector<char> characters{
			std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
		};
		std::vector<std::byte> file(characters.size());
		std::memcpy(file.data(), characters.data(), characters.size());
		size_t offset = 0;
		while (offset < file.size()) {
			constexpr size_t HEADER =
				sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t) + engine::assets::ContentHash::BYTES;
			if (file.size() - offset < HEADER) break;
			engine::core::ByteReader header(std::span<const std::byte>(file.data() + offset, HEADER));
			const uint32_t magic = header.ReadUInt32();
			const uint16_t version = header.ReadUInt16();
			const uint32_t length = header.ReadUInt32();
			engine::assets::ContentHash checksum;
			if (!header.ReadRaw(checksum.Digest.data(), checksum.Digest.size()) || !header.AtEnd() ||
				magic != MAGIC || version != VERSION || length == 0 || length > MAXIMUM_PAYLOAD)
				return false;
			if (file.size() - offset - HEADER < length) break;
			const std::span<const std::byte> payload(file.data() + offset + HEADER, length);
			PortalJournalRecord record;
			if (engine::assets::Hasher::Of(payload) != checksum || !Decode(payload, record)) {
				if (offset + HEADER + length == file.size()) break;
				return false;
			}
			auto same = std::find_if(records.begin(), records.end(), [&](const auto &prior) {
				return SameDecision(prior.Decision, record.Decision);
			});
			if (same != records.end()) {
				if (same->Outcome != PortalJournalOutcome::Commit ||
					record.Outcome != PortalJournalOutcome::Completed)
					return false;
				*same = std::move(record);
			} else {
				if (records.size() == MAXIMUM_RECORDS) return false;
				records.push_back(std::move(record));
			}
			offset += HEADER + length;
		}
		return true;
	}

	bool
	AppendPortalJournal(const std::filesystem::path &path, std::span<const PortalJournalRecord> records) {
		if (records.empty()) return true;
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) return false;
#if defined(_WIN32)
		const int file =
			_wopen(path.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_APPEND, _S_IREAD | _S_IWRITE);
#else
		const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
#endif
		if (file < 0) return false;
		bool written = true;
		for (const auto &record : records) {
			std::vector<std::byte> payload;
			if (!Encode(record, payload)) {
				written = false;
				break;
			}
			engine::core::ByteWriter frame;
			frame.WriteUInt32(MAGIC);
			frame.WriteUInt16(VERSION);
			frame.WriteUInt32(static_cast<uint32_t>(payload.size()));
			const engine::assets::ContentHash checksum = engine::assets::Hasher::Of(payload);
			frame.WriteRaw(checksum.Digest.data(), checksum.Digest.size());
			frame.WriteRaw(payload.data(), payload.size());
			if (!WriteAll(file, frame.Bytes())) {
				written = false;
				break;
			}
		}
		const bool flushed = written && Flush(file);
		Close(file);
		return flushed;
	}

	bool PortalJournalContains(
		std::span<const PortalJournalRecord> records, const engine::script::PortalTransferDecision &decision
	) {
		return std::any_of(records.begin(), records.end(), [&](const auto &record) {
			return SameDecision(record.Decision, decision);
		});
	}
	bool PortalJournalPending(
		std::span<const PortalJournalRecord> records, const engine::script::PortalTransferDecision &decision
	) {
		return std::any_of(records.begin(), records.end(), [&](const auto &record) {
			return record.Outcome == PortalJournalOutcome::Commit && SameDecision(record.Decision, decision);
		});
	}
}
