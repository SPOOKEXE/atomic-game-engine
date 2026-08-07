#include <engine/delivery/Source.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>

namespace engine::delivery {
	namespace {
		// Splits `host:port`, refusing anything that is not exactly that.
		//
		// A bracketed IPv6 literal is handled because `Endpoint::Text` writes
		// one and this has to read back what that wrote — otherwise a v6
		// deployment could be configured and never reached.
		bool SplitLocation(std::string_view location, std::string_view &host, uint16_t &port) {
			if (location.empty()) {
				return false;
			}

			size_t colon = std::string_view::npos;
			if (location.front() == '[') {
				const size_t close = location.find(']');
				if (close == std::string_view::npos) {
					return false;
				}
				host = location.substr(1, close - 1);
				colon = close + 1;
				if (colon >= location.size() || location[colon] != ':') {
					return false;
				}
			} else {
				colon = location.rfind(':');
				if (colon == std::string_view::npos || colon == 0) {
					return false;
				}
				host = location.substr(0, colon);
			}

			const std::string_view portText = location.substr(colon + 1);
			if (portText.empty() || portText.size() > 5) {
				return false;
			}
			uint32_t value = 0;
			const auto *const first = portText.data();
			const auto ending = std::from_chars(first, first + portText.size(), value);
			if (ending.ec != std::errc() || ending.ptr != first + portText.size()) {
				return false;
			}
			if (value == 0 || value > 65535) {
				return false;
			}
			port = static_cast<uint16_t>(value);
			return !host.empty();
		}
	}

	const char *Describe(SourceKind kind) {
		switch (kind) {
		case SourceKind::Directory:
			return "directory";
		case SourceKind::Http:
			return "http";
		}
		return "unknown";
	}

	const char *Describe(SourceRole role) {
		switch (role) {
		case SourceRole::Both:
			return "both";
		case SourceRole::Read:
			return "read";
		case SourceRole::Write:
			return "write";
		}
		return "unknown";
	}

	bool Source::IsValid() const {
		if (Name.empty() || Location.empty()) {
			return false;
		}
		if (Kind == SourceKind::Directory) {
			return true;
		}
		std::string_view host;
		uint16_t port = 0;
		return SplitLocation(Location, host, port);
	}

	bool HostPermitted(std::string_view location, const std::vector<std::string> &allowed) {
		if (allowed.empty()) {
			// No restriction. Right for a list a person typed into their own
			// preferences, and wrong for one a server sent — which is why the
			// caller assembling the second kind fills this in.
			return true;
		}
		std::string_view host;
		uint16_t port = 0;
		if (!SplitLocation(location, host, port)) {
			return false;
		}
		// Matched against the host alone rather than against `host:port`: an
		// operator declares which machines are permitted, and a port is a
		// deployment detail of the machine that was already permitted.
		return std::any_of(allowed.begin(), allowed.end(), [host](const std::string &candidate) {
			return candidate == host;
		});
	}

	DeliverySettings DeliverySettings::Default(const std::filesystem::path &cachePath) {
		DeliverySettings settings;
		settings.CachePath = cachePath;
		settings.Sources.push_back(
			Source{
				.Name = "localhost",
				.Kind = SourceKind::Http,
				.Location = "127.0.0.1:" + std::to_string(DEFAULT_ORIGIN_PORT),
				.Enabled = true,
				// **`Both`, because that is what every configuration written
				// before roles existed meant.** A default of `Read` would
				// quietly stop a single-origin setup from taking uploads, and
				// the symptom would be an upload button that does nothing.
				.Role = SourceRole::Both,
				.IngestKey = {},
			}
		);
		// No publisher key: there is no sensible default for a root of trust,
		// and inventing one would make an unconfigured client look configured.
		// IsValid refuses until somebody sets it.
		return settings;
	}

	namespace {
		// The shared half of `Usable` and `Writable`: enabled, valid, and on a
		// host the allow-list admits, in list order.
		//
		// One walk with a predicate rather than two walks, because the *order*
		// is the policy — see `SourceRole` — and two loops that drifted would
		// silently make uploads prefer a different origin than downloads for a
		// reason nobody wrote down.
		template <typename Predicate>
		std::vector<Source> Filtered(
			const std::vector<Source> &sources, const std::vector<std::string> &allowed, Predicate keep
		) {
			std::vector<Source> chosen;
			for (const Source &source : sources) {
				if (!source.Enabled || !source.IsValid() || !keep(source)) {
					continue;
				}
				if (source.Kind == SourceKind::Http && !HostPermitted(source.Location, allowed)) {
					continue;
				}
				chosen.push_back(source);
			}
			return chosen;
		}
	}

	std::vector<Source> DeliverySettings::Usable() const {
		return Filtered(Sources, AllowedHosts, [](const Source &source) { return source.Readable(); });
	}

	std::vector<Source> DeliverySettings::Writable() const {
		return Filtered(Sources, AllowedHosts, [](const Source &source) { return source.Writable(); });
	}

	bool DeliverySettings::IsValid() const {
		if (Publisher.IsZero()) {
			// Refused rather than read as "trust anything". A delivery client
			// that accepts an unsigned manifest has no trust boundary, and that
			// failure is invisible until somebody is serving content you did
			// not publish.
			return false;
		}
		return !Usable().empty();
	}
}
