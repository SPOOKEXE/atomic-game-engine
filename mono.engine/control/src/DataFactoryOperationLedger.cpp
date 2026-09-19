#include <engine/control/DataFactoryOperationLedger.hpp>

#include <algorithm>
#include <utility>

namespace engine::control {

	DataFactoryOperationReplay DataFactoryOperationLedger::Replay(
		std::string_view tool,
		std::string_view operationId,
		std::string_view arguments,
		nlohmann::json &result,
		std::string &failure
	) const {
		const auto found = Entries.find(std::string(operationId));
		if (found == Entries.end()) {
			if (Entries.size() >= MAXIMUM_ENTRIES) {
				failure = "operation_id_capacity: operation ledger is full; retry an existing operation_id";
				return DataFactoryOperationReplay::Conflict;
			}
			return DataFactoryOperationReplay::Fresh;
		}
		if (found->second.Tool != tool || found->second.Arguments != arguments) {
			failure = "operation_id_conflict: operation_id was already used with different tool or arguments";
			return DataFactoryOperationReplay::Conflict;
		}
		result = found->second.Result;
		failure = found->second.Failure;
		return DataFactoryOperationReplay::Replay;
	}

	void DataFactoryOperationLedger::Store(
		std::string tool,
		std::string operationId,
		std::string arguments,
		nlohmann::json result,
		std::string failure
	) {
		if (Entries.contains(operationId) || Entries.size() >= MAXIMUM_ENTRIES) return;
		Order.push_back(operationId);
		Entries.emplace(
			std::move(operationId),
			Entry{std::move(tool), std::move(arguments), std::move(result), std::move(failure)}
		);
	}

	void DataFactoryOperationLedger::Update(
		std::string_view operationId, nlohmann::json result, std::string failure
	) {
		const auto found = Entries.find(std::string(operationId));
		if (found == Entries.end()) return;
		found->second.Result = std::move(result);
		found->second.Failure = std::move(failure);
	}

	std::vector<DataFactoryOperationAudit> DataFactoryOperationLedger::Recent(size_t maximum) const {
		maximum = std::min(maximum, MAXIMUM_ENTRIES);
		std::vector<DataFactoryOperationAudit> rows;
		rows.reserve(std::min(maximum, Order.size()));
		for (auto entry = Order.rbegin(); entry != Order.rend() && rows.size() < maximum; ++entry) {
			const auto found = Entries.find(*entry);
			if (found == Entries.end()) continue;
			rows.push_back(
				{found->second.Tool,
				 *entry,
				 found->second.Result.value(
					 "status", found->second.Failure.empty() ? "completed" : "refused"
				 ),
				 !found->second.Failure.empty()}
			);
		}
		return rows;
	}
}
