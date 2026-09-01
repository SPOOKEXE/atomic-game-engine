#include <engine/datastore/Backend.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.datastore.backend")

TEST_CASE("datastore backend names round trip", "[datastore][backend]") {
	using engine::datastore::Backend;
	CHECK(engine::datastore::BackendOf(engine::datastore::Describe(Backend::Binary)) == Backend::Binary);
	CHECK(engine::datastore::BackendOf(engine::datastore::Describe(Backend::SQLite)) == Backend::SQLite);
	CHECK_FALSE(engine::datastore::BackendOf("sql"));
}
