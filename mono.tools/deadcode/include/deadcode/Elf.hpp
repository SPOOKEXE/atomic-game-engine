#pragma once

// The parts of an ELF64 relocatable object that reachability needs.
//
// Sections, the symbol table and the RELA relocations, and nothing else. No
// program headers, no dynamic table, no DWARF. Little-endian only, which is
// every target this repository builds an ELF for.
//
//     deadcode::ObjectFile object;
//     std::string error;
//     if (!deadcode::ReadObject(bytes, object, error)) { ... }
//
// @tier L0 · shared

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace deadcode {

	struct Section {
		std::string Name;
		uint32_t Type = 0;
		uint64_t Flags = 0;
		uint64_t Size = 0;
	};

	// One symbol table row. `Section` is the raw `st_shndx`, so 0 is undefined
	// and values at or above 0xff00 are the reserved indices.
	struct Symbol {
		std::string Name;
		uint64_t Value = 0;
		uint64_t Size = 0;
		uint16_t Section = 0;
		uint8_t Type = 0;
		uint8_t Bind = 0;
	};

	struct Relocation {
		uint64_t Offset = 0;
		uint32_t Symbol = 0;
		uint32_t Type = 0;
		int64_t Addend = 0;
	};

	// Every relocation that patches one section, keyed by that section's index.
	struct RelocationSet {
		uint32_t Target = 0;
		std::vector<Relocation> Entries;
	};

	struct ObjectFile {
		std::vector<Section> Sections;
		std::vector<Symbol> Symbols;
		std::vector<RelocationSet> Relocations;
	};

	constexpr uint8_t SYMBOL_OBJECT = 1;
	constexpr uint8_t SYMBOL_FUNC = 2;
	constexpr uint8_t SYMBOL_SECTION = 3;
	constexpr uint8_t BIND_LOCAL = 0;
	constexpr uint16_t SECTION_UNDEFINED = 0;
	constexpr uint16_t SECTION_RESERVED = 0xff00;

	// Parses `bytes` as an ELF64 little-endian file. Returns false and fills
	// `error` for anything else, or for a table that runs off the end.
	bool ReadObject(std::string_view bytes, ObjectFile &out, std::string &error);

}
