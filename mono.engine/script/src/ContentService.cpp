// What content this world actually has, from a script.
//
// **A scene could name an asset and had no way to ask what the names were.**
// `part.MeshId = "props/fox.amesh"` is rule 4 working correctly — a name crosses
// and an id does not — but it left every demo in `examples/` holding a string
// literal for a file that only exists if somebody baked and published that exact
// tree. `MeshGrid.luau` had six of them, and on any store but the one it was
// written against every single part fell back to a cube. Nothing warned: an
// unregistered mesh draws as a cube, which is also what a mesh still streaming
// in looks like.
//
// So this is the other half of the pair: the catalogues the client already fills
// as content arrives, readable from Luau.
//
// **Most of it reports what this world has, not what a store holds.**
// `MeshCatalogue` and `TextureCatalogue` are written by whatever registered the
// content this run — the client's content pump — so an empty list means "nothing
// has arrived here", which on a headless server is the permanent and correct
// answer.
//
// **`GetPublishedMeshes` is the one exception, and v0.10 made it necessary.**
// While content was fetched by kind, everything published arrived whether a
// scene wanted it or not, so "what has arrived" and "what exists" were the same
// list and only the first needed asking. Nothing is fetched by kind any more —
// `client/ContentDemand.hpp` carries the 6.9 GB that ended it — so a scene
// reading only the first can never discover anything, and every demo was back to
// string literals. It reads the manifest a client already verified, which is a
// few hundred strings rather than a store, and naming one of them is what
// fetches it.
//
// **Names, in sorted order, and nothing else.** Not handles, not a table of
// metadata that would then be a second place facts about a mesh live —
// `MeshPart.TrianglesCount` is already the way to ask about one. Sorted because
// the catalogues are hash maps and a demo that laid parts out in iteration order
// would arrange itself differently on every run, which is exactly the kind of
// non-determinism `AGENTS.md` rule 5 is about even when nothing replicates it.

#include "Bindings.hpp"

#include <engine/core/Name.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/TextureCatalogue.hpp>

#include <algorithm>
#include <lua.h>
#include <lualib.h>
#include <string>
#include <vector>

namespace engine::script {
	namespace {
		// The world this call's service table was installed against.
		//
		// **Its own copy of `Services.cpp`'s two-liner rather than a shared
		// helper**, because the alternative is a header exporting a function
		// that only makes sense inside a C closure — and `UpvalueContext`'s own
		// note is that calling it where no C closure is on the stack is how it
		// was crashed once already. Two lines beside their only caller is the
		// clearer of the two.
		ecs::Store &StoreOfUpvalue(lua_State *state) {
			return *UpvalueContext(state).World;
		}

		// Pushes a sorted array of names as a Luau table.
		//
		// One-based, because that is what `ipairs` and `#` mean in Luau and a
		// zero-based array handed to a script is a list whose first element is
		// invisible.
		int PushNames(lua_State *state, std::vector<std::string> names) {
			std::sort(names.begin(), names.end());

			lua_createtable(state, static_cast<int>(names.size()), 0);
			for (size_t index = 0; index < names.size(); index++) {
				lua_pushlstring(state, names[index].data(), names[index].size());
				lua_rawseti(state, -2, static_cast<int>(index) + 1);
			}
			return 1;
		}

		// ContentService:GetMeshes()
		int GetMeshes(lua_State *state) {
			const ecs::Store &store = StoreOfUpvalue(state);

			std::vector<std::string> names;
			if (const auto *catalogue = store.Resource<scene::MeshCatalogue>(); catalogue != nullptr) {
				names.reserve(catalogue->Triangles.size());
				for (const auto &[id, triangles] : catalogue->Triangles) {
					// **Skipped rather than reported as an empty string.** An id
					// that no longer resolves is a name registry that has been
					// reset under this world, which is not a thing a script can
					// act on.
					const core::Name name = core::Name::FromId(id);
					if (name.IsValid()) {
						names.emplace_back(name.Text());
					}
				}
			}
			return PushNames(state, std::move(names));
		}

		// ContentService:GetPublishedMeshes()
		//
		// **What there is to name, where `GetMeshes` says what has been named.**
		// The two used to be one question because content was fetched by kind, so
		// everything published arrived whether or not a scene wanted it. Since
		// v0.10 nothing is fetched by kind — `client/ContentDemand.hpp` has the
		// 6.9 GB that made that necessary — and the consequence landed here: a
		// scene reading `GetMeshes` sees only what it or another scene already
		// asked for, so it can never discover anything.
		//
		// Reading this and setting a `MeshId` *is* the ask. The name goes into
		// the world, `CollectWantedContent` finds it on the next pump, and that
		// one asset is fetched. A scene decides how many to take;
		// `MeshGrid.luau` takes twelve.
		int GetPublishedMeshes(lua_State *state) {
			const ecs::Store &store = StoreOfUpvalue(state);

			std::vector<core::Name> published;
			(void)scene::PublishedMeshes(store, published);

			std::vector<std::string> names;
			names.reserve(published.size());
			for (const core::Name &mesh : published) {
				names.emplace_back(mesh.Text());
			}
			return PushNames(state, std::move(names));
		}

		// ContentService:GetTextures()
		int GetTextures(lua_State *state) {
			const ecs::Store &store = StoreOfUpvalue(state);

			std::vector<std::string> names;
			if (const auto *catalogue = store.Resource<scene::TextureCatalogue>(); catalogue != nullptr) {
				names.reserve(catalogue->Flipbooks.size());
				for (const auto &[id, facts] : catalogue->Flipbooks) {
					const core::Name name = core::Name::FromId(id);
					if (name.IsValid()) {
						names.emplace_back(name.Text());
					}
				}
			}
			return PushNames(state, std::move(names));
		}

		// ContentService:GetFlipbook(texture) -> {Side, Frames, FrameRate} or nil
		//
		// **The one piece of metadata that is not derivable and not already
		// exposed.** A flipbook's grid and rate come from the source file — a
		// GIF states a delay per frame — and a scene that wanted to drive an
		// emitter at the authored rate would otherwise have to hardcode a number
		// the bake already knows. `nil` for a still image, which is the same
		// answer as "this world has not been told", and for the same reason as
		// `TextureCatalogue::Find`: neither is something to play.
		int GetFlipbook(lua_State *state) {
			const ecs::Store &store = StoreOfUpvalue(state);
			const char *texture = luaL_checkstring(state, 2);

			const scene::FlipbookFacts facts = scene::FlipbookOf(store, core::Name(texture));
			if (!facts.IsFlipbook()) {
				lua_pushnil(state);
				return 1;
			}

			lua_createtable(state, 0, 3);
			lua_pushinteger(state, facts.Side);
			lua_setfield(state, -2, "Side");
			lua_pushinteger(state, facts.Frames);
			lua_setfield(state, -2, "Frames");
			lua_pushnumber(state, facts.FrameRate);
			lua_setfield(state, -2, "FrameRate");
			return 1;
		}

		// ContentService:GetTriangleCount(mesh)
		//
		// The same number `MeshPart.TrianglesCount` gives, asked about a mesh
		// rather than about a part — so a script can size a layout before it has
		// built anything to measure.
		int GetTriangleCount(lua_State *state) {
			const ecs::Store &store = StoreOfUpvalue(state);
			const char *mesh = luaL_checkstring(state, 2);
			lua_pushinteger(state, static_cast<int>(scene::TrianglesOf(store, core::Name(mesh))));
			return 1;
		}
	}

	void OpenContentService(lua_State *state) {
		LuauContext &context = ContextOf(state);

		static const luaL_Reg methods[] = {
			{"GetMeshes", GetMeshes},
			{"GetPublishedMeshes", GetPublishedMeshes},
			{"GetTextures", GetTextures},
			{"GetFlipbook", GetFlipbook},
			{"GetTriangleCount", GetTriangleCount},
			{nullptr, nullptr},
		};

		lua_newtable(state);
		for (const luaL_Reg *method = methods; method->name != nullptr; method++) {
			lua_pushlightuserdata(state, &context);
			lua_pushcclosure(state, method->func, method->name, 1);
			lua_setfield(state, -2, method->name);
		}
		lua_setglobal(state, "ContentService");
	}
}
