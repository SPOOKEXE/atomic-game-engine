#include <engine/assets/Signature.hpp>
#include <engine/core/Log.hpp>

#include <fstream>
#include <sstream>
#include <string_view>
#include <studio/ContentSources.hpp>
#include <utility>

namespace studio {
	namespace {
		using engine::delivery::Source;
		using engine::delivery::SourceKind;
		using engine::delivery::SourceRole;

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
				text.remove_suffix(1);
			}
			return text;
		}
	}

	ContentSources ContentSources::Default() {
		ContentSources sources;
		const engine::delivery::DeliverySettings defaults = engine::delivery::DeliverySettings::Default();
		sources.Sources = defaults.Sources;
		return sources;
	}

	engine::delivery::DeliverySettings ContentSources::ToSettings() const {
		engine::delivery::DeliverySettings settings;
		settings.Sources = Sources;
		settings.CachePath = CachePath;
		if (const auto key = engine::assets::PublicKey::FromHex(PublisherKey)) {
			settings.Publisher = *key;
		}
		// **`AllowedHosts` is deliberately left empty here.** That check exists
		// for a source list a *server* sent — `repo_layout.md` §11's
		// request-forgery note — and this list is one a person typed into their
		// own preferences. Restricting what somebody may point their own editor
		// at would be security theatre with a real cost.
		return settings;
	}

	bool ContentSources::Save(const std::filesystem::path &path) const {
		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return false;
		}

		out << "# atomic studio content sources\n";
		out << "# one line per origin, in priority order:\n";
		out << "#   name | kind | location | on/off | read|write|both | ingest key\n";
		out << "cache = " << CachePath.generic_string() << "\n";
		out << "publisher = " << PublisherKey << "\n";

		for (const Source &source : Sources) {
			// The separator is a pipe rather than a comma because a Windows
			// path holds a colon and a name may hold anything a person typed.
			out << "source = " << source.Name << " | " << Describe(source.Kind) << " | " << source.Location
				<< " | " << (source.Enabled ? "on" : "off") << " | " << Describe(source.Role) << " | "
				<< source.IngestKey << "\n";
		}
		return out.good();
	}

	bool ContentSources::Load(const std::filesystem::path &path) {
		std::ifstream in(path);
		if (!in) {
			// **Not an error.** A fresh install has no file and gets the
			// default, which is what somebody expects the first time they open
			// the editor — `Keybinds::Load`'s rule.
			return false;
		}

		Sources.clear();
		CachePath.clear();
		PublisherKey.clear();

		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line.front() == '#') {
				continue;
			}
			const size_t equals = line.find('=');
			if (equals == std::string::npos) {
				continue;
			}

			const std::string_view key = Trim(std::string_view(line).substr(0, equals));
			const std::string_view value = Trim(std::string_view(line).substr(equals + 1));

			if (key == "cache") {
				CachePath = std::filesystem::path(value);
				continue;
			}
			if (key == "publisher") {
				PublisherKey = std::string(value);
				continue;
			}
			if (key != "source") {
				continue;
			}

			// name | kind | location | on/off | role | ingest key
			//
			// A row that does not have at least the first four is skipped
			// rather than half-read: a source with no location would sit in the
			// list looking configured and refuse every fetch.
			//
			// **The last two are optional, and a file written before roles
			// existed still loads.** Not for compatibility's sake — this is a
			// preferences file and `save-format breaks are acceptable` is the
			// standing rule — but because the absent case has an obviously
			// right answer: a list written when there was no choice meant
			// `Both`, and reading it as anything else would silently stop a
			// working setup from fetching or from uploading.
			//
			// **The key is the remainder rather than a field**, because it is a
			// secret somebody pasted and a pipe in it is not this parser's
			// business to forbid.
			std::vector<std::string_view> fields;
			std::string_view rest = value;
			while (fields.size() < 5) {
				const size_t bar = rest.find('|');
				if (bar == std::string_view::npos) {
					break;
				}
				fields.push_back(Trim(rest.substr(0, bar)));
				rest.remove_prefix(bar + 1);
			}
			fields.push_back(Trim(rest));

			if (fields.size() < 4) {
				continue;
			}

			Source source;
			source.Name = std::string(fields[0]);
			source.Kind = fields[1] == "directory" ? SourceKind::Directory : SourceKind::Http;
			source.Location = std::string(fields[2]);
			source.Enabled = fields[3] == "on";

			if (fields.size() >= 5) {
				// Anything unrecognised reads as `Both` for the reason above:
				// the permissive answer is the one that keeps a list working.
				source.Role = fields[4] == "read"    ? SourceRole::Read
							  : fields[4] == "write" ? SourceRole::Write
													 : SourceRole::Both;
			}
			if (fields.size() >= 6) {
				source.IngestKey = std::string(fields[5]);
			}

			Sources.push_back(std::move(source));
		}
		return true;
	}

	bool ContentSources::Move(size_t index, int delta) {
		if (index >= Sources.size()) {
			return false;
		}
		const auto target = static_cast<ptrdiff_t>(index) + delta;
		if (target < 0 || target >= static_cast<ptrdiff_t>(Sources.size())) {
			return false;
		}
		std::swap(Sources[index], Sources[static_cast<size_t>(target)]);
		return true;
	}
}
