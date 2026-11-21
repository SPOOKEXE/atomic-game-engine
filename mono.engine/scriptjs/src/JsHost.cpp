// The program host exposed to JavaScript plugin runtimes.
//
// Host names are data. A host contributes one flat object plus service objects
// for dotted names, and every function enters the same value marshaller. This
// is the QuickJS twin of `LuauHostCalls.cpp`; neither adapter names the other.

#include "JsBindings.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>

#include <algorithm>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		constexpr uint32_t HOST_MAX_DEPTH = 16;

		struct RetainingSource {
			JsContext &Context;
			ecs::Entity Previous;
			RetainingSource(JsContext &context, ecs::Entity source)
				: Context(context), Previous(std::exchange(context.RetainingSource, source)) {}
			~RetainingSource() {
				Context.RetainingSource = Previous;
			}
		};

		class PropertyNames {
		  public:
			PropertyNames(JSContext *context, JSValueConst object) : Context(context) {
				Present = JS_GetOwnPropertyNames(
							  context, &Properties, &Count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY
						  ) == 0;
			}

			~PropertyNames() {
				if (Properties == nullptr) {
					return;
				}
				for (uint32_t index = 0; index < Count; index++) {
					JS_FreeAtom(Context, Properties[index].atom);
				}
				js_free(Context, Properties);
			}

			PropertyNames(const PropertyNames &) = delete;
			PropertyNames &operator=(const PropertyNames &) = delete;
			PropertyNames(PropertyNames &&) = delete;
			PropertyNames &operator=(PropertyNames &&) = delete;

			bool Ok() const {
				return Present;
			}

			uint32_t Size() const {
				return Count;
			}

			JSAtom At(uint32_t index) const {
				return Properties[index].atom;
			}

		  private:
			JSContext *Context = nullptr;
			JSPropertyEnum *Properties = nullptr;
			uint32_t Count = 0;
			bool Present = false;
		};

		bool ReadHostValue(JSContext *context, JSValueConst value, HostValue &out, uint32_t depth) {
			if (depth > HOST_MAX_DEPTH) {
				return false;
			}

			if (JS_IsNull(value) || JS_IsUndefined(value)) {
				out = HostValue{};
				return true;
			}
			if (JS_IsBool(value)) {
				out = HostValue(HostTag::Boolean);
				out.Boolean = JS_ToBool(context, value) == 1;
				return true;
			}
			if (JS_IsNumber(value)) {
				double number = 0.0;
				if (JS_ToFloat64(context, &number, value) != 0) {
					return false;
				}
				out = HostValue(HostTag::Number);
				out.Number = number;
				return true;
			}
			if (JS_IsString(value)) {
				size_t length = 0;
				const char *text = JS_ToCStringLen(context, &length, value);
				if (text == nullptr) {
					return false;
				}
				out = HostValue(HostTag::String);
				out.Text.assign(text, length);
				JS_FreeCString(context, text);
				return true;
			}

			JsContext &bound = JsOf(context);
			if (JS_IsFunction(context, value)) {
				const HostCallback callback{++bound.NextHostCallback};
				bound.HostCallbacks.emplace(callback.Id, Retain(context, value));

				out = HostValue(HostTag::Callback);
				out.Callback = callback;
				return true;
			}

			if (!JS_IsObject(value)) {
				return false;
			}

			if (const auto *instance =
					static_cast<const ecs::Entity *>(JS_GetOpaque(value, bound.InstanceClass));
				instance != nullptr) {
				out = HostValue(HostTag::Instance);
				out.Instance = JsEntityOf(context, value);
				return true;
			}
			if (const auto *vector =
					static_cast<const core::Vector3 *>(JS_GetOpaque(value, bound.Vector3Class));
				vector != nullptr) {
				out = HostValue(HostTag::Vector3);
				out.Vector = *vector;
				return true;
			}
			if (const auto *colour =
					static_cast<const core::Color3 *>(JS_GetOpaque(value, bound.Color3Class));
				colour != nullptr) {
				out = HostValue(HostTag::Color3);
				out.Colour = *colour;
				return true;
			}
			if (const auto *frame = static_cast<const core::CFrame *>(JS_GetOpaque(value, bound.CFrameClass));
				frame != nullptr) {
				out = HostValue(HostTag::CFrame);
				out.Frame = *frame;
				return true;
			}

			if (JS_GetOpaque(value, bound.EnumItemClass) != nullptr) {
				JSValue name = JS_GetPropertyStr(context, value, "Name");
				size_t length = 0;
				const char *text = JS_ToCStringLen(context, &length, name);
				if (text == nullptr) {
					JS_FreeValue(context, name);
					return false;
				}
				out = HostValue(HostTag::String);
				out.Text.assign(text, length);
				JS_FreeCString(context, text);
				JS_FreeValue(context, name);
				return true;
			}

			if (JS_IsArray(value)) {
				JSValue held = JS_GetPropertyStr(context, value, "length");
				uint32_t length = 0;
				const int converted = JS_ToUint32(context, &length, held);
				JS_FreeValue(context, held);
				if (converted != 0) {
					return false;
				}

				out = HostValue(HostTag::Array);
				out.Items.reserve(length);
				for (uint32_t index = 0; index < length; index++) {
					held = JS_GetPropertyUint32(context, value, index);
					if (JS_IsException(held)) {
						JS_FreeValue(context, held);
						return false;
					}

					HostValue item;
					const bool read = ReadHostValue(context, held, item, depth + 1);
					JS_FreeValue(context, held);
					if (!read) {
						return false;
					}
					out.Items.push_back(std::move(item));
				}
				return true;
			}

			PropertyNames properties(context, value);
			if (!properties.Ok()) {
				return false;
			}

			out = HostValue(HostTag::Map);
			out.Entries.reserve(properties.Size());
			for (uint32_t index = 0; index < properties.Size(); index++) {
				const JSAtom atom = properties.At(index);
				size_t length = 0;
				const char *text = JS_AtomToCStringLen(context, &length, atom);
				if (text == nullptr) {
					return false;
				}
				std::string key(text, length);
				JS_FreeCString(context, text);

				JSValue held = JS_GetProperty(context, value, atom);
				if (JS_IsException(held)) {
					JS_FreeValue(context, held);
					return false;
				}

				HostValue item;
				const bool read = ReadHostValue(context, held, item, depth + 1);
				JS_FreeValue(context, held);
				if (!read) {
					return false;
				}
				out.Entries.emplace_back(std::move(key), std::move(item));
			}
			return true;
		}

		JSValue PushHostValue(JSContext *context, const HostValue &value) {
			switch (value.Tag) {
			case HostTag::Nil:
				return JS_NULL;
			case HostTag::Boolean:
				return JS_NewBool(context, value.Boolean);
			case HostTag::Number:
				return JS_NewFloat64(context, value.Number);
			case HostTag::String:
				return JS_NewStringLen(context, value.Text.data(), value.Text.size());
			case HostTag::Instance:
				return MakeJsInstance(context, value.Instance);
			case HostTag::Vector3:
				return MakeVector3(context, value.Vector);
			case HostTag::Color3:
				return MakeColor3(context, value.Colour);
			case HostTag::CFrame:
				return MakeCFrame(context, value.Frame);
			case HostTag::Array: {
				JSValue array = JS_NewArray(context);
				for (size_t index = 0; index < value.Items.size(); index++) {
					if (JS_SetPropertyUint32(
							context,
							array,
							static_cast<uint32_t>(index),
							PushHostValue(context, value.Items[index])
						) < 0) {
						JS_FreeValue(context, array);
						return JS_EXCEPTION;
					}
				}
				return array;
			}
			case HostTag::Map: {
				JSValue object = JS_NewObject(context);
				for (const auto &[key, item] : value.Entries) {
					const JSAtom atom = JS_NewAtomLen(context, key.data(), key.size());
					if (atom == JS_ATOM_NULL) {
						JS_FreeValue(context, object);
						return JS_EXCEPTION;
					}
					const int set = JS_SetProperty(context, object, atom, PushHostValue(context, item));
					JS_FreeAtom(context, atom);
					if (set < 0) {
						JS_FreeValue(context, object);
						return JS_EXCEPTION;
					}
				}
				return object;
			}
			case HostTag::Callback:
				// The callback is already retained by this runtime. Returning it
				// would create a second owner with no second release operation.
				return JS_NULL;
			}
			return JS_NULL;
		}

		std::string ExceptionMessage(JSContext *context) {
			JSValue thrown = JS_GetException(context);
			std::string message = "unknown";
			if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
				message = text;
				JS_FreeCString(context, text);
			}
			JS_FreeValue(context, thrown);
			return message;
		}

		// One host call from JavaScript. `self` is prepended when the call is a
		// `@Class` member, so the host sees which instance it was made on.
		JSValue CallHost(
			JSContext *context,
			JSValueConst nameValue,
			const ecs::Entity *self,
			int count,
			JSValueConst *values
		) {
			JsContext &bound = JsOf(context);
			if (bound.Host == nullptr) {
				return JS_ThrowPlainError(context, "this program offers no host surface");
			}

			size_t nameLength = 0;
			const char *name = JS_ToCStringLen(context, &nameLength, nameValue);
			if (name == nullptr) {
				return JS_EXCEPTION;
			}

			std::vector<HostValue> arguments;
			arguments.reserve(static_cast<size_t>(count) + 1);
			if (self != nullptr) arguments.push_back(HostValue::Of(*self));
			for (int index = 0; index < count; index++) {
				HostValue argument;
				if (!ReadHostValue(context, values[index], argument, 0)) {
					const std::string called(name, nameLength);
					JS_FreeCString(context, name);
					if (JS_HasException(context)) {
						return JS_EXCEPTION;
					}
					return JS_ThrowTypeError(
						context, "argument %d of %s has no host representation", index + 1, called.c_str()
					);
				}
				arguments.push_back(std::move(argument));
			}

			HostValue result;
			std::string failure;
			const std::string called(name, nameLength);
			JS_FreeCString(context, name);
			// Any JavaScript caller can wait, because waiting is a promise.
			HostSurface &host = *bound.Host;
			const HostSurface::CallState enclosing = host.BeginCall(true);
			const bool answered = host.Call(called, arguments, result, failure);
			const uint64_t token = host.EndCall(enclosing);
			if (!answered) {
				return JS_ThrowPlainError(context, "%s: %s", called.c_str(), failure.c_str());
			}
			if (token != 0) {
				JSValue settle[2];
				JSValue promise = JS_NewPromiseCapability(context, settle);
				if (JS_IsException(promise)) return promise;
				bound.AwaitedHostCalls.insert_or_assign(
					token, JsContext::HostAwait{Retain(context, settle[0]), Retain(context, settle[1])}
				);
				JS_FreeValue(context, settle[0]);
				JS_FreeValue(context, settle[1]);
				return promise;
			}
			return PushHostValue(context, result);
		}

		JSValue HostCall(
			JSContext *context, JSValueConst, int count, JSValueConst *values, int, JSValueConst *functionData
		) {
			return CallHost(context, functionData[0], nullptr, count, values);
		}

		// The instance a member was called on, or a thrown TypeError.
		bool MemberSelf(JSContext *context, JSValueConst self, ecs::Entity &out) {
			out = JsEntityOf(context, self);
			if (out != ecs::NULL_ENTITY) return true;
			JS_ThrowTypeError(context, "a host member has to be called on its instance");
			return false;
		}

		// `instance.Method(...)` for `@Class:Method`.
		JSValue HostMemberCall(
			JSContext *context, JSValueConst self, int count, JSValueConst *values, int, JSValueConst *data
		) {
			ecs::Entity instance;
			if (!MemberSelf(context, self, instance)) return JS_EXCEPTION;
			return CallHost(context, data[0], &instance, count, values);
		}

		// `instance.Property` and `instance.Property = value` for `@Class.Property`.
		JSValue
		HostMemberGet(JSContext *context, JSValueConst self, int, JSValueConst *, int, JSValueConst *data) {
			ecs::Entity instance;
			if (!MemberSelf(context, self, instance)) return JS_EXCEPTION;
			return CallHost(context, data[0], &instance, 0, nullptr);
		}

		JSValue HostMemberSet(
			JSContext *context, JSValueConst self, int count, JSValueConst *values, int, JSValueConst *data
		) {
			ecs::Entity instance;
			if (!MemberSelf(context, self, instance)) return JS_EXCEPTION;
			JSValue result = CallHost(context, data[0], &instance, count > 0 ? 1 : 0, values);
			if (JS_IsException(result)) return result;
			JS_FreeValue(context, result);
			return JS_UNDEFINED;
		}

		// `instance.Event` for `@Class!Event`, fired by `Runtime::FireHostEvent`.
		JSValue
		HostMemberEvent(JSContext *context, JSValueConst self, int, JSValueConst *, int, JSValueConst *data) {
			ecs::Entity instance;
			if (!MemberSelf(context, self, instance)) return JS_EXCEPTION;
			const char *event = JS_ToCString(context, data[0]);
			if (event == nullptr) return JS_EXCEPTION;
			const core::Name name(event);
			JS_FreeCString(context, event);
			return MakeJsSignal(context, SignalKind::HostEvent, instance, name);
		}

		JSValue HostFunction(JSContext *context, std::string_view member, std::string_view fullName) {
			JSValue name = JS_NewStringLen(context, fullName.data(), fullName.size());
			const std::string functionName(member);
			JSValue function = JS_NewCFunctionData2(context, HostCall, functionName.c_str(), 0, 0, 1, &name);
			JS_FreeValue(context, name);
			return function;
		}

		// Builds a signal around one raw host member; see `IsHostSignal` and
		// the Luau twin in `LuauHostCalls.cpp`. Calling the signal directly
		// still reaches the host unchanged.
		constexpr std::string_view SIGNAL_FACTORY = R"((function (raw) {
	const handlers = [];
	let installed = false;
	const dispatch = (...args) => {
		for (const handler of handlers.slice()) handler(...args);
	};
	const connect = (once, handler) => {
		if (typeof handler !== "function") throw new TypeError("Connect expects a function");
		if (!installed) {
			raw(dispatch);
			installed = true;
		}
		let entry;
		const connection = {
			Connected: true,
			Disconnect() {
				if (!connection.Connected) return;
				connection.Connected = false;
				const at = handlers.indexOf(entry);
				if (at >= 0) handlers.splice(at, 1);
			},
		};
		entry = once ? (...args) => { connection.Disconnect(); handler(...args); } : handler;
		handlers.push(entry);
		return connection;
	};
	const signal = (...args) => {
		installed = false;
		return raw(...args);
	};
	signal.Connect = (handler) => connect(false, handler);
	signal.Once = (handler) => connect(true, handler);
	return Object.freeze(signal);
}))";

		// Wraps `raw` as a signal, taking ownership of it. Answers `raw` itself
		// if the factory cannot be built, so the member still works as a call.
		JSValue WrapSignal(JSContext *context, JSValue raw) {
			JSValue factory = JS_Eval(
				context,
				SIGNAL_FACTORY.data(),
				SIGNAL_FACTORY.size(),
				"<signal>",
				JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT
			);
			if (JS_IsException(factory)) {
				ENGINE_ERROR("host signal factory did not load: {}", ExceptionMessage(context));
				return raw;
			}
			JSValue signal = JS_Call(context, factory, JS_UNDEFINED, 1, &raw);
			JS_FreeValue(context, factory);
			if (JS_IsException(signal)) {
				ENGINE_ERROR("host signal factory failed: {}", ExceptionMessage(context));
				return raw;
			}
			JS_FreeValue(context, raw);
			return signal;
		}

		void DefineHostFunction(
			JSContext *context, JSValueConst object, std::string_view member, std::string_view fullName
		) {
			const std::string property(member);
			JSValue function = HostFunction(context, member, fullName);
			if (member != fullName && IsHostSignal(member)) {
				function = WrapSignal(context, function);
			}
			JS_DefinePropertyValueStr(context, object, property.c_str(), function, JS_PROP_ENUMERABLE);
		}

		void DeleteGlobal(JSContext *context, JSValueConst global, const std::string &name) {
			if (name.empty()) {
				return;
			}
			const JSAtom atom = JS_NewAtomLen(context, name.data(), name.size());
			JS_DeleteProperty(context, global, atom, 0);
			JS_FreeAtom(context, atom);
		}
	}

	void OpenJsHost(JSContext *context) {
		JsContext &bound = JsOf(context);
		JSValue global = JS_GetGlobalObject(context);

		DeleteGlobal(context, global, bound.HostGlobal);
		for (const std::string &service : bound.HostServices) {
			DeleteGlobal(context, global, service);
		}
		bound.HostGlobal.clear();
		bound.HostServices.clear();
		bound.HostMembers.clear();
		bound.HostInstance = {};

		// Prototypes built before this host lack its members, or carry the old
		// host's. Dropped from the cache so the next instance object rebuilds
		// them; `Owned` still frees the old ones with the context.
		for (auto &[id, prototype] : bound.Prototypes) {
			JS_FreeValue(context, prototype);
		}
		bound.Prototypes.clear();

		if (bound.Host == nullptr) {
			JS_FreeValue(context, global);
			return;
		}

		std::vector<std::string> flat;
		std::vector<std::pair<std::string, std::vector<std::string>>> services;
		for (const std::string &name : bound.Host->Names()) {
			if (!name.empty() && name.front() == '@') {
				bound.HostMembers.push_back(name);
				continue;
			}
			const size_t dot = name.find('.');
			if (dot == std::string::npos) {
				flat.push_back(name);
				continue;
			}

			const std::string service = name.substr(0, dot);
			const auto found = std::find_if(services.begin(), services.end(), [&service](const auto &entry) {
				return entry.first == service;
			});
			if (found == services.end()) {
				services.emplace_back(service, std::vector<std::string>{name});
			} else {
				found->second.push_back(name);
			}
		}

		JSValue host = JS_NewObject(context);
		for (const std::string &name : flat) {
			DefineHostFunction(context, host, name, name);
		}
		JS_PreventExtensions(context, host);

		// An instance global replaces the table, and the flat calls become its
		// members; see `InstallJsHostMembers`.
		bound.HostGlobal = bound.Host->GlobalName();
		bound.HostInstance = bound.Host->GlobalInstance();
		if (bound.HostInstance != ecs::NULL_ENTITY) {
			JS_FreeValue(context, host);
			host = MakeJsInstance(context, bound.HostInstance);
		}
		JS_SetPropertyStr(context, global, bound.HostGlobal.c_str(), host);

		for (const auto &[service, names] : services) {
			JSValue object = JS_NewObject(context);
			for (const std::string &name : names) {
				const std::string method = name.substr(service.size() + 1);
				DefineHostFunction(context, object, method, name);
			}
			JS_PreventExtensions(context, object);
			JS_SetPropertyStr(context, global, service.c_str(), object);
			bound.HostServices.push_back(service);
		}

		JS_FreeValue(context, global);
	}

	JSValue PushJsHostValue(JSContext *context, const HostValue &value) {
		return PushHostValue(context, value);
	}

	void InstallJsHostMembers(JSContext *context, JSValueConst proto, ecs::ClassId id) {
		JsContext &bound = JsOf(context);
		if (bound.Host == nullptr) return;

		std::unordered_set<std::string> installed;
		const auto define = [&](const std::string &member, const std::string &full, char kind) {
			if (!installed.insert(member).second) return;
			JSValue name = JS_NewStringLen(context, full.data(), full.size());
			const JSAtom atom = JS_NewAtomLen(context, member.data(), member.size());
			if (kind == ':') {
				JSValue function =
					JS_NewCFunctionData2(context, HostMemberCall, member.c_str(), 0, 0, 1, &name);
				JS_DefinePropertyValue(context, proto, atom, function, JS_PROP_CONFIGURABLE);
			} else if (kind == '.') {
				JSValue getter = JS_NewCFunctionData(context, HostMemberGet, 0, 0, 1, &name);
				JSValue setter = JS_NewCFunctionData(context, HostMemberSet, 1, 0, 1, &name);
				JS_DefinePropertyGetSet(context, proto, atom, getter, setter, JS_PROP_CONFIGURABLE);
			} else {
				JSValue event = JS_NewStringLen(context, member.data(), member.size());
				JSValue getter = JS_NewCFunctionData(context, HostMemberEvent, 0, 0, 1, &event);
				JS_FreeValue(context, event);
				JS_DefinePropertyGetSet(context, proto, atom, getter, JS_UNDEFINED, JS_PROP_CONFIGURABLE);
			}
			JS_FreeAtom(context, atom);
			JS_FreeValue(context, name);
		};

		// Nearest class first, so a subclass's member shadows its parent's.
		for (ecs::ClassId owner = id; owner.IsValid(); owner = ecs::Classes::Describe(owner).Parent) {
			const std::string prefix = "@" + std::string(ecs::Classes::Describe(owner).Name.Text());
			for (const std::string &full : bound.HostMembers) {
				if (full.size() <= prefix.size() + 1 || !full.starts_with(prefix)) continue;
				const char kind = full[prefix.size()];
				if (kind != ':' && kind != '.' && kind != '!') continue;
				define(full.substr(prefix.size() + 1), full, kind);
			}
		}

		// The flat calls, where no member took the name. A JavaScript call has
		// no colon to tell `plugin:CreateToolbar` from `plugin.CreateToolbar`,
		// so the Roblox member wins.
		if (bound.HostInstance == ecs::NULL_ENTITY || !bound.World->Alive(bound.HostInstance) ||
			bound.World->ClassOf(bound.HostInstance) != id)
			return;
		for (const std::string &name : bound.Host->Names()) {
			if (name.empty() || name.front() == '@' || name.find('.') != std::string::npos) continue;
			if (installed.contains(name)) continue;
			JSValue function = HostFunction(context, name, name);
			JS_DefinePropertyValueStr(context, proto, name.c_str(), function, JS_PROP_CONFIGURABLE);
		}
	}

	bool CallJsHostCallback(
		JSContext *context,
		HostCallback callback,
		HostArguments arguments,
		std::string &error,
		HostValue *output
	) {
		JsContext &bound = JsOf(context);
		if (!callback.Valid()) {
			error = "invalid host callback";
			return false;
		}

		const auto found = bound.HostCallbacks.find(callback.Id);
		if (found == bound.HostCallbacks.end()) {
			error = "unknown host callback";
			return false;
		}

		JSValueConst callable = Held(context, found->second);
		if (!JsCallbackAlive(context, found->second)) {
			error = "released host callback";
			return false;
		}
		const RetainingSource retainingSource(
			bound, bound.Callables[static_cast<size_t>(found->second)].Source
		);

		std::vector<JSValue> values;
		values.reserve(arguments.size());
		for (const HostValue &argument : arguments) {
			JSValue value = PushHostValue(context, argument);
			if (JS_IsException(value)) {
				for (JSValue held : values) {
					JS_FreeValue(context, held);
				}
				error = "a host callback argument has no JavaScript representation";
				return false;
			}
			values.push_back(value);
		}

		JSValue result = JS_Call(
			context,
			callable,
			JS_UNDEFINED,
			static_cast<int>(values.size()),
			values.empty() ? nullptr : values.data()
		);
		for (JSValue value : values) {
			JS_FreeValue(context, value);
		}

		if (JS_IsException(result)) {
			error = ExceptionMessage(context);
			ENGINE_ERROR("host callback failed: {}", error);
			JS_FreeValue(context, result);
			return false;
		}
		if (output != nullptr && !ReadHostValue(context, result, *output, 0)) {
			JS_FreeValue(context, result);
			error = "host callback returned a value with no host representation";
			return false;
		}
		JS_FreeValue(context, result);
		error.clear();
		return true;
	}

	std::string PumpJsHostCalls(JSContext *context) {
		JsContext &bound = JsOf(context);
		if (bound.HostResolutions.empty()) return {};
		std::vector<JsContext::HostResolution> resolutions;
		resolutions.swap(bound.HostResolutions);

		std::string firstError;
		for (JsContext::HostResolution &resolution : resolutions) {
			const auto waiting = bound.AwaitedHostCalls.find(resolution.Token);
			if (waiting == bound.AwaitedHostCalls.end()) continue;
			const JsContext::HostAwait await = waiting->second;
			bound.AwaitedHostCalls.erase(waiting);

			JSValue argument =
				resolution.Succeeded ? PushHostValue(context, resolution.Value) : JS_NewError(context);
			if (!resolution.Succeeded && !JS_IsException(argument)) {
				JS_SetPropertyStr(
					context, argument, "message", JS_NewString(context, resolution.Failure.c_str())
				);
			}
			if (JS_IsException(argument)) {
				argument = JS_UNDEFINED;
			}
			JSValue result =
				InvokeJsCallback(context, resolution.Succeeded ? await.Resolve : await.Reject, 1, &argument);
			if (JS_IsException(result)) {
				JSValue thrown = JS_GetException(context);
				if (firstError.empty()) {
					if (const char *text = JS_ToCString(context, thrown); text != nullptr) {
						firstError = text;
						JS_FreeCString(context, text);
					}
				}
				JS_FreeValue(context, thrown);
			}
			JS_FreeValue(context, result);
			JS_FreeValue(context, argument);
			Release(context, await.Resolve);
			Release(context, await.Reject);
		}
		return firstError;
	}

	void ReleaseJsHostCallback(JSContext *context, HostCallback callback) {
		if (!callback.Valid()) {
			return;
		}

		JsContext &bound = JsOf(context);
		const auto found = bound.HostCallbacks.find(callback.Id);
		if (found == bound.HostCallbacks.end()) {
			return;
		}

		Release(context, found->second);
		bound.HostCallbacks.erase(found);
	}
}
