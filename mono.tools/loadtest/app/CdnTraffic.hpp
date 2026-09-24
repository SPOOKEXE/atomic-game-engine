#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace loadtest {

	// Runs one bounded HTTP cohort against a published local store's origin.
	bool RunCdnTraffic(
		const std::filesystem::path &storePath,
		std::string_view publisherKey,
		std::string_view grantKey,
		std::string_view address,
		uint16_t port,
		uint32_t requests,
		uint32_t concurrency,
		std::string &error
	);

}
