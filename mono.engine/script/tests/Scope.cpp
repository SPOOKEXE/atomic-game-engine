#include <engine/script/Scope.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.script.scope")

using engine::script::ScopeHandle;
using engine::script::ScopeItem;
using engine::script::ScopeItemKind;
using engine::script::ScopeTable;

TEST_CASE("a scope cleans newest resources first", "[scope]") {
	ScopeTable scopes;
	const ScopeHandle scope = scopes.Create();
	CHECK(scopes.Add(scope, {ScopeItemKind::Callback, 1}));
	CHECK(scopes.Add(scope, {ScopeItemKind::Callback, 2}));
	CHECK(scopes.Add(scope, {ScopeItemKind::Callback, 3}));

	std::vector<ScopeItem> items;
	CHECK(scopes.Clean(scope, items));
	REQUIRE(items.size() == 3);
	CHECK(items[0].Value == 1);
	CHECK(items[1].Value == 2);
	CHECK(items[2].Value == 3);
	CHECK(scopes.Count(scope) == 0);
}

TEST_CASE("removing a scope resource preserves cleanup order", "[scope]") {
	ScopeTable scopes;
	const ScopeHandle scope = scopes.Create();
	scopes.Add(scope, {ScopeItemKind::Callback, 1});
	scopes.Add(scope, {ScopeItemKind::Callback, 2});
	scopes.Add(scope, {ScopeItemKind::Callback, 3});
	CHECK(scopes.Remove(scope, {ScopeItemKind::Callback, 2}));

	std::vector<ScopeItem> items;
	scopes.Clean(scope, items);
	REQUIRE(items.size() == 2);
	CHECK(items[0].Value == 1);
	CHECK(items[1].Value == 3);
}

TEST_CASE("destroy invalidates stale scope handles", "[scope]") {
	ScopeTable scopes;
	const ScopeHandle first = scopes.Create();
	std::vector<ScopeItem> items;
	CHECK(scopes.Destroy(first, items));
	CHECK_FALSE(scopes.IsAlive(first));
	CHECK_FALSE(scopes.Add(first, {ScopeItemKind::Callback, 1}));

	const ScopeHandle replacement = scopes.Create();
	CHECK(replacement.Index == first.Index);
	CHECK(replacement.Generation != first.Generation);
}
