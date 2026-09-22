#pragma once

// Bounded, side-effect-free localized message lookup for the retained GUI.
//
// Catalogues carry only supplied strings. Resolving a message never reads a
// file, calls a script, or inspects an instance tree, which keeps a viewer's
// locale a normal explicit input to layout and compile. The bounded formatter
// accepts `{name}`, `{name, number}`, `{name, date}`, plural `=N`, `one`, and
// `other` cases, and select cases with `other`; malformed input stays visible.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/gui/Document.hpp>
#include <engine/gui/Enums.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {
	struct LocalizedArgument {
		core::Name Name;
		std::string Value;
		LocalizedArgumentType Type = LocalizedArgumentType::String;
		double Number = 0.0;
		int64_t UnixSeconds = 0;

		static LocalizedArgument FromNumber(core::Name name, double value);
		static LocalizedArgument FromDate(core::Name name, int64_t unixSeconds);
	};

	struct LocalizedMessage {
		core::Name Key;
		std::string Source;
		std::array<LocalizedArgument, 8> Arguments{};
		size_t ArgumentCount = 0;

		bool AddArgument(LocalizedArgument argument);
	};

	struct CatalogueEntry {
		std::string Locale;
		core::Name Key;
		std::string Text;
	};

	class LocalizationCatalogue {
	  public:
		static constexpr size_t MAXIMUM_ENTRIES = 256;
		static constexpr size_t MAXIMUM_SOURCE_BYTES = 1024u * 1024u;
		static constexpr size_t MAXIMUM_LOCALE_BYTES = 64;
		static constexpr size_t MAXIMUM_MESSAGE_BYTES = 4096;
		static constexpr size_t MAXIMUM_OUTPUT_BYTES = 8192;

		bool Set(CatalogueEntry entry);
		// Applies entries in source order. A later table wins a duplicate key,
		// matching the deterministic hierarchy order used by collection.
		bool Merge(const LocalizationCatalogue &other);
		// Selects the project locale used after the viewer's locale parents.
		bool SetSourceLocale(std::string locale);
		std::string Resolve(
			const LocalizedMessage &message, std::string_view locale, bool studioMissingMarker = false
		) const;
		uint64_t Revision() const {
			return RevisionNumber;
		}

	  private:
		std::array<CatalogueEntry, MAXIMUM_ENTRIES> Entries{};
		size_t Count = 0;
		std::string SourceLocale = "en";
		uint64_t RevisionNumber = 0;
	};

	// Decodes bounded RFC 4180 `locale,key,text` content. The result replaces
	// `out` only after every row is accepted.
	bool DecodeLocalizationCsv(std::string_view source, LocalizationCatalogue &out, DocumentReport &report);

	// Rebuilds a viewer-local catalogue from LocalizationTable instances under
	// ReplicatedStorage. Saved tables are data; this derived value never mutates
	// the store or crosses a world boundary.
	bool CollectLocalizationTables(const ecs::Store &store, LocalizationCatalogue &out);

	// Viewer-owned cache for the catalogue derived from one world. Only table text
	// and hierarchy changes rebuild it, so unrelated simulation writes do not
	// reparse localization data.
	struct LocalizationCache {
		uint64_t StoreIdentity = 0;
		uint64_t TextRevision = 0;
		uint64_t HierarchyRevision = 0;
		LocalizationCatalogue Catalogue;

		const LocalizationCatalogue *Refresh(ecs::Store &store);
	};
}
