#include <engine/control/DataFactoryOperationLedger.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.control.datafactoryoperationledger")

TEST_CASE("operation ledger fences accepted IDs when its capacity is exhausted", "[control][data-factory]") {
	using engine::control::DataFactoryOperationLedger;
	using engine::control::DataFactoryOperationReplay;
	DataFactoryOperationLedger ledger;
	for (size_t index = 0; index < DataFactoryOperationLedger::MAXIMUM_ENTRIES; ++index) {
		const std::string id = "operation-" + std::to_string(index);
		ledger.Store("create_world", id, "{\"id\":" + std::to_string(index) + "}", {{"status", "ok"}}, {});
	}

	nlohmann::json result;
	std::string failure;
	CHECK(
		ledger.Replay("create_world", "operation-0", "{\"id\":0}", result, failure) ==
		DataFactoryOperationReplay::Replay
	);
	CHECK(result.at("status") == "ok");
	CHECK(failure.empty());

	result = {};
	failure.clear();
	CHECK(
		ledger.Replay("create_world", "operation-new", "{\"id\":256}", result, failure) ==
		DataFactoryOperationReplay::Conflict
	);
	CHECK(failure == "operation_id_capacity: operation ledger is full; retry an existing operation_id");

	ledger.Update("operation-0", {{"status", "completed"}}, {});
	result = {};
	failure.clear();
	CHECK(
		ledger.Replay("create_world", "operation-0", "{\"id\":0}", result, failure) ==
		DataFactoryOperationReplay::Replay
	);
	CHECK(result.at("status") == "completed");
	CHECK(ledger.Recent(DataFactoryOperationLedger::MAXIMUM_ENTRIES).size() == DataFactoryOperationLedger::MAXIMUM_ENTRIES);
}
