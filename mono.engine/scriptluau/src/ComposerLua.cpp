#include "ComposerLuaState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/PcxExpression.hpp>
#include <engine/scriptluau/ComposerLua.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <luacode.h>
#include <unordered_map>
#include <unordered_set>
namespace engine::script {
	namespace composer_lua {
		namespace {
			const char *BOOTSTRAP = R"LUA(
Project={frame=0,frameTotal=1,fps=30}
abs=math.abs;floor=math.floor;ceil=math.ceil;max=math.max;min=math.min;sqrt=math.sqrt;exp=math.exp;ln=math.log;sin=math.sin;cos=math.cos;tan=math.tan;asin=math.asin;acos=math.acos;atan=math.atan;atan2=math.atan2
function round(x) local f=math.floor(x) local d=x-f if d>0.5 or (d==0.5 and f%2~=0) then return f+1 end return f end
function clamp(x,a,b)return math.max(a,math.min(x,b))end
function lerp(a,b,t)return a+(b-a)*t end
function sqr(x)return x*x end
function power(a,b)return a^b end
function log2(x)return math.log(x)/math.log(2)end
function log10(x)return math.log(x)/math.log(10)end
function logn(n,x)return math.log(x)/math.log(n)end
rad=math.rad;deg=math.deg
function dsin(x)return math.sin(math.rad(x))end
function dcos(x)return math.cos(math.rad(x))end
function dtan(x)return math.tan(math.rad(x))end
function dasin(x)return math.deg(math.asin(x))end
function dacos(x)return math.deg(math.acos(x))end
function datan(x)return math.deg(math.atan(x))end
function datan2(y,x)return math.deg(math.atan2(y,x))end
function dot(a,b,c,d)return a*c+b*d end
stringUpper=string.upper;stringLower=string.lower
function stringLength(s)return utf8.len(s)end
function stringSearch(a,b)local p=string.find(b,a,1,true) return p or 0 end
function stringCopy(s,start,amount)local a=utf8.offset(s,math.max(1,start)) if not a or amount<=0 then return '' end local b=utf8.offset(s,amount+1,a) return string.sub(s,a,b and b-1 or #s)end
function stringReplace(s,a,b)local p=string.find(s,a,1,true)if not p or a=='' then return s end return string.sub(s,1,p-1)..b..string.sub(s,p+#a)end
function stringReplaceAll(s,a,b)if a=='' then return s end local result='' local offset=1 while true do local p=string.find(s,a,offset,true)if not p then return result..string.sub(s,offset)end result=result..string.sub(s,offset,p-1)..b offset=p+#a end end
function stringSplit(s,d)local result={} if d=='' then for _,c in utf8.codes(s)do table.insert(result,utf8.char(c))end return result end local offset=1 while true do local p=string.find(s,d,offset,true)if not p then table.insert(result,string.sub(s,offset))return result end table.insert(result,string.sub(s,offset,p-1))offset=p+#d end end
function colorGetRed(c)return bit32.band(c,255)end
function colorGetGreen(c)return bit32.band(bit32.rshift(c,8),255)end
function colorGetBlue(c)return bit32.band(bit32.rshift(c,16),255)end
function colorCreateRGB(r,g,b,n)local scale=n and 255 or 1 return bit32.bor(math.floor(clamp(r*scale,0,255)),bit32.lshift(math.floor(clamp(g*scale,0,255)),8),bit32.lshift(math.floor(clamp(b*scale,0,255)),16))end
local function hsv(c)local r,g,b=colorGetRed(c)/255,colorGetGreen(c)/255,colorGetBlue(c)/255 local v=math.max(r,g,b)local m=math.min(r,g,b)local d=v-m local h=0 if d~=0 then if v==r then h=((g-b)/d)%6 elseif v==g then h=(b-r)/d+2 else h=(r-g)/d+4 end h=h/6 end return h,v==0 and 0 or d/v,v end
function colorGetHue(c)local h=hsv(c)return math.floor(h*255)end
function colorGetSaturation(c)local _,s=hsv(c)return math.floor(s*255)end
function colorGetValue(c)local _,_,v=hsv(c)return math.floor(v*255)end
function colorCreateHSV(h,s,v,n)local scale=n and 1 or 255 h=(h/scale)%1 s=clamp(s/scale,0,1) v=clamp(v/scale,0,1)local section=h*6 local k=math.floor(section)local f=section-k local p=v*(1-s)local q=v*(1-f*s)local t=v*(1-(1-f)*s)local r,g,b if k==0 then r,g,b=v,t,p elseif k==1 then r,g,b=q,v,p elseif k==2 then r,g,b=p,v,t elseif k==3 then r,g,b=p,q,v elseif k==4 then r,g,b=t,p,v else r,g,b=v,p,q end return colorCreateRGB(r,g,b,true)end
function colorMerge(a,b,t)return colorCreateRGB(lerp(colorGetRed(a),colorGetRed(b),t),lerp(colorGetGreen(a),colorGetGreen(b),t),lerp(colorGetBlue(a),colorGetBlue(b),t))end
)LUA";
			void *Allocate(void *opaque, void *pointer, size_t oldSize, size_t newSize) {
				auto &session = *static_cast<Session *>(opaque);
				auto &ledger = *session.Memory;
				if (!pointer) oldSize = 0;
				if (!newSize) {
					ledger.VmBytes -= oldSize;
					std::free(pointer);
					return nullptr;
				}
				if (oldSize > ledger.VmBytes || newSize > ledger.MaximumBytes - (ledger.VmBytes - oldSize) ||
					ledger.ExternalBytes > ledger.MaximumBytes - (ledger.VmBytes - oldSize) - newSize)
					return nullptr;
				void *next = std::realloc(pointer, newSize);
				if (next) {
					ledger.VmBytes = ledger.VmBytes - oldSize + newSize;
					core::Metrics::Count("imagegraph.lua.vm.allocation_operations", 1);
					core::Metrics::Count("imagegraph.lua.vm.allocated_bytes", newSize);
				}
				return next;
			}
			void Interrupt(lua_State *state, int gc) {
				if (gc >= 0) return;
				auto &session = Of(state);
				if (++session.Steps - session.Base > session.StepBudget)
					luaL_errorL(state, "Lua exceeded its instruction budget");
			}
			bool Run(Session &session, std::string_view source, std::string_view name, std::string &failure) {
				ENGINE_PROFILE_CAT("composer lua compile", core::ProfileCategory::Script);
				lua_CompileOptions options{};
				options.optimizationLevel = 1;
				options.debugLevel = 1;
				size_t size = 0;
				char *bytecode = luau_compile(source.data(), source.size(), &options, &size);
				if (!bytecode) {
					failure = "Lua bytecode allocation failed";
					return false;
				}
				const std::string label(name);
				const int loaded = luau_load(session.State, label.c_str(), bytecode, size, 0);
				std::free(bytecode);
				if (loaded != 0 || lua_pcall(session.State, 0, 0, 0) != 0) {
					const char *message = lua_tostring(session.State, -1);
					failure = message ? message : "Lua execution failed";
					lua_settop(session.State, 0);
					return false;
				}
				if (session.Steps - session.Base > session.StepBudget) {
					failure = "Lua exceeded its instruction budget";
					return false;
				}
				return true;
			}
			// Protect native preparation too: VM allocations may fail before a Lua function is called.
			bool Protect(Session &session, const std::function<bool()> &body, std::string &failure) {
				struct Call {
					const std::function<bool()> *Body;
					bool Result = false;
				} call{&body};
				const int status = lua_cpcall(
					session.State,
					[](lua_State *state) -> int {
						auto &active = *static_cast<Call *>(lua_touserdata(state, 1));
						active.Result = (*active.Body)();
						return 0;
					},
					&call
				);
				if (status != 0) {
					const char *message = lua_tostring(session.State, -1);
					failure = message ? message : "Lua protected execution failed";
					lua_settop(session.State, 0);
					session.Target = nullptr;
					return false;
				}
				return call.Result;
			}
			struct NativeCharge {
				Ledger &Memory;
				uint64_t Bytes = 0;
				~NativeCharge() {
					Memory.ExternalBytes -= Bytes;
				}
				bool Add(uint64_t bytes) {
					if (bytes > Memory.MaximumBytes - Memory.VmBytes - Memory.ExternalBytes) return false;
					Memory.ExternalBytes += bytes;
					Bytes += bytes;
					return true;
				}
				uint64_t Retain(uint64_t bytes) {
					Bytes -= bytes;
					return bytes;
				}
			};

			const Value *Find(const HostNodeInvocation &invocation, std::string_view port) {
				for (const auto &input : invocation.Inputs)
					if (input.Port == port) return &input.Data;
				return nullptr;
			}
			std::string_view TextInput(const HostNodeInvocation &invocation, std::string_view port) {
				const Value *value = Find(invocation, port);
				const auto *text = value ? std::get_if<std::string>(value) : nullptr;
				return text ? std::string_view(*text) : std::string_view{};
			}
			bool Boolean(const HostNodeInvocation &invocation, std::string_view port, bool fallback) {
				const Value *value = Find(invocation, port);
				if (const auto *flag = value ? std::get_if<bool>(value) : nullptr) return *flag;
				return fallback;
			}
			bool Identifier(std::string_view text) {
				if (text.empty() || !((text[0] >= 'a' && text[0] <= 'z') ||
									  (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_'))
					return false;
				for (char c : text)
					if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
						  c == '_'))
						return false;
				return true;
			}
			struct Conversion {
				Session &Owner;
				size_t Count = 0;
				uint64_t Bytes = 0;
				std::unordered_set<const void *> Tables;
				std::string &Failure;
				bool Admit(size_t depth, uint64_t bytes = 0) {
					if (depth > Limits::MaximumArrayDepth || ++Count > Limits::MaximumArrayElements ||
						bytes > Limits::MaximumArrayBytes - Bytes) {
						Failure = "Lua value exceeds native depth, element or byte limits";
						return false;
					}
					Bytes += bytes;
					return true;
				}
				bool PushItem(const SourceArrayItem &item, size_t depth) {
					return std::visit(
						[&](const auto &value) -> bool {
							using T = std::decay_t<decltype(value)>;
							if constexpr (std::is_same_v<T, ElementValue>)
								return std::visit(
									[&](const auto &leaf) { return Push(Value{leaf}, depth); }, value
								);
							else if constexpr (std::is_same_v<T, Image>)
								return Push(Value{SurfaceValue{value}}, depth);
							else {
								if (!Admit(depth, value.size() * sizeof(SourceArrayItem))) return false;
								lua_createtable(Owner.State, int(value.size()), 0);
								for (size_t i = 0; i < value.size(); ++i) {
									if (!PushItem(value[i], depth + 1)) return false;
									lua_rawseti(Owner.State, -2, int(i + 1));
								}
								return true;
							}
						},
						item.Data
					);
				}
				bool Push(const Value &value, size_t depth = 1) {
					if (!Admit(depth)) return false;
					lua_State *state = Owner.State;
					if (!lua_checkstack(state, 4)) {
						Failure = "Lua value exceeds VM stack budget";
						return false;
					}
					return std::visit(
						[&](const auto &item) -> bool {
							using T = std::decay_t<decltype(item)>;
							if constexpr (std::is_same_v<T, UndefinedValue>)
								lua_pushnil(state);
							else if constexpr (std::is_same_v<T, bool>)
								lua_pushboolean(state, item);
							else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t>)
								lua_pushnumber(state, double(item));
							else if constexpr (std::is_same_v<T, EnumValue>)
								lua_pushinteger(state, item.Value);
							else if constexpr (std::is_same_v<T, std::string>) {
								if (!Admit(depth, item.size())) return false;
								lua_pushlstring(state, item.data(), item.size());
							} else if constexpr (std::is_same_v<T, Colour>)
								lua_pushnumber(
									state,
									uint32_t(item.Red) | (uint32_t(item.Green) << 8) |
										(uint32_t(item.Blue) << 16)
								);
							else if constexpr (std::is_same_v<T, StructValue>) {
								lua_newtable(state);
								if (item.Data)
									for (const auto &[key, member] : item.Data->Fields) {
										if (!Push(member, depth + 1)) return false;
										lua_setfield(state, -2, key.c_str());
									}
							} else if constexpr (std::is_same_v<T, ArrayValue>) {
								lua_newtable(state);
								size_t index = 1;
								if (!item.Items.empty()) {
									for (const auto &member : item.Items) {
										if (!PushItem(member, depth + 1)) return false;
										lua_rawseti(state, -2, int(index++));
									}
								} else if (!item.Nested.empty()) {
									for (const auto &row : item.Nested) {
										lua_newtable(state);
										size_t child = 1;
										for (const auto &leaf : row) {
											if (!std::visit(
													[&](const auto &member) {
														return Push(Value{member}, depth + 2);
													},
													leaf
												))
												return false;
											lua_rawseti(state, -2, int(child++));
										}
										lua_rawseti(state, -2, int(index++));
									}
								} else
									for (const auto &leaf : item.Elements) {
										if (!std::visit(
												[&](const auto &member) {
													return Push(Value{member}, depth + 1);
												},
												leaf
											))
											return false;
										lua_rawseti(state, -2, int(index++));
									}
							} else if constexpr (std::is_same_v<T, SurfaceValue>) {
								const auto bytes = item.Data.Pixels.size();
								if (!Admit(depth, bytes)) {
									Failure = "Lua retained surface exceeds memory budget";
									return false;
								}
								size_t index = 0;
								while (index < Owner.Surfaces.size() && Owner.Surfaces[index] != item)
									++index;
								if (index == Owner.Surfaces.size()) {
									if (bytes > Owner.Memory->MaximumBytes - Owner.Memory->VmBytes -
													Owner.Memory->ExternalBytes) {
										Failure = "Lua retained surface exceeds memory budget";
										return false;
									}
									Owner.Memory->ExternalBytes += bytes;
									Owner.Surfaces.push_back(item);
								}
								auto *reference = static_cast<uint32_t *>(
									lua_newuserdatatagged(state, sizeof(uint32_t), SURFACE_TAG)
								);
								*reference = uint32_t(index);
							} else if constexpr (std::is_same_v<T, BufferValue>) {
								if (!Admit(depth, item.Bytes.size())) return false;
								size_t index = 0;
								while (index < Owner.Buffers.size() && Owner.Buffers[index] != item)
									++index;
								if (index == Owner.Buffers.size()) {
									if (item.Bytes.size() > Owner.Memory->MaximumBytes -
																Owner.Memory->VmBytes -
																Owner.Memory->ExternalBytes) {
										Failure = "Lua retained buffer exceeds memory budget";
										return false;
									}
									Owner.Memory->ExternalBytes += item.Bytes.size();
									Owner.Buffers.push_back(item);
								}
								auto *reference = static_cast<uint32_t *>(
									lua_newuserdatatagged(state, sizeof(uint32_t), BUFFER_TAG)
								);
								*reference = uint32_t(index);
							} else if constexpr (std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> ||
												 std::is_same_v<T, Vector4> ||
												 std::is_same_v<T, Quaternion>) {
								lua_newtable(state);
								lua_pushnumber(state, item.X);
								lua_rawseti(state, -2, 1);
								lua_pushnumber(state, item.Y);
								lua_rawseti(state, -2, 2);
								if constexpr (!std::is_same_v<T, Vector2>) {
									lua_pushnumber(state, item.Z);
									lua_rawseti(state, -2, 3);
								}
								if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>) {
									lua_pushnumber(state, item.W);
									lua_rawseti(state, -2, 4);
								}
							} else {
								Failure = "value type is not in Pixel Composer Lua argument vocabulary";
								return false;
							}
							return true;
						},
						value
					);
				}
				bool Read(int index, Value &output, size_t depth = 1) {
					if (!Admit(depth)) return false;
					lua_State *state = Owner.State;
					index = lua_absindex(state, index);
					switch (lua_type(state, index)) {
					case LUA_TNIL:
						output = UndefinedValue{};
						return true;
					case LUA_TBOOLEAN:
						output = bool(lua_toboolean(state, index));
						return true;
					case LUA_TNUMBER: {
						const double value = lua_tonumber(state, index);
						if (!std::isfinite(value)) {
							Failure = "Lua returned a nonfinite number";
							return false;
						}
						output = value;
						return true;
					}
					case LUA_TSTRING: {
						size_t size = 0;
						const char *text = lua_tolstring(state, index, &size);
						if (!Admit(depth, size) || size > Limits::MaximumTextBytes) return false;
						output = std::string(text, size);
						return true;
					}
					case LUA_TUSERDATA: {
						if (const auto *resource =
								static_cast<uint32_t *>(lua_touserdatatagged(state, index, SURFACE_TAG))) {
							if (*resource >= Owner.Surfaces.size()) return false;
							const auto &surface = Owner.Surfaces[*resource];
							if (!Admit(depth, surface.Data.Pixels.size())) return false;
							output = surface;
							return true;
						}
						if (const auto *resource =
								static_cast<uint32_t *>(lua_touserdatatagged(state, index, BUFFER_TAG))) {
							if (*resource >= Owner.Buffers.size()) return false;
							const auto &buffer = Owner.Buffers[*resource];
							if (!Admit(depth, buffer.Bytes.size())) return false;
							output = buffer;
							return true;
						}
						break;
					}
					case LUA_TTABLE: {
						const void *identity = lua_topointer(state, index);
						if (!Tables.insert(identity).second) {
							Failure = "Lua returned a cyclic table";
							return false;
						}
						bool numeric = true;
						size_t entries = 0, maxIndex = 0;
						lua_pushnil(state);
						while (lua_next(state, index)) {
							++entries;
							if (entries > Limits::MaximumArrayElements) {
								lua_pop(state, 2);
								Failure = "Lua table exceeds entry limit";
								return false;
							}
							if (lua_type(state, -2) != LUA_TNUMBER)
								numeric = false;
							else {
								const double key = lua_tonumber(state, -2);
								if (key < 1 || key > Limits::MaximumArrayElements || std::trunc(key) != key)
									numeric = false;
								else
									maxIndex = std::max(maxIndex, size_t(key));
							}
							lua_pop(state, 1);
						}
						if (numeric && entries > 0 && maxIndex == entries) {
							ArrayValue array{ValueType::Any, {}};
							array.Items.reserve(entries);
							for (size_t member = 1; member <= entries; ++member) {
								lua_rawgeti(state, index, int(member));
								Value value;
								if (!Read(-1, value, depth + 1)) {
									lua_pop(state, 1);
									return false;
								}
								lua_pop(state, 1);
								if (auto *nested = std::get_if<ArrayValue>(&value))
									array.Items.push_back({std::move(nested->Items)});
								else
									array.Items.push_back({std::visit(
										[](const auto &leaf) -> ElementValue {
											using T = std::decay_t<decltype(leaf)>;
											if constexpr (std::is_same_v<T, ArrayValue>)
												return double{0};
											else
												return leaf;
										},
										value
									)});
							}
							output = std::move(array);
						} else {
							StructValue object;
							auto &fields = object.Data.emplace().Fields;
							fields.reserve(entries);
							lua_pushnil(state);
							while (lua_next(state, index)) {
								if (lua_type(state, -2) != LUA_TSTRING) {
									lua_pop(state, 2);
									Failure = "Lua struct keys must be text";
									return false;
								}
								size_t size = 0;
								const char *text = lua_tolstring(state, -2, &size);
								if (!Admit(depth, size)) {
									lua_pop(state, 2);
									return false;
								}
								std::string key(text, size);
								Value value;
								if (!Read(-1, value, depth + 1)) {
									lua_pop(state, 2);
									return false;
								}
								fields.emplace_back(std::move(key), std::move(value));
								lua_pop(state, 1);
							}
							std::sort(fields.begin(), fields.end(), [](const auto &a, const auto &b) {
								return a.first < b.first;
							});
							output = std::move(object);
						}
						Tables.erase(identity);
						return true;
					}
					default:
						break;
					}
					Failure = "Lua returned an unsupported value type";
					return false;
				}
			};
			struct CachedNode {
				std::string Id;
				Node Authored;
				std::vector<AuthoredValue> Inputs;
				std::vector<HostImageBinding> Images;
				uint64_t Tick = 0;
				double Subframe = 0;
				bool Negative = false, Ready = false;
				uint64_t RetainedBytes = 0;
				Value Result = double{0};
				Image Surface;
			};
			class Host final : public ComposerLuaHost {
				ComposerLuaLimits LimitsValue;
				Ledger Memory;
				std::unordered_map<std::string, std::unique_ptr<Session>> Sessions;
				std::unordered_map<std::string, CachedNode> Cached;
				std::vector<ComposerLuaMessage> PcxNotifications;
				uint64_t PcxNotificationBytes = 0;
				bool Initialize(Session &session, std::string &failure) {
					session.Memory = &Memory;
					session.StepBudget = LimitsValue.MaximumSteps;
					session.PixelLimit = LimitsValue.MaximumPixelVisits;
					session.State = lua_newstate(Allocate, &session);
					if (!session.State) {
						failure = "Lua VM memory budget exhausted";
						return false;
					}
					lua_setthreaddata(session.State, &session);
					return Protect(
						session,
						[&]() {
							luaL_openlibs(session.State);
							for (const char *name :
								 {"os",
								  "io",
								  "debug",
								  "require",
								  "loadstring",
								  "getfenv",
								  "setfenv",
								  "collectgarbage"}) {
								lua_pushnil(session.State);
								lua_setglobal(session.State, name);
							}
							lua_getglobal(session.State, "math");
							lua_pushnil(session.State);
							lua_setfield(session.State, -2, "random");
							lua_pushnil(session.State);
							lua_setfield(session.State, -2, "randomseed");
							lua_pop(session.State, 1);
							lua_callbacks(session.State)->interrupt = Interrupt;
							lua_callbacks(session.State)->userthread = [](lua_State *parent,
																		  lua_State *child) {
								if (parent) lua_setthreaddata(child, lua_getthreaddata(parent));
							};
							InstallDrawing(session.State);
							return Run(session, BOOTSTRAP, "composer-builtins", failure);
						},
						failure
					);
				}

			  public:
				explicit Host(const ComposerLuaLimits &limits) : LimitsValue(limits) {
					Memory.MaximumBytes = limits.MaximumMemoryBytes;
				}
				bool PcxMessages(
					std::string_view node, std::span<const PcxMessage> messages, std::string &failure
				) override {
					uint64_t bytes = 0;
					for (const auto &message : messages)
						bytes += message.Text.size() + node.size() + sizeof(ComposerLuaMessage);
					if (messages.size() > 256 - PcxNotifications.size() ||
						bytes > 65536 - PcxNotificationBytes ||
						bytes > Memory.MaximumBytes - Memory.VmBytes - Memory.ExternalBytes) {
						failure = "PCX notifications exceed their host budget";
						return false;
					}
					for (const auto &message : messages)
						PcxNotifications.push_back(
							{std::string(node), message.Text, Memory.MessageOrder++, message.Warning}
						);
					Memory.ExternalBytes += bytes;
					PcxNotificationBytes += bytes;
					failure.clear();
					return true;
				}
				std::vector<ComposerLuaMessage> TakeMessages() override {
					std::vector<ComposerLuaMessage> output = std::move(PcxNotifications);
					PcxNotifications.clear();
					Memory.ExternalBytes -= PcxNotificationBytes;
					PcxNotificationBytes = 0;
					for (auto &[id, session] : Sessions) {
						for (auto &message : session->Messages)
							output.push_back(std::move(message));
						session->Messages.clear();
						Memory.ExternalBytes -= session->MessageBytes;
						session->MessageBytes = 0;
					}
					std::sort(output.begin(), output.end(), [](const auto &a, const auto &b) {
						return a.Order < b.Order;
					});
					return output;
				}
				void Reset() override {
					Sessions.clear();
					Cached.clear();
					PcxNotifications.clear();
					PcxNotificationBytes = 0;
					Memory = {0, 0, LimitsValue.MaximumMemoryBytes};
				}
				bool Capture(
					const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
				) override {
					ENGINE_PROFILE_CAT("composer lua host", core::ProfileCategory::Script);
					failure.clear();
					const auto type = invocation.Authored.Type;
					if (type != "pc.lua_compute" && type != "pc.lua_global" && type != "pc.lua_surface") {
						failure = "provider accepts only Pixel Composer Lua nodes";
						return false;
					}
					if (!LimitsValue.MaximumMemoryBytes ||
						LimitsValue.MaximumMemoryBytes > Limits::MaximumEvaluationBytes ||
						!LimitsValue.MaximumSteps || !LimitsValue.MaximumPixelVisits ||
						!LimitsValue.MaximumStates || LimitsValue.MaximumStates > Limits::MaximumNodes ||
						LimitsValue.MaximumSourceBytes > Limits::MaximumTextBytes) {
						failure = "Lua host limits are invalid";
						return false;
					}
					const auto code = TextInput(invocation, "lua_code");
					if (code.size() > LimitsValue.MaximumSourceBytes) {
						failure = "Lua source exceeds its byte budget";
						return false;
					}
					std::string threadId = "composer-lua:" + invocation.Authored.Id;
					if (type != "pc.lua_global") {
						const auto *linked = Find(invocation, "execution_thread");
						if (const auto *thread =
								linked ? std::get_if<ExecutionThreadValue>(linked) : nullptr) {
							if (!Sessions.contains(thread->Id)) {
								failure = "Lua execution thread is stale";
								return false;
							}
							threadId = thread->Id;
						}
					}
					auto found = Sessions.find(threadId);
					if (found == Sessions.end()) {
						if (Sessions.size() == LimitsValue.MaximumStates) {
							failure = "Lua session count exceeds its budget";
							return false;
						}
						auto session = std::make_unique<Session>();
						session->Id = threadId;
						session->RandomState = invocation.Request.Seed | 1;
						if (!Initialize(*session, failure)) return false;
						found = Sessions.emplace(threadId, std::move(session)).first;
					}
					auto &session = *found->second;
					return Protect(
						session,
						[&]() {
							session.Base = session.Steps;
							session.ActiveNode = invocation.Authored.Id;
							session.PixelVisits = 0;
							lua_settop(session.State, 0);
							if (Cached.size() == LimitsValue.MaximumStates &&
								!Cached.contains(invocation.Authored.Id)) {
								failure = "Lua node cache exceeds its budget";
								return false;
							}
							auto &cache = Cached[invocation.Authored.Id];
							bool same =
								cache.Ready && cache.Authored == invocation.Authored &&
								cache.Inputs.size() == invocation.Inputs.size() &&
								std::equal(
									cache.Inputs.begin(), cache.Inputs.end(), invocation.Inputs.begin()
								);
							bool frame = false;
							if (type == "pc.lua_global") {
								const auto *order = Find(invocation, "run_order");
								frame = order &&
										((std::get_if<EnumValue>(order) &&
										  std::get<EnumValue>(*order).Value == 1) ||
										 (std::get_if<int64_t>(order) && std::get<int64_t>(*order) == 1));
							} else
								frame = Boolean(invocation, "execute_on_frame", true);
							if (frame && (cache.Tick != invocation.Request.Tick ||
										  cache.Subframe != invocation.Request.Subframe ||
										  cache.Negative != invocation.Request.NegativeFrame))
								same = false;
							std::vector<HostImageBinding> bindings;
							for (const auto &image : invocation.Images) {
								if (!image.Data) {
									failure = "Lua surface input is missing";
									return false;
								}
								bindings.push_back({std::string(image.Port), SurfaceHash(*image.Data)});
							}
							if (bindings.size() != cache.Images.size() ||
								!std::equal(
									bindings.begin(),
									bindings.end(),
									cache.Images.begin(),
									[](const auto &a, const auto &b) {
										return a.Port == b.Port && a.Hash == b.Hash;
									}
								))
								same = false;
							lua_getglobal(session.State, "Project");
							const double frameNumber =
								double(invocation.Request.Tick) + invocation.Request.Subframe;
							lua_pushnumber(
								session.State, invocation.Request.NegativeFrame ? -frameNumber : frameNumber
							);
							lua_setfield(session.State, -2, "frame");
							lua_pushnumber(
								session.State, invocation.Timeline ? double(invocation.Timeline->Frames) : 1
							);
							lua_setfield(session.State, -2, "frameTotal");
							lua_pushnumber(
								session.State, invocation.Timeline ? invocation.Timeline->FramesPerSecond : 30
							);
							lua_setfield(session.State, -2, "fps");
							lua_pop(session.State, 1);
							const auto authoredBytes = NodeClonePayloadBytes(invocation.Authored);
							uint64_t controlBytes = authoredBytes.value_or(0);
							if (!authoredBytes || invocation.Inputs.size() > Limits::MaximumLinks ||
								invocation.Images.size() > Limits::MaximumLinks) {
								failure = "Lua authored controls exceed native payload limits";
								return false;
							}
							for (const auto &input : invocation.Inputs) {
								const auto bytes = ValueClonePayloadBytes(input.Data);
								if (!bytes ||
									*bytes > LimitsValue.MaximumMemoryBytes -
												 std::min(controlBytes, LimitsValue.MaximumMemoryBytes)) {
									failure = "Lua input payload exceeds memory budget";
									return false;
								}
								controlBytes += *bytes + input.Port.size() + sizeof(AuthoredValue);
							}
							for (const auto &binding : bindings)
								controlBytes += sizeof(HostImageBinding) + binding.Port.size();
							NativeCharge cacheCharge{Memory};
							if (!same && (!cacheCharge.Add(controlBytes) ||
										  !cacheCharge.Add(Limits::MaximumArrayBytes))) {
								failure = "Lua cached controls and return value exceed memory budget";
								return false;
							}

							if (!same) {
								cache.Ready = false;
								Node nextAuthored = invocation.Authored;
								std::vector<AuthoredValue> nextInputs(
									invocation.Inputs.begin(), invocation.Inputs.end()
								);
								Value nextResult = cache.Result;
								if (type == "pc.lua_global") {
									if (!Run(session, code, invocation.Authored.Id, failure)) return false;
								} else {
									std::string function(TextInput(invocation, "function_name"));
									if (function.empty()) function = "composerFunction";
									if (!Identifier(function)) {
										failure = "Lua function name must be an identifier";
										return false;
									}
									const auto *catalogue = FindCatalogueEntry(type);
									if (!catalogue) {
										failure = "Lua catalogue is unavailable";
										return false;
									}
									std::vector<std::pair<size_t, std::string_view>> arguments;
									for (const auto &input : invocation.Authored.DynamicInputs) {
										size_t group = 0;
										const auto *part = FindDynamicTemplate(*catalogue, input.Id, group);
										if (part && part->Id == "argument_name")
											arguments.emplace_back(group, input.Id);
									}
									std::sort(arguments.begin(), arguments.end());
									std::string source = "function " + function + "(";
									std::vector<std::string_view> valuePorts;
									for (const auto &[group, port] : arguments) {
										const auto name = TextInput(invocation, port);
										if (name.empty()) continue;
										if (!Identifier(name)) {
											failure = "Lua argument name must be an identifier";
											return false;
										}
										if (!valuePorts.empty()) source += ",";
										source += name;
										std::string_view valuePort;
										for (const auto &input : invocation.Authored.DynamicInputs) {
											size_t candidate = 0;
											const auto *part =
												FindDynamicTemplate(*catalogue, input.Id, candidate);
											if (part && candidate == group && part->Id == "argument_value")
												valuePort = input.Id;
										}
										valuePorts.push_back(valuePort);
									}
									source += ")\n";
									source += code;
									source += "\nend";
									if (source.size() > LimitsValue.MaximumSourceBytes) {
										failure = "Lua compiled function exceeds source budget";
										return false;
									}
									if (!Run(session, source, invocation.Authored.Id, failure)) return false;
									if (type == "pc.lua_surface") {
										Vector2 dimensions{32, 32};
										if (const auto *value = Find(invocation, "output_dimension"))
											if (const auto *size = std::get_if<Vector2>(value))
												dimensions = *size;
										if (!std::isfinite(dimensions.X) || !std::isfinite(dimensions.Y) ||
											dimensions.X < 1 || dimensions.Y < 1 ||
											dimensions.X > invocation.Request.MaximumImageDimension ||
											dimensions.Y > invocation.Request.MaximumImageDimension) {
											failure = "Lua output dimensions exceed image budget";
											return false;
										}
										if (!invocation.OutputFormat) {
											failure = "Lua output surface format is unresolved";
											return false;
										}
										const auto layout = CheckedSurfaceLayout(
											uint32_t(dimensions.X),
											uint32_t(dimensions.Y),
											*invocation.OutputFormat,
											std::min(
												invocation.MaximumOperationBytes, Limits::MaximumOutputBytes
											)
										);
										if (!layout) {
											failure = "Lua output exceeds byte budget";
											return false;
										}
										if (cache.Surface.Width != uint32_t(dimensions.X) ||
											cache.Surface.Height != uint32_t(dimensions.Y) ||
											cache.Surface.Format != *invocation.OutputFormat) {
											if (layout->Bytes >
												Memory.MaximumBytes - Memory.VmBytes - Memory.ExternalBytes) {
												failure = "Lua target exceeds memory budget";
												return false;
											}
											Memory.ExternalBytes += layout->Bytes;
											Memory.ExternalBytes -= cache.Surface.Pixels.size();
											cache.Surface = {
												uint32_t(dimensions.X),
												uint32_t(dimensions.Y),
												std::vector<uint8_t>(size_t(layout->Bytes), 0),
												0,
												*invocation.OutputFormat
											};
										}
										session.Target = &cache.Surface;
									}
									lua_getglobal(session.State, function.c_str());
									Conversion conversion{session, 0, 0, {}, failure};
									for (auto port : valuePorts) {
										const auto *value = Find(invocation, port);
										const Image *image = nullptr;
										for (const auto &entry : invocation.Images)
											if (entry.Port == port) image = entry.Data;
										const Value fallback =
											image ? Value{SurfaceValue{*image}} : Value{double{0}};
										if (!conversion.Push(value ? *value : fallback)) {
											session.Target = nullptr;
											lua_settop(session.State, 0);
											return false;
										}
									}
									const int status = lua_pcall(
										session.State,
										int(valuePorts.size()),
										type == "pc.lua_compute" ? 1 : 0,
										0
									);
									session.Target = nullptr;
									session.Alpha = 1;
									if (status != 0) {
										const char *message = lua_tostring(session.State, -1);
										failure = message ? message : "Lua call failed";
										lua_settop(session.State, 0);
										return false;
									}
									if (session.Steps - session.Base > session.StepBudget) {
										failure = "Lua exceeded its instruction budget";
										return false;
									}
									if (type == "pc.lua_compute") {
										Conversion result{session, 0, 0, {}, failure};
										if (!result.Read(-1, nextResult)) return false;
										lua_pop(session.State, 1);
										if (!ValueClonePayloadBytes(nextResult)) {
											failure = "Lua return value is outside native payload limits";
											return false;
										}
									} else
										cache.Surface.Hash = SurfaceHash(cache.Surface);
								}
								cache.Authored = std::move(nextAuthored);
								cache.Inputs = std::move(nextInputs);
								cache.Result = std::move(nextResult);
								cache.Images = bindings;
								cache.Tick = invocation.Request.Tick;
								cache.Subframe = invocation.Request.Subframe;
								cache.Negative = invocation.Request.NegativeFrame;
								const auto resultBytes = ValueClonePayloadBytes(cache.Result);
								const uint64_t retained = controlBytes + resultBytes.value_or(0);
								if (!resultBytes || retained > cacheCharge.Bytes) {
									failure = "Lua cached result exceeds its reserved budget";
									return false;
								}
								Memory.ExternalBytes -= cache.RetainedBytes;
								cache.RetainedBytes = cacheCharge.Retain(retained);
								cache.Ready = true;
							}
							const auto resultBytes = ValueClonePayloadBytes(cache.Result);
							const uint64_t captureBytes =
								controlBytes + resultBytes.value_or(0) + cache.Surface.Pixels.size();
							if (!resultBytes || captureBytes > invocation.MaximumOperationBytes) {
								failure = "Lua host recording exceeds operation byte budget";
								return false;
							}
							HostNodeCapture captured;
							captured.Authored = invocation.Authored;
							captured.Tick = invocation.Request.Tick;
							captured.Subframe = invocation.Request.Subframe;
							captured.NegativeFrame = invocation.Request.NegativeFrame;
							captured.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
							captured.InputImages = bindings;
							captured.Outputs.push_back({"execution_thread", ExecutionThreadValue{threadId}});
							if (type == "pc.lua_compute")
								captured.Outputs.push_back({"return_value", cache.Result});
							else if (type == "pc.lua_surface")
								captured.Images.push_back({"surface_out", cache.Surface});
							output = std::move(captured);
							return true;
						},
						failure
					);
				}
			};
		}
	}
	std::unique_ptr<imagegraph::ComposerLuaHost>
	MakeLuauComposerHost(const imagegraph::ComposerLuaLimits &limits) {
		return std::make_unique<composer_lua::Host>(limits);
	}
}
