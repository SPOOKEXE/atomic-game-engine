#include "Bindings.hpp"

#include <lualib.h>
#include <string_view>

namespace engine::script {

	namespace {
		// Turns the store's reparent recording on, the first time a tree signal is
		// connected.
		//
		// **On connection rather than on world creation**, for the reason
		// `.Changed` observes late: a world nobody is watching must not pay for a
		// list nobody drains. Idempotent, so the second connection is a store
		// write of a bool it already holds.
		void WatchTreeFor(LuauContext &context, SignalKind kind) {
			if (kind == SignalKind::ChildAdded || kind == SignalKind::ChildRemoved ||
				kind == SignalKind::DescendantAdded || kind == SignalKind::AncestryChanged) {
				context.World->ObserveTree();
			}
		}

		using ecs::Entity;

		// What a signal userdata carries.
		//
		// Three fields and no list: the connections live in `SignalTable`, so
		// two scripts that reached the same signal by different routes —
		// `part.Changed` twice, say — are holding the same thing rather than two
		// objects that behave alike. A list per handle would have made
		// `:Disconnect` on one invisible to the other.
		struct SignalHandle {
			SignalKind Kind = SignalKind::Heartbeat;
			Entity Subject;
			core::Name Property;
		};

		SignalHandle &CheckSignal(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_SIGNAL);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "RBXScriptSignal");
			}
			return *static_cast<SignalHandle *>(value);
		}

		ConnectionId &CheckConnection(lua_State *state, int index) {
			void *value = lua_touserdatatagged(state, index, TAG_CONNECTION);
			if (value == nullptr) {
				luaL_typeerrorL(state, index, "RBXScriptConnection");
			}
			return *static_cast<ConnectionId *>(value);
		}

		void PushConnection(lua_State *state, ConnectionId id) {
			void *memory = lua_newuserdatatagged(state, sizeof(ConnectionId), TAG_CONNECTION);
			*static_cast<ConnectionId *>(memory) = id;

			luaL_getmetatable(state, "RBXScriptConnection");
			lua_setmetatable(state, -2);
		}

		// `signal:Connect(fn)` -> RBXScriptConnection
		int SignalConnect(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			SignalHandle &signal = CheckSignal(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);

			// **`.Changed` starts observing on the first connection**, not when
			// the instance is made. Observing a component is an archetype move,
			// and paying it for every part in a scene because one of them might
			// be watched later is the cost that makes a feature not worth
			// having.
			if (signal.Kind == SignalKind::Changed || signal.Kind == SignalKind::PropertyChanged) {
				context.Changes.Watch(*context.World, signal.Subject);
			}
			WatchTreeFor(context, signal.Kind);

			// A registry ref keeps the function alive, and is what `CallbackRef`
			// means on this side. The JavaScript side puts an index into its own
			// vector in the same integer; neither `SignalTable` nor anything
			// else shared may interpret one.
			lua_pushvalue(state, 2);
			const int reference = lua_ref(state, -1);
			lua_pop(state, 1);

			const ConnectionId id =
				context.Signals.Connect(signal.Kind, signal.Subject, reference, signal.Property);

			PushConnection(state, id);
			return 1;
		}

		// `signal:Once(fn)` — connect, and disconnect after the first call.
		//
		// Roblox's, and worth having rather than leaving to an author: written
		// by hand it is a connection variable captured by the closure that
		// assigns it, which is the one shape a Luau author reliably gets wrong.
		int SignalOnce(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			SignalHandle &signal = CheckSignal(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);

			if (signal.Kind == SignalKind::Changed || signal.Kind == SignalKind::PropertyChanged) {
				context.Changes.Watch(*context.World, signal.Subject);
			}
			WatchTreeFor(context, signal.Kind);

			lua_pushvalue(state, 2);
			const int reference = lua_ref(state, -1);
			lua_pop(state, 1);

			const ConnectionId id =
				context.Signals.Connect(signal.Kind, signal.Subject, reference, signal.Property);
			context.Signals.MarkOnce(id);

			PushConnection(state, id);
			return 1;
		}

		int SignalToString(lua_State *state) {
			CheckSignal(state, 1);
			lua_pushstring(state, "Signal");
			return 1;
		}

		int SignalIndex(lua_State *state) {
			const char *field = luaL_checkstring(state, 2);
			const std::string_view name(field);

			// The methods live on the metatable's own function table, reached
			// here rather than through a plain `__index` table because each
			// needs the context upvalue.
			if (name == "Connect" || name == "Once") {
				lua_getfield(state, LUA_REGISTRYINDEX, "engine.signal.methods");
				lua_getfield(state, -1, field);
				lua_remove(state, -2);
				return 1;
			}

			// **No `:Wait()`, and the omission is the design.** Roblox's yields
			// the calling thread until the signal next fires, and
			// `docs/SCRIPT_CONCURRENCY.md` §1 permits a resume only from
			// something the barrier delivers. A signal fires from inside a
			// handler pump, which is not a barrier — resuming there would put
			// one script's continuation inside another's call, at a point that
			// depends on connection order rather than on the tick.
			//
			// `task.wait` is the resume that is legal, and it says which tick it
			// comes back on.
			if (name == "Wait") {
				luaL_errorL(
					state, "RBXScriptSignal has no Wait; use task.wait, which resumes at a tick boundary"
				);
			}

			luaL_errorL(state, "RBXScriptSignal has no member '%s'", field);
		}

		// `connection:Disconnect()`
		int ConnectionDisconnect(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const ConnectionId id = CheckConnection(state, 1);

			CallbackRef released = 0;
			if (context.Signals.Disconnect(id, released)) {
				// Released **after** the table has forgotten it, so a fire in
				// progress cannot reach a ref that no longer resolves.
				lua_unref(state, released);
			}

			// Disconnecting twice is not an error. A cleanup path runs whether
			// or not something else already ran, and making the ordinary case
			// throw would push every author into a `pcall`.
			return 0;
		}

		int ConnectionIndex(lua_State *state) {
			LuauContext &context = UpvalueContext(state);
			const ConnectionId id = CheckConnection(state, 1);
			const char *field = luaL_checkstring(state, 2);
			const std::string_view name(field);

			if (name == "Connected") {
				lua_pushboolean(state, context.Signals.Connected(id));
				return 1;
			}

			if (name == "Disconnect") {
				lua_getfield(state, LUA_REGISTRYINDEX, "engine.connection.methods");
				lua_getfield(state, -1, field);
				lua_remove(state, -2);
				return 1;
			}

			luaL_errorL(state, "RBXScriptConnection has no member '%s'", field);
		}

		int ConnectionToString(lua_State *state) {
			CheckConnection(state, 1);
			lua_pushstring(state, "Connection");
			return 1;
		}

		// Equality by identity, so `a == b` on two handles onto one signal is
		// true. Without it `part.Changed == part.Changed` would be false, and an
		// author storing one and comparing later would be quietly wrong.
		int SignalEqual(lua_State *state) {
			const SignalHandle &left = CheckSignal(state, 1);
			const SignalHandle &right = CheckSignal(state, 2);

			lua_pushboolean(
				state,
				left.Kind == right.Kind && left.Subject == right.Subject && left.Property == right.Property
			);
			return 1;
		}

		int ConnectionEqual(lua_State *state) {
			lua_pushboolean(state, CheckConnection(state, 1) == CheckConnection(state, 2));
			return 1;
		}
	}

	void PushSignal(lua_State *state, SignalKind kind, Entity subject, core::Name property) {
		void *memory = lua_newuserdatatagged(state, sizeof(SignalHandle), TAG_SIGNAL);
		auto *handle = new (memory) SignalHandle();
		handle->Kind = kind;
		handle->Subject = subject;
		handle->Property = property;

		luaL_getmetatable(state, "RBXScriptSignal");
		lua_setmetatable(state, -2);
	}

	void OpenSignals(lua_State *state) {
		LuauContext &context = ContextOf(state);

		// The method tables, held in the registry so `__index` can hand a
		// closure back without building one per access.
		lua_newtable(state);
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, SignalConnect, "Connect", 1);
		lua_setfield(state, -2, "Connect");
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, SignalOnce, "Once", 1);
		lua_setfield(state, -2, "Once");
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.signal.methods");

		lua_newtable(state);
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, ConnectionDisconnect, "Disconnect", 1);
		lua_setfield(state, -2, "Disconnect");
		lua_setfield(state, LUA_REGISTRYINDEX, "engine.connection.methods");

		luaL_newmetatable(state, "RBXScriptSignal");
		lua_pushcfunction(state, SignalIndex, "__index");
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, SignalToString, "__tostring");
		lua_setfield(state, -2, "__tostring");
		lua_pushcfunction(state, SignalEqual, "__eq");
		lua_setfield(state, -2, "__eq");
		lua_pushstring(state, "RBXScriptSignal");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "RBXScriptSignal");
		lua_setfield(state, -2, "__type");
		lua_pop(state, 1);

		luaL_newmetatable(state, "RBXScriptConnection");
		lua_pushlightuserdata(state, &context);
		lua_pushcclosure(state, ConnectionIndex, "__index", 1);
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, ConnectionToString, "__tostring");
		lua_setfield(state, -2, "__tostring");
		lua_pushcfunction(state, ConnectionEqual, "__eq");
		lua_setfield(state, -2, "__eq");
		lua_pushstring(state, "RBXScriptConnection");
		lua_setfield(state, -2, "__metatable");
		lua_pushstring(state, "RBXScriptConnection");
		lua_setfield(state, -2, "__type");
		lua_pop(state, 1);
	}

	std::string FireSignal(lua_State *state, SignalKind kind, Entity subject, int arguments) {
		LuauContext &context = ContextOf(state);
		const int base = lua_gettop(state) - arguments;

		std::string firstError;
		std::vector<ConnectionId> spent;

		context.Signals.Fire(kind, subject, [&](const Connection &connection) {
			lua_getref(state, connection.Callback);
			for (int index = 1; index <= arguments; index++) {
				lua_pushvalue(state, base + index);
			}

			// **Every connection runs even when one raises**, and the first
			// error is what the host hears about. A handler that threw once
			// would otherwise silently stop everything registered after it, and
			// the symptom — half a scene animating — points nowhere near the
			// cause.
			if (lua_pcall(state, arguments, 0, 0) != LUA_OK) {
				if (firstError.empty()) {
					const char *message = lua_tostring(state, -1);
					firstError = message != nullptr ? message : "a connection failed";
				}
				lua_pop(state, 1);
			}

			if (connection.Once) {
				spent.push_back(connection.Id);
			}
		});

		// Retired after the fire rather than inside it, so a `:Once` handler
		// that connected another one does not have its list compacted underneath
		// the walk.
		for (const ConnectionId id : spent) {
			CallbackRef released = 0;
			if (context.Signals.Disconnect(id, released)) {
				lua_unref(state, released);
			}
		}

		lua_pop(state, arguments);
		return firstError;
	}
}
