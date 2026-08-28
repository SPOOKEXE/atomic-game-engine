// What the catalogue claims, against what the two VMs actually install.
//
// **This suite is the reason the catalogue is worth having.** A single list of
// services is only an improvement over two lists if something checks it against
// reality - otherwise it is a third place the same fact is written, and the
// least trustworthy of the three, because a row costs nothing to add and nothing
// to get wrong. What went before was exactly that failure without the list:
// Luau bound nine surface services and JavaScript bound five, four were
// reachable from one language and not the other, and the TypeScript declarations
// claimed two of the four anyway. Nobody noticed for two versions.
//
// So every case here asks a running VM rather than reading a header:
//
//   - a service the row says this language binds must be reachable by name
//   - a service it says the language does not bind must refuse, and the refusal
//     must say *which* language has it rather than "no such service"
//   - a name that is in no row at all must still get the old refusal
//
// **`game:GetService(name)` is the question, not the global.** That is what a
// script actually writes, it is the one path both languages share, and it is the
// only place a script can tell "absent" from "absent here".

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/ServiceCatalogue.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scripthost.servicecatalogue")

using engine::ecs::Store;
using engine::script::Binds;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;
using engine::script::RuntimeLimits;
using engine::script::ScriptCapabilities;
using engine::script::ScriptOrigin;
using engine::script::ServiceAvailability;
using engine::script::ServiceDefinition;
using engine::script::ServiceLanguages;
using engine::script::Services;

namespace {
	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	// The one expression both languages spell nearly the same, so the two halves
	// of every case below differ by a colon and nothing else.
	std::string Fetch(Language language, const std::string &name) {
		return language == Language::Luau ? "return game:GetService('" + name + "') ~= nil"
										  : "game.GetService('" + name + "') !== undefined";
	}

	std::unique_ptr<Runtime> FullRuntime(Store &store, Language language) {
		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();
		limits.Role.Studio = true;
		return MakeRuntime(store, language, limits);
	}
}

TEST_CASE("every service the catalogue claims for a language is reachable in it", "[scripting][services]") {
	// **The claim, checked against a live VM.** A row is a promise that a
	// script can reach that name; nothing but running one can keep it honest.
	//
	// **Studio rows are skipped and that is not a hole.** `BreakpointService`
	// installs only when the runtime carries a debugger, which a plain
	// `MakeRuntime` does not - `engine.script.debugger` is where that pairing is
	// pinned, and asserting it here would be asserting the absence of a
	// debugger.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		const ServiceLanguages want =
			language == Language::Luau ? ServiceLanguages::Luau : ServiceLanguages::JavaScript;

		Store store = Fresh("catalogue_present");
		const auto runtime = FullRuntime(store, language);
		REQUIRE(runtime != nullptr);

		for (const ServiceDefinition &definition : Services()) {
			if (definition.Availability != ServiceAvailability::Always) {
				continue;
			}
			if (!Binds(definition.Languages, want)) {
				continue;
			}

			INFO(definition.Name);
			INFO((language == Language::Luau ? "luau" : "javascript"));
			const bool ok = runtime->Run(Fetch(language, definition.Name).c_str());
			INFO(runtime->LastError());
			CHECK(ok);
		}
	}
}

TEST_CASE("runtime profiles gate host-sensitive services", "[scripting][services][security]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		SECTION(language == Language::Luau ? "luau" : "javascript") {
			Store serverStore = Fresh("capability_server");
			RuntimeLimits serverLimits;
			serverLimits.Role = engine::script::HostRole::OfServer();
			const auto server = MakeRuntime(serverStore, language, serverLimits);
			REQUIRE(server != nullptr);
			CHECK(server->Can(ScriptCapabilities::Messaging));
			CHECK(server->Can(ScriptCapabilities::Input));
			CHECK(server->Run(Fetch(language, "MessagingService").c_str()));
			CHECK(server->Run(Fetch(language, "UserInputService").c_str()));

			Store clientStore = Fresh("capability_client");
			RuntimeLimits clientLimits;
			clientLimits.Role = engine::script::HostRole::OfClient();
			const auto client = MakeRuntime(clientStore, language, clientLimits);
			REQUIRE(client != nullptr);
			CHECK(client->Can(ScriptCapabilities::Input));
			CHECK_FALSE(client->Can(ScriptCapabilities::Persistence));
			CHECK(client->Run(Fetch(language, "ContextActionService").c_str()));
			CHECK_FALSE(client->Run(Fetch(language, "DataStoreService").c_str()));
			CHECK(client->LastError().find("'persistence' script capability") != std::string::npos);

			Store pluginStore = Fresh("capability_plugin");
			RuntimeLimits pluginLimits;
			pluginLimits.Role.Server = false;
			pluginLimits.Role.Client = false;
			pluginLimits.Role.Studio = true;
			pluginLimits.Origin = ScriptOrigin::Plugin;
			const auto plugin = MakeRuntime(pluginStore, language, pluginLimits);
			REQUIRE(plugin != nullptr);
			CHECK(plugin->Can(ScriptCapabilities::PluginHost));
			CHECK(plugin->Can(ScriptCapabilities::StudioDebug));
			CHECK_FALSE(plugin->Can(ScriptCapabilities::Messaging));
			CHECK_FALSE(plugin->Run(Fetch(language, "CrossWorldService").c_str()));
			CHECK(plugin->LastError().find("'messaging' script capability") != std::string::npos);
		}
	}
}

TEST_CASE("an explicit capability set narrows the automatic profile", "[scripting][services][security]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Store store = Fresh("capability_override");
		RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();
		limits.Capabilities = ScriptCapabilities::World;

		const auto runtime = MakeRuntime(store, language, limits);
		REQUIRE(runtime != nullptr);
		CHECK(runtime->Run(Fetch(language, "RunService").c_str()));
		CHECK_FALSE(runtime->Run(Fetch(language, "UserInputService").c_str()));
		CHECK(runtime->LastError().find("'input' script capability") != std::string::npos);
		CHECK_FALSE(runtime->Run(Fetch(language, "DataStoreService").c_str()));
		CHECK(runtime->LastError().find("'persistence' script capability") != std::string::npos);
	}
}

TEST_CASE("every surface service is now in both languages", "[scripting][services]") {
	// **What this case used to be, and why it is this instead.** It ran the
	// services one language does *not* bind and asserted the refusal named the
	// other - with a landmine on the end saying the JavaScript half must find
	// exactly `{SoundService, UserInputService}`, set deliberately so that
	// closing them could not land silently. They are closed: `ServiceProperty`
	// gave a live property a neutral shape, both VMs install both services, and
	// the loop that walked the gap now walks nothing.
	//
	// **The landmine is replaced rather than deleted, because the fact it
	// guarded is still worth guarding** - it has simply become the opposite
	// fact. Every `Always` row binds both languages, and the set below is empty
	// and *named*: un-bind one and this fails saying which, exactly as adding one
	// used to.
	//
	// The refusal path itself is not lost with it - see the case below, which
	// exercises it against the one row that is genuinely one language's.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		const ServiceLanguages want =
			language == Language::Luau ? ServiceLanguages::Luau : ServiceLanguages::JavaScript;

		std::set<std::string> unbound;
		for (const ServiceDefinition &definition : Services()) {
			if (definition.Availability != ServiceAvailability::Always) {
				continue;
			}
			if (!Binds(definition.Languages, want)) {
				unbound.insert(definition.Name);
			}
		}

		INFO((language == Language::Luau ? "luau" : "javascript"));
		CHECK(unbound == std::set<std::string>{});
	}
}

TEST_CASE("a service the other language binds refuses by name", "[scripting][services]") {
	// **The half that makes the mask worth carrying.** Saying "no such service"
	// for something that exists and is bound elsewhere sends an author to check
	// their spelling, which is the one place the answer is not.
	//
	// **`BreakpointService` is the one row left in that position, and it is a
	// feature gap rather than a binding one** - `Debugger::Add` refuses a `.js`,
	// `.mjs`, `.cjs`, `.ts` or `.tsx` chunk outright, so a JavaScript binding
	// would answer "nothing can be armed" to everything. It is a `Studio` row, so
	// a plain `MakeRuntime` installs it in neither VM; what is being asserted
	// here is the *catalogue's* answer, which is a language refusal in JavaScript
	// and an ordinary absence in Luau.
	Store store = Fresh("catalogue_absent");
	const auto runtime = FullRuntime(store, Language::JavaScript);
	REQUIRE(runtime != nullptr);

	const ServiceDefinition *breakpoints = engine::script::FindService("BreakpointService");
	REQUIRE(breakpoints != nullptr);
	REQUIRE_FALSE(Binds(breakpoints->Languages, ServiceLanguages::JavaScript));

	CHECK_FALSE(runtime->Run(Fetch(Language::JavaScript, "BreakpointService").c_str()));
	CHECK(runtime->LastError().find("not bound for JavaScript") != std::string::npos);
}

TEST_CASE("a name in no row still fails the old way", "[scripting][services]") {
	// **The catalogue must not swallow a typo.** A refusal naming a language is
	// only useful because it is *narrower* than the general one; if every miss
	// got it, it would say nothing. `MarketplaceService` is a real Roblox
	// service this engine does not have, which is exactly the case an author
	// migrating a place hits.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Store store = Fresh("catalogue_unknown");
		const auto runtime = FullRuntime(store, language);
		REQUIRE(runtime != nullptr);

		CHECK_FALSE(runtime->Run(Fetch(language, "MarketplaceService").c_str()));
		CHECK(runtime->LastError().find("not a service this engine provides") != std::string::npos);
	}
}

TEST_CASE("every service member is reachable in both languages", "[scripting][services]") {
	// **The case that would have caught `GetTeleportData`.** The three above ask
	// whether a *service* is reachable, which is what the catalogue claims; this
	// asks whether every *member* of one is, which the catalogue cannot claim
	// because a language mask is per service. `TeleportService::GetTeleportData`
	// lived in that gap from v0.15 to v0.16 - declared in `engine.d.ts`, present
	// in Luau, absent in JavaScript - and so did `ContextActionService`'s two
	// reporting methods.
	//
	// **A hand-written list, deliberately, and it is the point rather than a
	// duplicate.** Reading the surfaces back would assert that the installers
	// install what the surfaces say, which is a tautology: both walks read the
	// same span. What is worth checking is that the *engine* offers what a
	// script author was told it offers, so the list is written here from the
	// declaration files and asked of a running VM.
	struct Surface {
		const char *Service;

		// Members that must be callable.
		std::vector<const char *> Methods;

		// Signals and live properties: reachable and not functions.
		std::vector<const char *> Values;
	};

	const std::vector<Surface> SURFACES = {
		{"MessagingService", {"PublishAsync", "SubscribeAsync"}, {}},
		{"TeleportService", {"Teleport", "GetLocalPlayerTeleportData", "GetTeleportData"}, {}},
		{"MemoryStoreService", {"GetAsync", "SetAsync", "UpdateAsync", "RemoveAsync"}, {}},
		{"DataStoreService", {"GetAsync", "SetAsync", "RemoveAsync"}, {}},
		{"CrossWorldService", {"OpenChannel", "CloseChannel", "SendAsync"}, {}},
		{"RunService", {"IsServer", "IsClient", "IsStudio", "IsReplica"}, {"Heartbeat"}},
		{"UserInputService",
		 {"IsKeyDown",
		  "IsMouseButtonPressed",
		  "GetMouseLocation",
		  "GetMouseDelta",
		  "GetKeysPressed",
		  "GetMouseButtonsPressed",
		  "GetLastInputType"},
		 {"MouseBehavior",
		  "MouseIconEnabled",
		  "MouseDeltaSensitivity",
		  "KeyboardEnabled",
		  "MouseEnabled",
		  "GamepadEnabled",
		  "TouchEnabled",
		  "VREnabled",
		  "AccelerometerEnabled",
		  "GyroscopeEnabled",
		  "InputBegan",
		  "InputEnded",
		  "InputChanged",
		  "WindowFocused",
		  "WindowFocusReleased",
		  "LastInputTypeChanged"}},
		{"SoundService", {"GetListener", "SetListener"}, {"Volume"}},
		{"ContextActionService",
		 {"BindAction",
		  "BindActionAtPriority",
		  "UnbindAction",
		  "UnbindAllActions",
		  "GetBoundActionInfo",
		  "GetAllBoundActionInfo"},
		 {}},
		{"ContentService",
		 {"GetMeshes",
		  "GetPublishedMeshes",
		  "GetMeshTextures",
		  "GetTextures",
		  "GetFlipbook",
		  "GetTriangleCount"},
		 {}},
		{"CollectionService", {"AddTag", "RemoveTag", "HasTag", "GetTagged", "GetTags", "GetAllTags"}, {}},
		{"HttpService", {"JSONEncode", "JSONDecode", "GenerateGUID", "UrlEncode"}, {}},
		{"TweenService", {"GetValue", "Create"}, {}},
		{"Debris", {"AddItem"}, {}},
	};

	// **Every `Always` row appears above.** A service added to the catalogue and
	// not to this list would otherwise be checked by nothing here, which is the
	// silent half of the failure this suite exists for.
	std::set<std::string> listed;
	for (const Surface &surface : SURFACES) {
		listed.insert(surface.Service);
	}
	std::set<std::string> expected;
	for (const ServiceDefinition &definition : Services()) {
		if (definition.Availability == ServiceAvailability::Always) {
			expected.insert(definition.Name);
		}
	}
	CHECK(listed == expected);

	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Store store = Fresh("catalogue_members");
		const auto runtime = FullRuntime(store, language);
		REQUIRE(runtime != nullptr);

		for (const Surface &surface : SURFACES) {
			// **The probe raises rather than answering, because `Run` reports
			// whether a chunk *ran*.** A script returning `false` ran perfectly
			// well, so `return typeof(x) == 'function'` passes whatever the answer
			// is - which is how the first version of this case reported green
			// against a `TeleportService` with `GetTeleportData` deliberately
			// removed.
			//
			// **`type`/`typeof` and not a nil test**, because a member that exists
			// and is not callable is the other half of the same mistake: a signal
			// installed where a method was meant reads as present and fails at the
			// call site.
			for (const char *member : surface.Methods) {
				const std::string reached =
					std::string(language == Language::Luau ? "game:GetService('" : "game.GetService('") +
					surface.Service + "')." + member;

				const std::string source =
					language == Language::Luau
						? "assert(type(" + reached + ") == 'function', 'missing')"
						: "if (typeof " + reached + " !== 'function') { throw new Error('missing'); }";

				INFO(source);
				const bool ok = runtime->Run(source.c_str());
				INFO(runtime->LastError());
				CHECK(ok);
			}

			for (const char *member : surface.Values) {
				const std::string reached =
					std::string(language == Language::Luau ? "game:GetService('" : "game.GetService('") +
					surface.Service + "')." + member;

				const std::string source =
					language == Language::Luau
						? "assert(" + reached + " ~= nil, 'missing')"
						: "if (" + reached + " === undefined) { throw new Error('missing'); }";

				INFO(source);
				const bool ok = runtime->Run(source.c_str());
				INFO(runtime->LastError());
				CHECK(ok);
			}
		}
	}
}

TEST_CASE("the catalogue holds no duplicate names", "[scripting][services]") {
	// **Two rows for one name is one row that never installs.** Both would run,
	// the second would overwrite the first's global, and `FindService` would
	// answer with whichever came first - so the mask consulted by a refusal and
	// the installer that actually ran could describe different services. Cheap
	// to check and impossible to see by reading the table.
	for (const ServiceDefinition &left : Services()) {
		size_t seen = 0;
		for (const ServiceDefinition &right : Services()) {
			seen += std::string(left.Name) == right.Name ? 1u : 0u;
		}
		INFO(left.Name);
		CHECK(seen == 1);
	}
}

TEST_CASE("a row claims no language it has no installer for", "[scripting][services]") {
	// **The mask and the function pointer are two statements of one fact**, and
	// the catalogue is the only place they can disagree. A row claiming a
	// language whose installer is null would install nothing and then refuse
	// with the *general* message - a service that reads as never having existed,
	// with a table saying otherwise.
	//
	// Checked through the VM rather than by reaching for the private table: the
	// claim is "a script can get this", so a script is what asks.
	for (const ServiceDefinition &definition : Services()) {
		if (definition.Availability != ServiceAvailability::Always) {
			continue;
		}

		for (const Language language : {Language::Luau, Language::JavaScript}) {
			const ServiceLanguages want =
				language == Language::Luau ? ServiceLanguages::Luau : ServiceLanguages::JavaScript;
			if (!Binds(definition.Languages, want)) {
				continue;
			}

			Store store = Fresh("catalogue_installer");
			const auto runtime = FullRuntime(store, language);
			REQUIRE(runtime != nullptr);

			INFO(definition.Name);
			CHECK(runtime->Run(Fetch(language, definition.Name).c_str()));
		}
	}
}
