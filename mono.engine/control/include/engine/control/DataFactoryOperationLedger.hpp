#pragma once

// Bounded replay and audit state shared by every data-factory mutation tool on
// one control surface. It intentionally stores canonical requests only inside
// the process: callers can inspect outcomes, but not replay raw arguments from
// another client session.

#include <cstddef>
#include <deque>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::control {

	// The result of looking up an externally supplied operation identifier.
	enum class DataFactoryOperationReplay {
		Fresh,
		Replay,
		Conflict,
	};

	// One bounded, redacted audit row for a data-factory operation.
	struct DataFactoryOperationAudit {
		// MCP tool name that consumed OperationId.
		std::string Tool;
		// Caller supplied idempotency key retained for this surface lifetime.
		std::string OperationId;
		// Terminal status reported by the operation's most recent result.
		std::string Status;
		// True when the tool returned a refusal instead of an accepted outcome.
		bool Refused = false;
	};

	// Shares operation identifiers across lifecycle and capture tools on one MCP
	// surface. The identifier is single-use across tool names so a retry cannot
	// silently become a different action after an agent changes tools.
	class DataFactoryOperationLedger final {
	  public:
		// Every accepted identifier remains fenced for this surface lifetime.
		// Admission refuses a new identifier once this bound is reached, rather
		// than forgetting an older lifecycle operation and allowing it to recur.
		static constexpr size_t MAXIMUM_ENTRIES = 256;

		// Replays a prior result only when tool name and canonical arguments match its key.
		DataFactoryOperationReplay Replay(
			std::string_view tool,
			std::string_view operationId,
			std::string_view arguments,
			nlohmann::json &result,
			std::string &failure
		) const;

		// Claims an operation key and saves its canonical request, result, and failure text.
		void Store(
			std::string tool,
			std::string operationId,
			std::string arguments,
			nlohmann::json result,
			std::string failure
		);

		// Refreshes an asynchronous operation's terminal result without changing
		// the canonical request that owns its operation identifier.
		void Update(std::string_view operationId, nlohmann::json result, std::string failure);

		// Returns at most maximum newest-first redacted audit rows.
		std::vector<DataFactoryOperationAudit> Recent(size_t maximum) const;

	  private:
		struct Entry {
			std::string Tool;
			std::string Arguments;
			nlohmann::json Result;
			std::string Failure;
		};

		std::unordered_map<std::string, Entry> Entries;
		std::deque<std::string> Order;
	};
}
