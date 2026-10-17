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
	// One named formatting value for a localized message.
	struct LocalizedArgument {
		// Placeholder name in the message source.
		core::Name Name;
		// String payload when Type is String.
		std::string Value;
		// Selects the active formatting payload.
		LocalizedArgumentType Type = LocalizedArgumentType::String;
		// Numeric payload when Type is Number.
		double Number = 0.0;
		// UTC timestamp payload when Type is Date.
		int64_t UnixSeconds = 0;

		// Creates a numeric formatting argument.
		static LocalizedArgument FromNumber(core::Name name, double value);
		// Creates a UTC date formatting argument.
		static LocalizedArgument FromDate(core::Name name, int64_t unixSeconds);
	};

	// A source message and its bounded formatting arguments.
	struct LocalizedMessage {
		// Catalogue key for a translated message.
		core::Name Key;
		// Fallback source text when no translation resolves.
		std::string Source;
		// Formatting arguments in insertion order.
		std::array<LocalizedArgument, 8> Arguments{};
		// Number of active entries in Arguments.
		size_t ArgumentCount = 0;

		// Adds a named argument within the fixed capacity.
		bool AddArgument(LocalizedArgument argument);
	};

	// One localized text value for a locale and message key.
	struct CatalogueEntry {
		// BCP 47 locale of this translation.
		std::string Locale;
		// Message key this entry translates.
		core::Name Key;
		// Localized message source.
		std::string Text;
	};

	// A bounded viewer-owned set of localized message entries.
	class LocalizationCatalogue {
	  public:
		// Largest number of translated message entries.
		static constexpr size_t MAXIMUM_ENTRIES = 256;
		// Largest accepted localization source file.
		static constexpr size_t MAXIMUM_SOURCE_BYTES = 1024u * 1024u;
		// Largest encoded locale tag.
		static constexpr size_t MAXIMUM_LOCALE_BYTES = 64;
		// Largest localized message source.
		static constexpr size_t MAXIMUM_MESSAGE_BYTES = 4096;
		// Largest formatted message output.
		static constexpr size_t MAXIMUM_OUTPUT_BYTES = 8192;

		// Adds or replaces one valid translation.
		bool Set(CatalogueEntry entry);
		// Applies entries in source order. A later table wins a duplicate key,
		// matching the deterministic hierarchy order used by collection.
		bool Merge(const LocalizationCatalogue &other);
		// Selects the project locale used after the viewer's locale parents.
		bool SetSourceLocale(std::string locale);
		// Resolves and formats a message for a viewer locale.
		std::string Resolve(
			const LocalizedMessage &message, std::string_view locale, bool studioMissingMarker = false
		) const;
		// Monotonic revision of accepted catalogue changes.
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
		// Identity of the world that supplied the cached catalogue.
		uint64_t StoreIdentity = 0;
		// Last observed localization-table text revision.
		uint64_t TextRevision = 0;
		// Last observed localization-table hierarchy revision.
		uint64_t HierarchyRevision = 0;
		// Viewer-local catalogue rebuilt from the world.
		LocalizationCatalogue Catalogue;

		// Refreshes the cache when its source world changes.
		const LocalizationCatalogue *Refresh(ecs::Store &store);
	};
}
