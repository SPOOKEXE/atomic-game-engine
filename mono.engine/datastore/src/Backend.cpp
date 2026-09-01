#include <engine/datastore/Backend.hpp>

namespace engine::datastore {
	const char *Describe(const Backend backend) {
		switch (backend) {
		case Backend::Binary:
			return "binary";
		case Backend::SQLite:
			return "sqlite";
		}
		return "unknown";
	}

	std::optional<Backend> BackendOf(const std::string_view text) {
		if (text == "binary") {
			return Backend::Binary;
		}
		if (text == "sqlite") {
			return Backend::SQLite;
		}
		return std::nullopt;
	}
}
