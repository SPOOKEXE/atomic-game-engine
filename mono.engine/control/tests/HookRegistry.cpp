// The owned hook registry's atomic publication and guarded removal rules.

#include "HookFixture.hpp"

#include <engine/control/HookRegistry.hpp>
#include <engine/control/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.control.hook-registry")

using engine::control::HookDescriptor;
using engine::control::HookLease;
using engine::control::HookRegistration;
using engine::control::Prompt;
using engine::control::Resource;
using engine::control::Surface;
using engine::control::Tool;
using nlohmann::json;

namespace {
	json Ask(Surface &surface, std::string_view method, json parameters = json::object()) {
		return json::parse(surface.Answer(
			json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", parameters}}.dump()
		));
	}

	json Call(Surface &surface, std::string_view name) {
		const json reply = Ask(surface, "tools/call", {{"name", name}, {"arguments", json::object()}});
		return json::parse(reply["result"]["content"][0]["text"].get<std::string>());
	}

	HookDescriptor Descriptor(std::string id, std::vector<std::string> dependencies = {}) {
		return {
			.Id = std::move(id),
			.Revision = "v1",
			.Purpose = "Test hook.",
			.Dependencies = std::move(dependencies),
			.Limits = {}
		};
	}

	const json *Named(const json &rows, std::string_view name) {
		for (const json &row : rows) {
			if (row["name"] == name) return &row;
		}
		return nullptr;
	}
}

TEST_CASE("owned hooks publish atomically and report both collision owners", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	HookLease first = surface.Hooks().Activate(
		Descriptor("test.first"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"owned", "owned row", nullptr, [](const json &, std::string &) { return json{}; }});
		},
		failure
	);
	REQUIRE(failure.empty());
	REQUIRE(first.IsValid());

	HookLease duplicate = surface.Hooks().Activate(
		Descriptor("test.second"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"owned", "conflicting row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
			rows.Add(Tool{"must_not_publish", "later row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	CHECK_FALSE(duplicate.IsValid());
	CHECK(failure.find("owned") != std::string::npos);
	CHECK(failure.find("test.first") != std::string::npos);
	CHECK(failure.find("test.second") != std::string::npos);
	CHECK(surface.Count() == 1);
}

TEST_CASE("surface rows require an explicit owner and cannot replace an active hook", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	CHECK_THROWS(surface.Add(Tool{"unowned_tool", "unowned tool", nullptr, [](const json &, std::string &) {
									  return json{};
								  }}));
	CHECK_THROWS(surface.AddResource(
		Resource{"atomic://test/unowned", "unowned resource", "test", "text/plain", [](std::string &) {
					 return std::string{};
				 }}
	));
	CHECK_THROWS(
		surface.AddPrompt(Prompt{"unowned_prompt", "unowned prompt", {}, [](const json &, std::string &) {
									 return std::string{};
								 }})
	);
	CHECK(surface.Count() == 0);
	CHECK(surface.Readable().empty());
	CHECK(surface.Prompted().empty());
	CHECK(surface.Hooks().Active().empty());

	HookLease lease = surface.ActivateHook(
		Descriptor("test.owned-rows"),
		[&surface](HookRegistration &) {
			surface.Add(Tool{"owned_tool", "owned tool", nullptr, [](const json &, std::string &) {
								 return json{};
							 }});
			surface.AddResource(
				Resource{"atomic://test/owned", "owned resource", "test", "text/plain", [](std::string &) {
							 return std::string{};
						 }}
			);
			surface.AddPrompt(Prompt{"owned_prompt", "owned prompt", {}, [](const json &, std::string &) {
										 return std::string{};
									 }});
		},
		failure
	);
	REQUIRE(failure.empty());
	CHECK(surface.Hooks().OwnsTool("owned_tool"));
	CHECK(surface.Hooks().OwnsResource("atomic://test/owned"));
	CHECK(surface.Hooks().OwnsPrompt("owned_prompt"));
	CHECK(surface.Count() == 1);

	HookLease duplicate = surface.Hooks().Activate(
		Descriptor("test.duplicate-rows"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"owned_tool", "replacement", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
			rows.Add(Resource{"atomic://test/owned", "replacement", "test", "text/plain", [](std::string &) {
								  return std::string{};
							  }});
			rows.Add(Prompt{"owned_prompt", "replacement", {}, [](const json &, std::string &) {
								return std::string{};
							}});
		},
		failure
	);
	CHECK_FALSE(duplicate.IsValid());
	CHECK(failure.find("test.owned-rows") != std::string::npos);
	CHECK(failure.find("test.duplicate-rows") != std::string::npos);
}

TEST_CASE("hook resource and prompt collisions publish no rows", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	HookLease existing = surface.Hooks().Activate(
		Descriptor("test.existing-rows"),
		[](HookRegistration &rows) {
			rows.Add(Resource{"atomic://test/collision", "existing", "test", "text/plain", [](std::string &) {
								  return std::string{};
							  }});
			rows.Add(Prompt{"collision", "existing", {}, [](const json &, std::string &) {
								return std::string{};
							}});
		},
		failure
	);
	REQUIRE(existing.IsValid());
	HookLease resource = surface.Hooks().Activate(
		Descriptor("test.resource-collision"),
		[](HookRegistration &rows) {
			rows.Add(
				Resource{"atomic://test/collision", "collision", "test", "text/plain", [](std::string &) {
							 return std::string{};
						 }}
			);
			rows.Add(Tool{"must_not_publish", "later", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	CHECK_FALSE(resource.IsValid());
	CHECK(surface.Count() == 0);
	CHECK(surface.Readable().size() == 1);
	HookLease prompt = surface.Hooks().Activate(
		Descriptor("test.prompt-collision"),
		[](HookRegistration &rows) {
			rows.Add(Prompt{"collision", "collision", {}, [](const json &, std::string &) {
								return std::string{};
							}});
			rows.Add(Tool{"must_not_publish", "later", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	CHECK_FALSE(prompt.IsValid());
	CHECK(surface.Count() == 0);
	CHECK(surface.Prompted().size() == 1);
}

TEST_CASE("negotiate reports active hook metadata and control generation", "[control][hooks][discovery]") {
	Surface surface("test", "hook registry");
	engine::control::test::Install(surface, std::array{engine::control::test::Discovery()});
	const json builtin = Call(surface, "negotiate");
	CHECK(builtin["control_generation"] == 1);
	REQUIRE(builtin["hooks"].size() == 1);
	CHECK(builtin["hooks"][0]["id"] == "builtin.discovery");
	CHECK(builtin["hooks"][0]["tools"] == json::array({"negotiate"}));
	std::string failure;
	HookLease lease = surface.Hooks().Activate(
		{.Id = "test.discovery",
		 .Revision = "v7",
		 .Purpose = "Discovery test hook.",
		 .Dependencies = {},
		 .Limits = {{"records", 12}, {"bytes", 4096}}},
		[](HookRegistration &rows) {
			rows.Add(Tool{"hook_probe", "hook row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(failure.empty());
	const json active = Call(surface, "negotiate");
	CHECK(active["control_generation"] == 2);
	REQUIRE(active["hooks"].size() == 2);
	CHECK(active["hooks"][0]["id"] == "builtin.discovery");
	CHECK(active["hooks"][1]["id"] == "test.discovery");
	CHECK(active["hooks"][1]["state"] == "active");
	CHECK(active["hooks"][1]["revision"] == "v7");
	CHECK(active["hooks"][1]["tools"] == json::array({"hook_probe"}));
	CHECK(active["hooks"][1]["limits"] == json{{"records", 12}, {"bytes", 4096}});

	lease.Close();
	const json removed = Call(surface, "negotiate");
	CHECK(removed["control_generation"] == 4);
	REQUIRE(removed["hooks"].size() == 1);
	CHECK(removed["hooks"][0]["id"] == "builtin.discovery");
}

TEST_CASE("negotiate hides draining hook tools with tools/list", "[control][hooks][discovery]") {
	Surface surface("test", "hook registry");
	engine::control::test::Install(surface, std::array{engine::control::test::Discovery()});
	bool terminal = false;
	std::string failure;
	HookLease lease = surface.ActivateHook(
		Descriptor("test.discovery-drain"),
		[&terminal](HookRegistration &rows) {
			rows.SetDrain([&terminal] { return terminal; });
			rows.Add(Tool{"capture", "draining discovery row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(failure.empty());

	lease.Close();
	const json tools = Ask(surface, "tools/list")["result"]["tools"];
	const json negotiated = Call(surface, "negotiate");
	CHECK(tools.size() == negotiated["operations"].size());
	CHECK(Named(tools, "negotiate") != nullptr);
	CHECK(Named(negotiated["operations"], "negotiate") != nullptr);
	CHECK(Named(tools, "capture") == nullptr);
	CHECK(Named(negotiated["operations"], "capture") == nullptr);
	CHECK(Named(negotiated["unsupported_operations"], "capture") != nullptr);
	CHECK(Call(surface, "capture")["error"] == "hook is draining: capture");

	terminal = true;
	surface.PumpHooks();
	REQUIRE(surface.Hooks().Active().size() == 1);
	CHECK(surface.Hooks().Active()[0].Descriptor.Id == "builtin.discovery");
	CHECK(surface.Count() == 1);
}

TEST_CASE("draining hooks expose only named cleanup calls", "[control][hooks][discovery]") {
	Surface surface("test", "hook registry");
	engine::control::test::Install(surface, std::array{engine::control::test::Discovery()});
	bool released = false;
	std::string failure;
	HookLease lease = surface.ActivateHook(
		Descriptor("test.cleanup"),
		[&released](HookRegistration &rows) {
			rows.SetDrain([&released] { return released; });
			rows.Add(Tool{"submit", "submit", nullptr, [](const json &, std::string &) {
							  return json{{"status", "queued"}};
						  }});
			rows.Add(Tool{"release", "release", nullptr, [&released](const json &, std::string &) {
							  released = true;
							  return json{{"status", "released"}};
						  }});
			rows.KeepToolDuringDrain("release");
		},
		failure
	);
	REQUIRE(lease.IsValid());
	const uint64_t beforeClose = surface.Hooks().ControlGeneration();
	lease.Close();
	CHECK(surface.Hooks().ControlGeneration() == beforeClose + 1);
	CHECK(Named(Ask(surface, "tools/list")["result"]["tools"], "submit") == nullptr);
	CHECK(Named(Ask(surface, "tools/list")["result"]["tools"], "release") != nullptr);
	CHECK(Call(surface, "submit")["error"] == "hook is draining: submit");
	const json draining = Call(surface, "negotiate");
	const auto hook = std::find_if(draining["hooks"].begin(), draining["hooks"].end(), [](const json &row) {
		return row["id"] == "test.cleanup";
	});
	REQUIRE(hook != draining["hooks"].end());
	CHECK((*hook)["state"] == "draining");
	CHECK((*hook)["tools"] == json::array({"release"}));
	CHECK(Call(surface, "release")["status"] == "released");
	CHECK_FALSE(lease.IsValid());
	CHECK(Named(Ask(surface, "tools/list")["result"]["tools"], "release") == nullptr);
}

TEST_CASE("hook release owns provider callbacks through failed and closed activation", "[control][hooks]") {
	Surface surface("test", "hook registry");
	bool released = false;
	std::string failure;
	auto lease = surface.Hooks().Activate(
		Descriptor("test.release"),
		[&released](HookRegistration &rows) {
			rows.SetRelease([&released] { released = true; });
			rows.Add(Tool{"release_probe", "test", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(lease.IsValid());
	CHECK_FALSE(released);

	bool rejectedReleased = false;
	auto collision = surface.Hooks().Activate(
		Descriptor("test.release-collision"),
		[&rejectedReleased](HookRegistration &rows) {
			rows.SetRelease([&rejectedReleased] { rejectedReleased = true; });
			rows.Add(Tool{"release_probe", "test", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	CHECK_FALSE(collision.IsValid());
	CHECK(rejectedReleased);
	CHECK_FALSE(released);
	lease.Close();
	CHECK(released);
}

TEST_CASE("owned dependencies keep their providers active until children close", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	HookLease parent = surface.Hooks().Activate(
		Descriptor("test.parent"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"parent", "parent row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(failure.empty());
	HookLease child = surface.Hooks().Activate(
		Descriptor("test.child", {"test.parent"}),
		[](HookRegistration &rows) {
			rows.Add(Tool{"child", "child row", nullptr, [](const json &, std::string &) { return json{}; }});
		},
		failure
	);
	REQUIRE(failure.empty());
	parent.Close();
	CHECK(surface.Count() == 2);
	parent = {};
	child.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(surface.Count() == 0);
}

TEST_CASE("owned hooks refuse inactive dependencies and repeated close is safe", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	HookLease missing = surface.Hooks().Activate(Descriptor("test.child", {"test.parent"}), {}, failure);
	CHECK_FALSE(missing.IsValid());
	CHECK(failure.find("test.parent") != std::string::npos);

	HookLease lease = surface.Hooks().Activate(
		Descriptor("test.parent"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"parent", "parent row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(failure.empty());
	lease.Close();
	lease.Close();
	CHECK(surface.Count() == 0);
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("a self-closing tool remains guarded until its call completes", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	std::optional<HookLease> lease;
	lease.emplace(surface.Hooks().Activate(
		Descriptor("test.tool"),
		[&lease](HookRegistration &rows) {
			rows.Add(Tool{"self_close", "closes itself", nullptr, [&lease](const json &, std::string &) {
							  lease->Close();
							  return json{{"complete", true}};
						  }});
		},
		failure
	));
	REQUIRE(failure.empty());
	CHECK(Call(surface, "self_close")["complete"]);
	CHECK(surface.Count() == 0);
	CHECK(Ask(surface, "tools/list")["result"]["tools"].empty());
}

TEST_CASE("a draining child retains its parent through the callback that closes both", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	std::optional<HookLease> parent;
	std::optional<HookLease> child;
	parent.emplace(surface.Hooks().Activate(
		Descriptor("test.parent"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"parent", "parent row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	));
	REQUIRE(failure.empty());
	child.emplace(surface.Hooks().Activate(
		Descriptor("test.child", {"test.parent"}),
		[&child, &parent](HookRegistration &rows) {
			rows.Add(
				Tool{
					"close_both",
					"closes child and parent",
					nullptr,
					[&child, &parent](const json &, std::string &) {
						child->Close();
						parent->Close();
						return json{{"complete", true}};
					}
				}
			);
		},
		failure
	));
	REQUIRE(failure.empty());
	CHECK(Call(surface, "close_both")["complete"]);
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("owned resource and prompt callbacks hold their hook lease", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	std::optional<HookLease> resourceLease;
	resourceLease.emplace(surface.Hooks().Activate(
		Descriptor("test.resource"),
		[&resourceLease](HookRegistration &rows) {
			rows.Add(
				Resource{
					"atomic://test/resource",
					"resource",
					"test",
					"text/plain",
					[&resourceLease](std::string &) {
						resourceLease->Close();
						return std::string("ready");
					}
				}
			);
		},
		failure
	));
	REQUIRE(failure.empty());
	CHECK(
		Ask(surface,
			"resources/read",
			{{"uri", "atomic://test/resource"}})["result"]["contents"][0]["text"] == "ready"
	);
	CHECK(surface.Readable().empty());

	std::optional<HookLease> promptLease;
	promptLease.emplace(surface.Hooks().Activate(
		Descriptor("test.prompt"),
		[&promptLease](HookRegistration &rows) {
			rows.Add(Prompt{"prompt", "test", {}, [&promptLease](const json &, std::string &) {
								promptLease->Close();
								return std::string("ready");
							}});
		},
		failure
	));
	REQUIRE(failure.empty());
	CHECK(
		Ask(surface, "prompts/get", {{"name", "prompt"}})["result"]["messages"][0]["content"]["text"] ==
		"ready"
	);
	CHECK(surface.Prompted().empty());
}

TEST_CASE("draining hooks hide every row until a terminal pump", "[control][hooks]") {
	Surface surface("test", "hook registry");
	bool terminal = false;
	std::string failure;
	auto lease = surface.ActivateHook(
		Descriptor("test.drain"),
		[&terminal](HookRegistration &rows) {
			rows.SetDrain([&terminal] { return terminal; });
			rows.Add(Tool{"draining_tool", "test", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
			rows.Add(Resource{"atomic://test/draining", "test", "test", "text/plain", [](std::string &) {
								  return std::string{};
							  }});
			rows.Add(Prompt{"draining_prompt", "test", {}, [](const json &, std::string &) {
								return std::string{};
							}});
		},
		failure
	);
	REQUIRE(lease.IsValid());
	lease.Close();
	lease.Close();
	CHECK(Ask(surface, "tools/list")["result"]["tools"].empty());
	CHECK(Ask(surface, "resources/list")["result"]["resources"].empty());
	CHECK(Ask(surface, "prompts/list")["result"]["prompts"].empty());
	CHECK(surface.Hooks().Active().size() == 1);
	surface.PumpHooks();
	CHECK(surface.Hooks().Active().size() == 1);
	terminal = true;
	surface.PumpHooks();
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("a draining provider cannot reactivate before its generation is removed", "[control][hooks]") {
	Surface surface("test", "hook registry");
	bool terminal = false;
	std::string failure;
	auto first = surface.Hooks().Activate(
		Descriptor("test.reactivate"),
		[&terminal](HookRegistration &rows) { rows.SetDrain([&terminal] { return terminal; }); },
		failure
	);
	REQUIRE(first.IsValid());
	first.Close();
	CHECK(surface.Hooks().Active().front().State == engine::control::HookState::Draining);

	auto duplicate = surface.Hooks().Activate(Descriptor("test.reactivate"), {}, failure);
	CHECK_FALSE(duplicate.IsValid());
	CHECK(failure == "hook is already active or draining: test.reactivate");

	terminal = true;
	surface.PumpHooks();
	auto replacement = surface.Hooks().Activate(Descriptor("test.reactivate"), {}, failure);
	CHECK(replacement.IsValid());
}

TEST_CASE("closing a provider stops new calls while its dependant drains", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	auto provider = surface.Hooks().Activate(
		Descriptor("test.provider"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"provider", "provider row", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(provider.IsValid());
	auto dependant = surface.Hooks().Activate(Descriptor("test.dependant", {"test.provider"}), {}, failure);
	REQUIRE(dependant.IsValid());

	provider.Close();
	CHECK(surface.Hooks().Active().front().State == engine::control::HookState::Draining);
	CHECK(Call(surface, "provider")["error"] == "hook is draining: provider");
	CHECK(surface.Hooks().Active().size() == 2);

	dependant.Close();
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("a throwing drain predicate leaves the registry usable", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	auto lease = surface.ActivateHook(
		Descriptor("test.throwing-drain"),
		[](HookRegistration &rows) {
			rows.SetDrain([]() -> bool { throw std::runtime_error("drain failure"); });
			rows.Add(Tool{"throwing_drain", "test", nullptr, [](const json &, std::string &) {
							  return json{};
						  }});
		},
		failure
	);
	REQUIRE(lease.IsValid());
	CHECK_NOTHROW(lease.Close());
	CHECK_NOTHROW(surface.PumpHooks());
	CHECK(surface.Hooks().Active().size() == 1);
}

TEST_CASE("built-in registrations cannot replace another built-in row", "[control][hooks]") {
	Surface surface("test", "hook registry");
	engine::control::test::Install(
		surface, std::array{engine::control::test::Custom("first", [](Surface &owner) {
			owner.Add(Tool{"duplicate", "test", nullptr, [](const json &, std::string &) { return json{}; }});
		})}
	);
	CHECK_THROWS(
		engine::control::test::Install(
			surface, std::array{engine::control::test::Custom("second", [](Surface &owner) {
				owner.Add(Tool{"duplicate", "test", nullptr, [](const json &, std::string &) {
								   return json{};
							   }});
			})}
		)
	);
	CHECK(surface.Registered().size() == 1);
}

TEST_CASE("stale copied hook callbacks cannot run a newer owner", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	auto first = surface.Hooks().Activate(
		Descriptor("test.first"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"same", "test", nullptr, [](const json &, std::string &) {
							  return json{{"owner", "first"}};
						  }});
		},
		failure
	);
	REQUIRE(first.IsValid());
	const auto stale =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const Tool &tool) {
			return tool.Name == "same";
		})->Call;
	first.Close();
	auto second = surface.Hooks().Activate(
		Descriptor("test.second"),
		[](HookRegistration &rows) {
			rows.Add(Tool{"same", "test", nullptr, [](const json &, std::string &) {
							  return json{{"owner", "second"}};
						  }});
		},
		failure
	);
	REQUIRE(second.IsValid());
	CHECK(stale(json::object(), failure).is_null());
	CHECK(failure == "hook is draining: same");
}

TEST_CASE("stale copied resources and prompts cannot run a newer owner", "[control][hooks]") {
	Surface surface("test", "hook registry");
	std::string failure;
	auto first = surface.Hooks().Activate(
		Descriptor("test.first"),
		[](HookRegistration &rows) {
			rows.Add(Resource{"atomic://same", "test", "test", "text/plain", [](std::string &) {
								  return std::string("first");
							  }});
			rows.Add(Prompt{"same", "test", {}, [](const json &, std::string &) {
								return std::string("first");
							}});
		},
		failure
	);
	REQUIRE(first.IsValid());
	const auto staleResource = surface.Readable().front().Read;
	const auto stalePrompt = surface.Prompted().front().Render;
	first.Close();
	auto second = surface.Hooks().Activate(
		Descriptor("test.second"),
		[](HookRegistration &rows) {
			rows.Add(Resource{"atomic://same", "test", "test", "text/plain", [](std::string &) {
								  return std::string("second");
							  }});
			rows.Add(Prompt{"same", "test", {}, [](const json &, std::string &) {
								return std::string("second");
							}});
		},
		failure
	);
	REQUIRE(second.IsValid());
	CHECK(staleResource(failure).empty());
	CHECK(failure == "hook is draining: atomic://same");
	CHECK(stalePrompt(json::object(), failure).empty());
	CHECK(failure == "hook is draining: same");
}
