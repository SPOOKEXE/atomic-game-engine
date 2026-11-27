#include <deadcode/Elf.hpp>

#include <cstring>

namespace deadcode {

	namespace {

		constexpr uint32_t SHT_SYMTAB = 2;
		constexpr uint32_t SHT_RELA = 4;

		// Bounds-checked little-endian reads. Every offset in an ELF file is
		// data from the file, so every read is checked rather than trusted.
		struct Reader {
			std::string_view Bytes;
			bool Ok = true;

			template <typename T> T At(uint64_t offset) {
				T value{};
				if (offset > Bytes.size() || Bytes.size() - offset < sizeof(T)) {
					Ok = false;
					return value;
				}
				std::memcpy(&value, Bytes.data() + offset, sizeof(T));
				return value;
			}

			std::string String(uint64_t table, uint64_t tableSize, uint32_t index) {
				if (index >= tableSize || table + tableSize > Bytes.size()) {
					return {};
				}
				const char *start = Bytes.data() + table + index;
				const size_t limit = static_cast<size_t>(tableSize - index);
				return std::string(start, strnlen(start, limit));
			}
		};

		struct RawSection {
			uint32_t Name = 0;
			uint32_t Type = 0;
			uint64_t Flags = 0;
			uint64_t Offset = 0;
			uint64_t Size = 0;
			uint32_t Link = 0;
			uint32_t Info = 0;
			uint64_t EntrySize = 0;
		};
	}

	bool ReadObject(std::string_view bytes, ObjectFile &out, std::string &error) {
		out = {};
		Reader read{bytes};

		if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0) {
			error = "not an ELF file";
			return false;
		}
		if (bytes[4] != 2 || bytes[5] != 1) {
			error = "not ELF64 little-endian";
			return false;
		}

		const uint64_t sectionTable = read.At<uint64_t>(0x28);
		const uint16_t sectionEntrySize = read.At<uint16_t>(0x3a);
		const uint16_t sectionCount = read.At<uint16_t>(0x3c);
		const uint16_t namesIndex = read.At<uint16_t>(0x3e);
		if (sectionEntrySize < 64 || namesIndex >= sectionCount) {
			error = "bad section header table";
			return false;
		}

		std::vector<RawSection> raw(sectionCount);
		for (uint16_t i = 0; i < sectionCount; ++i) {
			const uint64_t at = sectionTable + uint64_t(i) * sectionEntrySize;
			raw[i].Name = read.At<uint32_t>(at + 0x00);
			raw[i].Type = read.At<uint32_t>(at + 0x04);
			raw[i].Flags = read.At<uint64_t>(at + 0x08);
			raw[i].Offset = read.At<uint64_t>(at + 0x18);
			raw[i].Size = read.At<uint64_t>(at + 0x20);
			raw[i].Link = read.At<uint32_t>(at + 0x28);
			raw[i].Info = read.At<uint32_t>(at + 0x2c);
			raw[i].EntrySize = read.At<uint64_t>(at + 0x38);
		}
		if (!read.Ok) {
			error = "section header table runs past the end of the file";
			return false;
		}

		const RawSection &names = raw[namesIndex];
		out.Sections.resize(sectionCount);
		for (uint16_t i = 0; i < sectionCount; ++i) {
			out.Sections[i].Name = read.String(names.Offset, names.Size, raw[i].Name);
			out.Sections[i].Type = raw[i].Type;
			out.Sections[i].Flags = raw[i].Flags;
			out.Sections[i].Size = raw[i].Size;
		}

		// A relocatable object has exactly one static symbol table.
		for (const RawSection &section : raw) {
			if (section.Type != SHT_SYMTAB || section.EntrySize < 24 || section.Link >= sectionCount) {
				continue;
			}
			const RawSection &strings = raw[section.Link];
			const uint64_t count = section.Size / section.EntrySize;
			out.Symbols.resize(count);
			for (uint64_t i = 0; i < count; ++i) {
				const uint64_t at = section.Offset + i * section.EntrySize;
				Symbol &symbol = out.Symbols[i];
				symbol.Name = read.String(strings.Offset, strings.Size, read.At<uint32_t>(at));
				const uint8_t info = read.At<uint8_t>(at + 4);
				symbol.Type = info & 0xf;
				symbol.Bind = info >> 4;
				symbol.Section = read.At<uint16_t>(at + 6);
				symbol.Value = read.At<uint64_t>(at + 8);
				symbol.Size = read.At<uint64_t>(at + 16);
			}
			break;
		}

		for (const RawSection &section : raw) {
			if (section.Type != SHT_RELA || section.EntrySize < 24 || section.Info >= sectionCount) {
				continue;
			}
			RelocationSet set;
			set.Target = section.Info;
			const uint64_t count = section.Size / section.EntrySize;
			set.Entries.resize(count);
			for (uint64_t i = 0; i < count; ++i) {
				const uint64_t at = section.Offset + i * section.EntrySize;
				const uint64_t info = read.At<uint64_t>(at + 8);
				set.Entries[i].Offset = read.At<uint64_t>(at);
				set.Entries[i].Symbol = static_cast<uint32_t>(info >> 32);
				set.Entries[i].Type = static_cast<uint32_t>(info & 0xffffffffu);
				set.Entries[i].Addend = read.At<int64_t>(at + 16);
			}
			out.Relocations.push_back(std::move(set));
		}

		if (!read.Ok) {
			error = "a table runs past the end of the file";
			return false;
		}
		return true;
	}

}
