#include <engine/datastore/Provider.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.datastore.provider")

TEST_CASE("datastore provider names round trip", "[datastore][provider]") {
	using engine::datastore::Provider;
	CHECK(engine::datastore::ProviderOf(engine::datastore::Describe(Provider::File)) == Provider::File);
	CHECK(engine::datastore::ProviderOf(engine::datastore::Describe(Provider::Http)) == Provider::Http);
	CHECK_FALSE(engine::datastore::ProviderOf("supabase"));
}
