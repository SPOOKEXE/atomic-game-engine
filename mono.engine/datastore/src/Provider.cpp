#include <engine/datastore/Provider.hpp>

namespace engine::datastore {
	const char *Describe(const Provider provider) {
		switch (provider) {
		case Provider::File:
			return "file";
		case Provider::Http:
			return "http";
		}
		return "unknown";
	}

	std::optional<Provider> ProviderOf(const std::string_view text) {
		if (text == "file") {
			return Provider::File;
		}
		if (text == "http") {
			return Provider::Http;
		}
		return std::nullopt;
	}
}
