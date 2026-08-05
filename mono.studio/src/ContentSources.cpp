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
		out << "# one line per origin, in priority order: name | kind | location | on/off\n";
		out << "cache = " << CachePath.generic_string() << "\n";
		out << "publisher = " << PublisherKey << "\n";

		for (const Source &source : Sources) {
			// The separator is a pipe rather than a comma because a Windows
			// path holds a colon and a name may hold anything a person typed.
			out << "source = " << source.Name << " | " << Describe(source.Kind) << " | " << source.Location
				<< " | " << (source.Enabled ? "on" : "off") << "\n";
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

			// name | kind | location | on/off. A row that does not have four
			// fields is skipped rather than half-read: a source with no
			// location would sit in the list looking configured and refuse
			// every fetch.
			std::vector<std::string_view> fields;
			std::string_view rest = value;
			while (!rest.empty() && fields.size() < 4) {
				const size_t bar = rest.find('|');
				if (bar == std::string_view::npos) {
					fields.push_back(Trim(rest));
					break;
				}
				fields.push_back(Trim(rest.substr(0, bar)));
				rest.remove_prefix(bar + 1);
			}
			if (fields.size() != 4) {
				continue;
			}

			Source source;
			source.Name = std::string(fields[0]);
			source.Kind = fields[1] == "directory" ? SourceKind::Directory : SourceKind::Http;
			source.Location = std::string(fields[2]);
			source.Enabled = fields[3] == "on";
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
