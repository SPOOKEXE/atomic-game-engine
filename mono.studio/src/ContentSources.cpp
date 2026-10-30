#include <engine/assets/LocalStore.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Log.hpp>

#include <fstream>
#include <iterator>
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

		std::string EncodeField(std::string_view text) {
			constexpr char hex[] = "0123456789ABCDEF";
			std::string encoded;
			for (const unsigned char c : text) {
				if (c == '%' || c == '|' || c <= ' ' || c == 127) {
					encoded += '%';
					encoded += hex[c >> 4];
					encoded += hex[c & 15];
				} else {
					encoded += static_cast<char>(c);
				}
			}
			return encoded;
		}

		bool DecodeField(std::string_view text, std::string &decoded) {
			const auto digit = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				return -1;
			};
			for (size_t i = 0; i < text.size(); ++i) {
				if (text[i] != '%') {
					decoded += text[i];
					continue;
				}
				if (i + 2 >= text.size()) return false;
				const int high = digit(text[i + 1]);
				const int low = digit(text[i + 2]);
				if (high < 0 || low < 0) return false;
				decoded += static_cast<char>((high << 4) | low);
				i += 2;
			}
			return true;
		}
	}

	ContentSources ContentSources::Default() {
		// **The store on this machine, and a key that can verify it.** The
		// default was `127.0.0.1:9080` with no publisher key, which is a
		// perfectly reasonable default for a *deployment* and meant that a fresh
		// editor could fetch nothing at all: `DeliverySettings::IsValid` refuses
		// without a key, so `MakeAssetClient` was never called and no mesh, no
		// texture and no material ever reached the renderer. A `MeshPart` drew
		// the fallback cube, a `ColorMap` drew nothing, and the assets panel
		// listed a store full of content none of which the viewport could show.
		//
		// The client already defaults to the same folder - `client/app/main.cpp`
		// - and to `engine::assets::DevelopmentPublisher` for it, which is what makes the
		// key here safe to fill in: it verifies only the store this editor
		// publishes to, and pointing a row anywhere else means supplying the
		// real one. `engine::assets::DevelopmentSigningKey` carries what that identity is
		// and is not for.
		const engine::assets::LocalPaths local = engine::assets::DefaultLocalPaths();

		ContentSources sources;
		sources.PublisherKey = engine::assets::DevelopmentPublisher().ToHex();
		sources.Sources.push_back(
			engine::delivery::Source{
				.Name = "local store",
				.Kind = engine::delivery::SourceKind::Directory,
				.Location = local.Processed.string(),
				.Enabled = true,
				.Role = engine::delivery::SourceRole::Read,
				.IngestKey = {},
			}
		);

		// **The origin next door is kept and turned off**, rather than dropped.
		// A row that is off is one somebody can switch on - `ContentSources.hpp`
		// says why a disabled row is kept - and this is the address a deployment
		// uses, so deleting it would make the common remote case something to
		// retype rather than something to enable.
		const engine::delivery::DeliverySettings defaults = engine::delivery::DeliverySettings::Default();
		for (engine::delivery::Source source : defaults.Sources) {
			source.Enabled = false;
			sources.Sources.push_back(std::move(source));
		}
		return sources;
	}

	engine::delivery::DeliverySettings ContentSources::ToSettings() const {
		engine::delivery::DeliverySettings settings;
		settings.Sources = UniverseSources;
		settings.Sources.insert(settings.Sources.end(), Sources.begin(), Sources.end());
		settings.CachePath = CachePath;
		const std::string &publisher = UniversePublisherKey.empty() ? PublisherKey : UniversePublisherKey;
		if (const auto key = engine::assets::PublicKey::FromHex(publisher)) {
			settings.Publisher = *key;
		}
		// **`AllowedHosts` is deliberately left empty here.** That check exists
		// for a source list a *server* sent, where fetching from a hostile host
		// is a request-forgery primitive - and this list is one a person typed
		// into their own preferences. Restricting what somebody may point their
		// own editor at would be security theatre with a real cost.
		return settings;
	}

	void ContentSources::SetUniverseAssets(const std::filesystem::path &processed) {
		SetUniverseContent(processed, {}, {});
	}

	void ContentSources::SetUniverseContent(
		const std::filesystem::path &processed,
		std::vector<engine::delivery::Source> remote,
		std::string publisherKey
	) {
		UniverseSources.clear();
		UniversePublisherKey = std::move(publisherKey);
		if (processed.empty()) {
			UniverseSources = std::move(remote);
			return;
		}
		UniverseSources.push_back(
			engine::delivery::Source{
				.Name = "universe assets",
				.Kind = engine::delivery::SourceKind::Directory,
				.Location = processed.string(),
				.Enabled = true,
				.Role = engine::delivery::SourceRole::Read,
				.IngestKey = {},
			}
		);
		UniverseSources.insert(
			UniverseSources.end(),
			std::make_move_iterator(remote.begin()),
			std::make_move_iterator(remote.end())
		);
	}

	bool ContentSources::Save(const std::filesystem::path &path) const {
		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return false;
		}

		out << "# atomic studio content sources\n";
		out << "# one line per origin, in priority order:\n";
		out << "#   source-v2: percent-escaped name | kind | location | on/off | role | ingest key\n";
		out << "# raw = a folder of unprocessed art this editor may bake from\n";
		out << "cache = " << CachePath.generic_string() << "\n";
		out << "publisher = " << PublisherKey << "\n";
		out << "memory-only = " << (MemoryOnly ? "on" : "off") << "\n";

		for (const std::filesystem::path &folder : RawFolders) {
			out << "raw = " << folder.generic_string() << "\n";
		}

		for (const Source &source : Sources) {
			out << "source-v2 = " << EncodeField(source.Name) << " | " << Describe(source.Kind) << " | "
				<< EncodeField(source.Location) << " | " << (source.Enabled ? "on" : "off") << " | "
				<< Describe(source.Role) << " | " << EncodeField(source.IngestKey) << "\n";
		}
		return out.good();
	}

	bool ContentSources::Load(const std::filesystem::path &path) {
		std::ifstream in(path);
		if (!in) {
			// **Not an error.** A fresh install has no file and gets the
			// default, which is what somebody expects the first time they open
			// the editor - `Keybinds::Load`'s rule.
			return false;
		}

		Sources.clear();
		CachePath.clear();
		PublisherKey.clear();
		RawFolders.clear();

		// **Not reset to `true` with the rest.** A file written before raw
		// folders existed says nothing about this, and the default is the safe
		// answer - a store that quietly started collecting baked copies of
		// somebody's art folder would be the wrong way to be wrong.
		MemoryOnly = true;

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
			if (key == "raw") {
				RawFolders.emplace_back(value);
				continue;
			}
			if (key == "memory-only") {
				// Anything unrecognised reads as on, which is the answer that
				// writes nothing to somebody's disk.
				MemoryOnly = value != "off";
				continue;
			}
			const bool escaped = key == "source-v2";
			if (key != "source" && !escaped) {
				continue;
			}

			// name | kind | location | on/off | role | ingest key
			//
			// A row that does not have at least the first four is skipped
			// rather than half-read: a source with no location would sit in the
			// list looking configured and refuse every fetch.
			//
			// **The last two are optional, and a file written before roles
			// existed still loads.** Not for compatibility's sake - this is a
			// preferences file and `save-format breaks are acceptable` is the
			// standing rule - but because the absent case has an obviously
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
			if (escaped) {
				if (!DecodeField(fields[0], source.Name) || !DecodeField(fields[2], source.Location) ||
					(fields.size() >= 6 && !DecodeField(fields[5], source.IngestKey))) {
					continue;
				}
			} else {
				source.Name = std::string(fields[0]);
				source.Location = std::string(fields[2]);
				if (fields.size() >= 6) source.IngestKey = std::string(fields[5]);
			}
			source.Kind = fields[1] == "directory" ? SourceKind::Directory : SourceKind::Http;
			source.Enabled = fields[3] == "on";

			if (fields.size() >= 5) {
				// Anything unrecognised reads as `Both` for the reason above:
				// the permissive answer is the one that keeps a list working.
				source.Role = fields[4] == "read"	 ? SourceRole::Read
							  : fields[4] == "write" ? SourceRole::Write
													 : SourceRole::Both;
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
