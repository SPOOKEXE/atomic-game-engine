#include <engine/ang/Document.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/NodeCanvas.hpp>
#include <engine/imagegraph/AngGraph.hpp>
#include <engine/procedural/AngGraph.hpp>
#include <engine/script/NodeGraphService.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace engine::script {

	// Named rather than anonymous: unity builds compile this beside files with
	// their own `Text`, `Number` and `Store`.
	namespace node_graph_detail {
		// A table's identity in cache keys: its contents, never its address.
		uint64_t HashScript(uint64_t hash, const ScriptValue &value) {
			hash = ang::HashMix(hash, static_cast<uint64_t>(value.Tag));
			switch (value.Tag) {
			case ValueTag::Nil:
			case ValueTag::False:
			case ValueTag::True:
				return hash;
			case ValueTag::Number:
				return ang::HashMix(hash, std::bit_cast<uint64_t>(value.Number));
			case ValueTag::String:
				return ang::HashText(hash, value.Text);
			case ValueTag::Array:
				for (const ScriptValue &item : value.Items) {
					hash = HashScript(hash, item);
				}
				return hash;
			case ValueTag::Map:
				for (const auto &[key, item] : value.Entries) {
					hash = HashScript(ang::HashText(hash, key), item);
				}
				return hash;
			case ValueTag::Vector3:
				for (const float part : {value.Vector.X, value.Vector.Y, value.Vector.Z}) {
					hash = ang::HashMix(hash, std::bit_cast<uint32_t>(part));
				}
				return hash;
			case ValueTag::Color3:
				for (const float part : {value.Colour.R, value.Colour.G, value.Colour.B}) {
					hash = ang::HashMix(hash, std::bit_cast<uint32_t>(part));
				}
				return hash;
			case ValueTag::CFrame:
				return ang::HashBytes(hash, &value.Frame, sizeof(value.Frame));
			}
			return hash;
		}

		size_t ScriptBytes(const ScriptValue &value) {
			size_t bytes = sizeof(ScriptValue) + value.Text.size();
			for (const ScriptValue &item : value.Items) {
				bytes += ScriptBytes(item);
			}
			for (const auto &[key, item] : value.Entries) {
				bytes += key.size() + ScriptBytes(item);
			}
			return bytes;
		}

		ScriptValue Number(double number) {
			ScriptValue value(ValueTag::Number);
			value.Number = number;
			return value;
		}

		ScriptValue Text(std::string_view text) {
			ScriptValue value(ValueTag::String);
			value.Text = text;
			return value;
		}

		ScriptValue Boolean(bool flag) {
			ScriptValue value(flag ? ValueTag::True : ValueTag::False);
			value.Boolean = flag;
			return value;
		}

		ScriptValue Colour(const core::Color3 &colour) {
			ScriptValue value(ValueTag::Color3);
			value.Colour = colour;
			return value;
		}

		ScriptValue MapOf(std::vector<std::pair<std::string, ScriptValue>> entries) {
			ScriptValue value(ValueTag::Map);
			value.Entries = std::move(entries);
			return value;
		}

		const ScriptValue *Field(const ScriptValue &record, std::string_view key) {
			for (const auto &[name, value] : record.Entries) {
				if (name == key) {
					return &value;
				}
			}
			return nullptr;
		}

		// An empty Luau table reads as an empty array; treat it as an empty map.
		bool IsRecord(const ScriptValue &value) {
			return value.Tag == ValueTag::Map || (value.Tag == ValueTag::Array && value.Items.empty());
		}

		std::string FieldText(
			ScriptCall &call, const ScriptValue &record, std::string_view key, std::string fallback = {}
		) {
			const ScriptValue *value = Field(record, key);
			if (value == nullptr || value->Tag == ValueTag::Nil) {
				return fallback;
			}
			if (value->Tag != ValueTag::String || value->Text.size() > 256) {
				call.Raise(("NodeGraph: field " + std::string(key) + " must be a short string").c_str());
			}
			return value->Text;
		}

		double FieldNumber(const ScriptValue &record, std::string_view key, double fallback) {
			const ScriptValue *value = Field(record, key);
			return value != nullptr && value->Tag == ValueTag::Number ? value->Number : fallback;
		}

		bool FieldBoolean(const ScriptValue &record, std::string_view key, bool fallback) {
			const ScriptValue *value = Field(record, key);
			if (value == nullptr || value->Tag == ValueTag::Nil) {
				return fallback;
			}
			return value->Tag == ValueTag::True;
		}

		const std::vector<ScriptValue> &
		FieldList(ScriptCall &call, const ScriptValue &record, std::string_view key) {
			static const std::vector<ScriptValue> NONE;
			const ScriptValue *value = Field(record, key);
			if (value == nullptr || value->Tag == ValueTag::Nil ||
				(IsRecord(*value) && value->Items.empty())) {
				return NONE;
			}
			if (value->Tag != ValueTag::Array) {
				call.Raise(("NodeGraph: field " + std::string(key) + " must be a list").c_str());
			}
			return value->Items;
		}
	}

	ang::Value ToGraphValue(const ScriptValue &value, std::string_view type) {
		using namespace node_graph_detail;
		switch (value.Tag) {
		case ValueTag::Nil:
			return {};
		case ValueTag::False:
		case ValueTag::True:
			return ang::Value(value.Tag == ValueTag::True);
		case ValueTag::Number:
			if (type == "int") {
				return ang::Value(static_cast<int64_t>(std::llround(value.Number)));
			}
			return ang::Value(value.Number);
		case ValueTag::String:
			if (type == "name") {
				return ang::Value(core::Name(value.Text));
			}
			return ang::Value(value.Text);
		case ValueTag::Vector3:
			return ang::Value(value.Vector);
		case ValueTag::Color3:
			return ang::Value(value.Colour);
		case ValueTag::Array:
		case ValueTag::Map:
		case ValueTag::CFrame: {
			if (value.Tag == ValueTag::Map && (type == "vector2" || type == "vector4")) {
				const auto part = [&](std::string_view key) {
					return static_cast<float>(FieldNumber(value, key, 0.0));
				};
				if (type == "vector2") {
					return ang::Value(core::Vector2{part("X"), part("Y")});
				}
				return ang::Value(ang::Vector4{part("X"), part("Y"), part("Z"), part("W")});
			}
			auto copy = std::make_shared<ScriptValue>(value);
			const uint64_t identity = HashScript(ang::HASH_SEED, value) | 1u;
			return ang::Value::FromBlob(
				"table", {.Data = copy, .Bytes = ScriptBytes(value), .Identity = identity}
			);
		}
		}
		return {};
	}

	ScriptValue ToScriptValue(const ang::Value &value) {
		using namespace node_graph_detail;
		return std::visit(
			[&](const auto &held) -> ScriptValue {
				using Held = std::decay_t<decltype(held)>;
				if constexpr (std::is_same_v<Held, std::monostate>) {
					return ScriptValue{};
				} else if constexpr (std::is_same_v<Held, bool>) {
					return Boolean(held);
				} else if constexpr (std::is_same_v<Held, int64_t> || std::is_same_v<Held, double>) {
					return Number(static_cast<double>(held));
				} else if constexpr (std::is_same_v<Held, std::string>) {
					return Text(held);
				} else if constexpr (std::is_same_v<Held, core::Name>) {
					return Text(held.Text());
				} else if constexpr (std::is_same_v<Held, core::Vector3>) {
					ScriptValue vector(ValueTag::Vector3);
					vector.Vector = held;
					return vector;
				} else if constexpr (std::is_same_v<Held, core::Color3>) {
					return Colour(held);
				} else if constexpr (std::is_same_v<Held, core::Vector2>) {
					return MapOf({{"X", Number(held.X)}, {"Y", Number(held.Y)}});
				} else if constexpr (std::is_same_v<Held, ang::Vector4>) {
					return MapOf(
						{{"W", Number(held.W)},
						 {"X", Number(held.X)},
						 {"Y", Number(held.Y)},
						 {"Z", Number(held.Z)}}
					);
				} else {
					if (value.TypeName() == "table" && held.Data != nullptr) {
						return *static_cast<const ScriptValue *>(held.Data.get());
					}
					return ScriptValue{};
				}
			},
			value.Raw()
		);
	}

	// --- the per-VM objects ---------------------------------------------------------------------

	namespace node_graph_detail {
		struct Callbacks {
			std::function<void(CallbackRef)> Release;
			std::vector<CallbackRef> Held;

			Callbacks() = default;
			Callbacks(const Callbacks &) = delete;
			Callbacks &operator=(const Callbacks &) = delete;
			~Callbacks() {
				if (Release) {
					for (const CallbackRef ref : Held) {
						Release(ref);
					}
				}
			}
		};

		struct RegistryObject {
			ang::Registry Types = ang::Registry::WithScalars();
			std::shared_ptr<Callbacks> Retained = std::make_shared<Callbacks>();
			// The engine's own node types, which scripts read and do not extend.
			bool Locked = false;
		};

		struct GraphObject {
			std::shared_ptr<RegistryObject> Types{};
			ang::Graph Document{};
			bool ReadOnly = false;
			// The image graph this was opened from: its outputs, inputs and bindings.
			std::optional<imagegraph::Document> Image{};
		};

		struct HookEvent {
			std::string Name;
			ScriptValue Info;
		};

		// Filled by the runner's hooks, some on worker threads that may outlive
		// the runner.
		struct HookQueue {
			std::mutex Lock;
			std::vector<HookEvent> Events;
		};

		struct RunnerObject {
			std::shared_ptr<GraphObject> Source;
			ang::Domain Runs = ang::Domain::Tool;
			std::unique_ptr<ang::Runner> Run;
			uint64_t CompiledRevision = 0;
			std::map<std::string, CallbackRef> Hooks;
			std::shared_ptr<Callbacks> Retained = std::make_shared<Callbacks>();
			std::shared_ptr<HookQueue> Queue = std::make_shared<HookQueue>();
			bool Running = false;
		};

		struct ViewObject {
			std::shared_ptr<GraphObject> Source;
			std::unique_ptr<angview::View> Interaction;
			// What the last render wrote, so canvas edits can be told from graph edits.
			ecs::Entity Canvas;
			std::unordered_map<std::string, core::Vector2> RenderedPositions;
			std::vector<ang::Link> RenderedLinks;
		};

		using Object = std::variant<
			std::shared_ptr<RegistryObject>,
			std::shared_ptr<GraphObject>,
			std::shared_ptr<RunnerObject>,
			std::shared_ptr<ViewObject>>;

		// A host value a script holds a handle to. The runner owns the value.
		struct BlobHandle {
			std::string Type;
			std::weak_ptr<const void> Data;
			size_t Bytes = 0;
			uint64_t Identity = 0;
		};

		constexpr size_t MAXIMUM_BLOB_HANDLES = 4096;
		constexpr std::string_view BLOB_FIELD = "NodeGraphBlob";
	}

	struct NodeGraphLibrary::State {
		std::function<void(CallbackRef)> Release;
		NodeGraphLimits Bounds;
		std::unordered_map<uint32_t, node_graph_detail::Object> Objects;
		uint32_t NextHandle = 0;
		// The call running a graph, so script Evaluate functions can reach their VM.
		ScriptCall *ActiveCall = nullptr;
		std::shared_ptr<const NodeGraphHost> Host;
		std::map<uint64_t, node_graph_detail::BlobHandle> Blobs;
		uint64_t NextBlob = 0;
	};

	NodeGraphLibrary::NodeGraphLibrary(
		std::function<void(CallbackRef)> release,
		std::shared_ptr<const NodeGraphHost> host,
		NodeGraphLimits limits
	)
		: Owned(std::make_unique<State>()) {
		Owned->Release = std::move(release);
		Owned->Host = std::move(host);
		Owned->Bounds = limits;
	}

	NodeGraphLibrary::~NodeGraphLibrary() = default;

	const NodeGraphLimits &NodeGraphLibrary::Limits() const {
		return Owned->Bounds;
	}

	size_t NodeGraphLibrary::ObjectCount() const {
		return Owned->Objects.size();
	}

	NodeGraphLibrary::State &NodeGraphLibrary::Data() {
		return *Owned;
	}

	namespace node_graph_detail {
		NodeGraphLibrary::State &StateOf(ScriptCall &call) {
			return call.NodeGraphs().Data();
		}

		uint32_t Store(ScriptCall &call, Object object) {
			NodeGraphLibrary::State &state = StateOf(call);
			if (state.Objects.size() >= state.Bounds.MaximumObjects) {
				call.Raise(
					"NodeGraph: too many registries, graphs, runners and views in this script context"
				);
			}
			const uint32_t handle = ++state.NextHandle;
			state.Objects.emplace(handle, std::move(object));
			return handle;
		}

		template <typename T> std::shared_ptr<T> Find(ScriptCall &call, size_t index, const char *what) {
			const double number = call.AsNumber(index);
			NodeGraphLibrary::State &state = StateOf(call);
			const auto found = state.Objects.find(static_cast<uint32_t>(number));
			if (found == state.Objects.end() || !std::holds_alternative<std::shared_ptr<T>>(found->second)) {
				call.Raise((std::string("NodeGraph: not a live ") + what).c_str());
			}
			return std::get<std::shared_ptr<T>>(found->second);
		}

		ScriptValue Read(ScriptCall &call, size_t index) {
			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;
			if (call.IsNil(index)) {
				return value;
			}
			if (!call.ReadValue(index, value, why)) {
				call.Raise("NodeGraph: value has no script form");
			}
			return value;
		}

		// The script form of a value; a host blob becomes a handle of this VM.
		ScriptValue ToScript(NodeGraphLibrary::State &state, const ang::Value &value) {
			const ang::Blob *blob = value.Get<ang::Blob>();
			if (blob == nullptr || value.TypeName() == "table" || blob->Data == nullptr) {
				return ToScriptValue(value);
			}
			uint64_t handle = 0;
			for (const auto &[id, held] : state.Blobs) {
				if (held.Data.lock() == blob->Data && held.Identity == blob->Identity &&
					held.Type == value.TypeName()) {
					handle = id;
				}
			}
			if (handle == 0) {
				if (state.Blobs.size() >= MAXIMUM_BLOB_HANDLES) {
					std::erase_if(state.Blobs, [](const auto &entry) { return entry.second.Data.expired(); });
				}
				if (state.Blobs.size() >= MAXIMUM_BLOB_HANDLES) {
					state.Blobs.erase(state.Blobs.begin());
				}
				handle = ++state.NextBlob;
				state.Blobs[handle] = {value.TypeName(), blob->Data, blob->Bytes, blob->Identity};
			}
			return MapOf({
				{"Bytes", Number(static_cast<double>(blob->Bytes))},
				{std::string(BLOB_FIELD), Number(static_cast<double>(handle))},
				{"Type", Text(value.TypeName())},
			});
		}

		// The value a script value stands for. False when it is a handle to a
		// host value nothing holds any more.
		bool FromScript(
			NodeGraphLibrary::State &state, const ScriptValue &value, std::string_view type, ang::Value &out
		) {
			const ScriptValue *field = value.Tag == ValueTag::Map ? Field(value, BLOB_FIELD) : nullptr;
			if (field == nullptr || field->Tag != ValueTag::Number) {
				out = ToGraphValue(value, type);
				return true;
			}
			const auto found = field->Number >= 1.0 && field->Number <= 9007199254740992.0
								   ? state.Blobs.find(static_cast<uint64_t>(field->Number))
								   : state.Blobs.end();
			std::shared_ptr<const void> data =
				found != state.Blobs.end() ? found->second.Data.lock() : nullptr;
			if (data == nullptr) {
				return false;
			}
			const BlobHandle &held = found->second;
			out = ang::Value::FromBlob(
				held.Type, {.Data = std::move(data), .Bytes = held.Bytes, .Identity = held.Identity}
			);
			return true;
		}

		ang::Value FromScript(ScriptCall &call, const ScriptValue &value, std::string_view type) {
			ang::Value out;
			if (!FromScript(StateOf(call), value, type, out)) {
				call.Raise("NodeGraph: the host value behind this handle is gone");
			}
			return out;
		}

		ang::PortDef ReadPort(ScriptCall &call, const ScriptValue &spec, ang::Direction direction) {
			if (!IsRecord(spec)) {
				call.Raise("NodeGraph: a port is a table with Name and Type");
			}
			ang::PortDef port;
			port.Name = FieldText(call, spec, "Name");
			port.Type = FieldText(call, spec, "Type", std::string(ang::ANY_TYPE));
			port.Dir = direction;
			port.Optional = FieldBoolean(spec, "Optional", false);
			port.MaximumLinks =
				static_cast<uint16_t>(std::clamp(FieldNumber(spec, "MaximumLinks", 1.0), 0.0, 65535.0));
			port.Generic = FieldText(call, spec, "Generic");
			for (const ScriptValue &constraint : FieldList(call, spec, "Constraints")) {
				if (constraint.Tag == ValueTag::String) {
					port.Constraints.push_back(constraint.Text);
				}
			}
			if (const ScriptValue *fallback = Field(spec, "Default")) {
				port.Default = ToGraphValue(*fallback, port.Type);
			}
			return port;
		}

		ScriptValue NodeInfo(const ang::NodeInfo &info) {
			return MapOf({
				{"Def", Text(info.Def)},
				{"Node", Text(info.Id)},
				{"RunNumber", Number(static_cast<double>(info.RunNumber))},
				{"Wave", Number(info.Wave)},
			});
		}

		ScriptValue Report(const ang::Plan &plan, const ang::RunReport &report) {
			ScriptValue nodes(ValueTag::Map);
			for (size_t index = 0; index < plan.Nodes.size() && index < report.Nodes.size(); ++index) {
				const ang::NodeReport &entry = report.Nodes[index];
				nodes.Entries.emplace_back(
					plan.Nodes[index].Id,
					MapOf({
						{"Bytes", Number(static_cast<double>(entry.Bytes))},
						{"Milliseconds", Number(entry.Milliseconds)},
						{"Note", Text(entry.Note)},
						{"Status", Text(ang::Describe(entry.Status))},
					})
				);
			}
			return MapOf({
				{"Budget", Number(static_cast<double>(report.Budget))},
				{"Bypassed", Number(static_cast<double>(report.Bypassed))},
				{"Cached", Number(static_cast<double>(report.Cached))},
				{"Complete", Boolean(report.Complete)},
				{"Failed", Number(static_cast<double>(report.Failed))},
				{"Milliseconds", Number(report.Milliseconds)},
				{"Nodes", std::move(nodes)},
				{"PeakBytes", Number(static_cast<double>(report.PeakBytes))},
				{"Ran", Number(static_cast<double>(report.Ran))},
				{"RetainedBytes", Number(static_cast<double>(report.RetainedBytes))},
				{"RunNumber", Number(static_cast<double>(report.RunNumber))},
				{"Skipped", Number(static_cast<double>(report.Skipped))},
				{"Waiting", Number(static_cast<double>(report.Waiting))},
			});
		}

		std::string Diagnostics(const ang::CompileResult &result) {
			std::string text = "NodeGraph: graph refused:";
			for (const ang::Diagnostic &diagnostic : result.Diagnostics) {
				text += " [" + diagnostic.Node + (diagnostic.Port.empty() ? "" : "." + diagnostic.Port) +
						"] " + diagnostic.Message + ";";
			}
			return text;
		}

		ang::Plan Compile(ScriptCall &call, const GraphObject &graph, ang::Domain domain) {
			const NodeGraphLimits &limits = StateOf(call).Bounds;
			ang::Plan plan;
			const ang::CompileResult result = ang::Compile(
				graph.Document,
				graph.Types->Types,
				{.Runs = domain, .MaximumNodes = limits.MaximumNodes, .MaximumLinks = limits.MaximumLinks},
				plan
			);
			if (!result.Ok) {
				call.Raise(Diagnostics(result).c_str());
			}
			return plan;
		}

		void Edit(ScriptCall &call, GraphObject &graph, const ang::Edit &change) {
			if (graph.ReadOnly) {
				call.Raise("NodeGraph: this graph is read-only");
			}
			const ang::EditResult result = graph.Document.Apply(change);
			if (!result.Ok) {
				call.Raise(("NodeGraph: " + result.Error).c_str());
			}
		}

		// Whether a removal applied; a read-only graph raises.
		bool Remove(ScriptCall &call, GraphObject &graph, const ang::Edit &change) {
			if (graph.ReadOnly) {
				call.Raise("NodeGraph: this graph is read-only");
			}
			return graph.Document.Apply(change).Ok;
		}

		std::shared_ptr<RegistryObject> FindOwnRegistry(ScriptCall &call) {
			auto registry = Find<RegistryObject>(call, 0, "registry");
			if (registry->Locked) {
				call.Raise("NodeGraph: the engine's node types cannot be changed");
			}
			return registry;
		}

		// --- registry ---------------------------------------------------------------------------

		void CreateRegistry(ScriptCall &call) {
			auto object = std::make_shared<RegistryObject>();
			object->Retained->Release = StateOf(call).Release;
			object->Types.DefineType({.Id = "table", .Label = "Table"});
			call.ReturnNumber(Store(call, object));
		}

		void DefineType(ScriptCall &call) {
			auto registry = FindOwnRegistry(call);
			const std::string id = call.AsBoundedString(1, 128);
			const ScriptValue spec = Read(call, 2);
			ang::DataType type{
				.Id = id, .Label = IsRecord(spec) ? FieldText(call, spec, "Label") : std::string()
			};
			if (const ScriptValue *wire = IsRecord(spec) ? Field(spec, "Wire") : nullptr;
				wire && wire->Tag == ValueTag::Color3) {
				type.Wire = wire->Colour;
			}
			const ang::RegistryResult result = registry->Types.DefineType(std::move(type));
			if (!result.Ok) {
				call.Raise(("NodeGraph: " + result.Error).c_str());
			}
		}

		void DefineNode(ScriptCall &call) {
			auto registry = FindOwnRegistry(call);
			const ScriptValue spec = Read(call, 1);
			if (!IsRecord(spec)) {
				call.Raise("NodeGraph: DefineNode takes a table");
			}
			ang::NodeDef def;
			def.Id = FieldText(call, spec, "Id");
			def.Version = static_cast<uint32_t>(std::max(1.0, FieldNumber(spec, "Version", 1.0)));
			def.Category = FieldText(call, spec, "Category");
			def.Label = FieldText(call, spec, "Label");
			def.Pure = FieldBoolean(spec, "Pure", true) ? ang::Purity::Pure : ang::Purity::Impure;
			for (const ScriptValue &port : FieldList(call, spec, "Inputs")) {
				def.Inputs.push_back(ReadPort(call, port, ang::Direction::Input));
			}
			for (const ScriptValue &port : FieldList(call, spec, "Outputs")) {
				def.Outputs.push_back(ReadPort(call, port, ang::Direction::Output));
			}
			for (const ScriptValue &param : FieldList(call, spec, "Params")) {
				if (!IsRecord(param)) {
					call.Raise("NodeGraph: a param is a table with Name and Type");
				}
				ang::ParamDef entry{
					.Name = FieldText(call, param, "Name"),
					.Type = FieldText(call, param, "Type", std::string(ang::ANY_TYPE)),
					.Label = FieldText(call, param, "Label"),
					.Widget = FieldText(call, param, "Widget"),
					.Minimum = FieldNumber(param, "Minimum", 0.0),
					.Maximum = FieldNumber(param, "Maximum", 0.0),
				};
				if (const ScriptValue *fallback = Field(param, "Default")) {
					entry.Default = ToGraphValue(*fallback, entry.Type);
				}
				def.Params.push_back(std::move(entry));
			}
			for (const ScriptValue &route : FieldList(call, spec, "Bypass")) {
				if (IsRecord(route)) {
					def.Bypass.push_back({FieldText(call, route, "Input"), FieldText(call, route, "Output")});
				}
			}
			if (!call.IsNil(2)) {
				const CallbackRef evaluate = call.RetainCallback(2);
				registry->Retained->Held.push_back(evaluate);
				NodeGraphLibrary::State *state = &StateOf(call);
				const bool readOnly = def.Pure == ang::Purity::Pure;
				std::vector<std::string> outputs;
				std::vector<std::string> outputTypes;
				for (const ang::PortDef &port : def.Outputs) {
					outputs.push_back(port.Name);
					outputTypes.push_back(port.Type);
				}
				std::vector<std::string> inputs;
				for (const ang::PortDef &port : def.Inputs) {
					inputs.push_back(port.Name);
				}
				def.Evaluate = [state, evaluate, readOnly, inputs, outputs, outputTypes](
								   ang::EvalContext &context
							   ) {
					if (state->ActiveCall == nullptr) {
						return ang::NodeResult::Failure("script nodes run only inside Runner:Run");
					}
					ScriptValue given(ValueTag::Map);
					for (const std::string &input : inputs) {
						given.Entries.emplace_back(input, ToScript(*state, context.Input(input)));
					}
					ScriptValue info = MapOf({{"Node", Text(context.NodeId())}});
					const std::array arguments{std::move(info), std::move(given)};
					ScriptValue result;
					std::string error;
					if (!state->ActiveCall->InvokeCallback(evaluate, arguments, result, error, readOnly)) {
						return ang::NodeResult::Failure(error);
					}
					if (result.Tag == ValueTag::Nil) {
						return ang::NodeResult::Success();
					}
					if (!IsRecord(result)) {
						return ang::NodeResult::Failure("Evaluate must return a table of outputs");
					}
					for (size_t index = 0; index < outputs.size(); ++index) {
						if (const ScriptValue *value = Field(result, outputs[index])) {
							ang::Value output;
							if (!FromScript(*state, *value, outputTypes[index], output)) {
								return ang::NodeResult::Failure("the host value behind a handle is gone");
							}
							context.SetOutput(outputs[index], std::move(output));
						}
					}
					return ang::NodeResult::Success();
				};
			}
			const ang::RegistryResult result = registry->Types.DefineNode(std::move(def));
			if (!result.Ok) {
				call.Raise(("NodeGraph: " + result.Error).c_str());
			}
		}

		// --- graph ------------------------------------------------------------------------------

		void CreateGraph(ScriptCall &call) {
			auto registry = Find<RegistryObject>(call, 0, "registry");
			call.ReturnNumber(Store(call, std::make_shared<GraphObject>(GraphObject{.Types = registry})));
		}

		void AddNode(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			const std::string def = call.AsBoundedString(1, 128);
			const ScriptValue params = Read(call, 2);
			if (graph->Document.Nodes().size() >= StateOf(call).Bounds.MaximumNodes) {
				call.Raise("NodeGraph: graph node limit reached");
			}
			const auto definition = graph->Types->Types.FindNode(def);
			ang::NodeInstance node{
				.Id = call.IsNil(5) ? graph->Document.NewNodeId(def) : call.AsBoundedString(5, 128),
				.Def = def,
				.Version = definition != nullptr ? definition->Version : 0,
				.Position = {
					call.IsNil(3) ? 0.0f : static_cast<float>(call.AsNumber(3)),
					call.IsNil(4) ? 0.0f : static_cast<float>(call.AsNumber(4))
				},
			};
			if (IsRecord(params)) {
				for (const auto &[name, value] : params.Entries) {
					const ang::ParamDef *declared =
						definition != nullptr ? definition->FindParam(name) : nullptr;
					node.Params[name] =
						FromScript(call, value, declared != nullptr ? declared->Type : std::string_view{});
				}
			}
			const std::string id = node.Id;
			Edit(call, *graph, ang::edit::AddNode{std::move(node)});
			call.ReturnString(id);
		}

		void RemoveNode(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			call.ReturnBoolean(Remove(call, *graph, ang::edit::RemoveNode{call.AsBoundedString(1, 128)}));
		}

		ang::Link ReadLink(ScriptCall &call) {
			return {
				call.AsBoundedString(1, 128),
				call.AsBoundedString(2, 128),
				call.AsBoundedString(3, 128),
				call.AsBoundedString(4, 128)
			};
		}

		void Link(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			const ang::Link wire = ReadLink(call);
			if (graph->Document.Links().size() >= StateOf(call).Bounds.MaximumLinks) {
				call.Raise("NodeGraph: graph link limit reached");
			}
			const ang::LinkVerdict verdict = ang::CheckNewLink(graph->Document, graph->Types->Types, wire);
			if (!verdict.Ok) {
				call.Raise(("NodeGraph: link refused: " + verdict.Reason).c_str());
			}
			Edit(call, *graph, ang::edit::AddLink{wire});
		}

		void Unlink(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			call.ReturnBoolean(Remove(call, *graph, ang::edit::RemoveLink{ReadLink(call)}));
		}

		void SetParam(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			const std::string node = call.AsBoundedString(1, 128);
			const std::string name = call.AsBoundedString(2, 128);
			const ang::NodeInstance *instance = graph->Document.FindNode(node);
			const auto definition =
				instance != nullptr ? graph->Types->Types.FindNode(instance->Def) : nullptr;
			const ang::ParamDef *declared = definition != nullptr ? definition->FindParam(name) : nullptr;
			Edit(
				call,
				*graph,
				ang::edit::SetParam{
					node,
					name,
					FromScript(call, Read(call, 3), declared != nullptr ? declared->Type : std::string_view{})
				}
			);
		}

		void MoveNode(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			Edit(
				call,
				*graph,
				ang::edit::MoveNode{
					call.AsBoundedString(1, 128),
					{static_cast<float>(call.AsNumber(2)), static_cast<float>(call.AsNumber(3))}
				}
			);
		}

		void SetBypass(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			Edit(
				call,
				*graph,
				ang::edit::SetBypass{call.AsBoundedString(1, 128), call.OptionalBoolean(2, true)}
			);
		}

		void SetCollapsed(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			Edit(
				call,
				*graph,
				ang::edit::SetCollapsed{call.AsBoundedString(1, 128), call.OptionalBoolean(2, true)}
			);
		}

		void GetNodes(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			ScriptValue nodes(ValueTag::Array);
			for (const ang::NodeInstance &node : graph->Document.Nodes()) {
				ScriptValue params(ValueTag::Map);
				for (const auto &[name, value] : node.Params) {
					params.Entries.emplace_back(name, ToScript(StateOf(call), value));
				}
				nodes.Items.push_back(MapOf({
					{"Bypassed", Boolean(node.Bypassed)},
					{"Collapsed", Boolean(node.Collapsed)},
					{"Def", Text(node.Def)},
					{"Id", Text(node.Id)},
					{"Params", std::move(params)},
					{"X", Number(node.Position.X)},
					{"Y", Number(node.Position.Y)},
				}));
			}
			call.ReturnValue(nodes);
		}

		void GetLinks(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			ScriptValue links(ValueTag::Array);
			for (const ang::Link &wire : graph->Document.Links()) {
				links.Items.push_back(MapOf({
					{"From", Text(wire.FromNode)},
					{"FromPort", Text(wire.FromPort)},
					{"To", Text(wire.ToNode)},
					{"ToPort", Text(wire.ToPort)},
				}));
			}
			call.ReturnValue(links);
		}

		// --- documents ----------------------------------------------------------------------------

		void Save(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			std::string text, error;
			if (!ang::WriteGraph(graph->Document, text, error)) {
				call.Raise(("NodeGraph: " + error).c_str());
			}
			call.ReturnString(text);
		}

		void Load(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			if (graph->ReadOnly) {
				call.Raise("NodeGraph: this graph is read-only");
			}
			const NodeGraphLimits &limits = StateOf(call).Bounds;
			const std::string text = call.AsString(1);
			std::string error;
			if (!ang::ReadGraph(
					text,
					graph->Document,
					error,
					{.MaximumBytes = limits.MaximumValueBytes,
					 .MaximumNodes = limits.MaximumNodes,
					 .MaximumLinks = limits.MaximumLinks}
				)) {
				call.Raise(("NodeGraph: " + error).c_str());
			}
		}

		// Hands an opened engine graph to the prelude as its two handles.
		void ReturnOpened(ScriptCall &call, ang::Registry types, GraphObject graph) {
			auto registry = std::make_shared<RegistryObject>();
			registry->Types = std::move(types);
			registry->Locked = true;
			graph.Types = registry;
			const uint32_t registryHandle = Store(call, registry);
			const uint32_t graphHandle = Store(call, std::make_shared<GraphObject>(std::move(graph)));
			call.ReturnValue(MapOf({{"Graph", Number(graphHandle)}, {"Registry", Number(registryHandle)}}));
		}

		void OpenImageGraph(ScriptCall &call) {
			const std::string encoded = call.AsString(0);
			imagegraph::Document document;
			imagegraph::Diagnostic diagnostic;
			GraphObject graph;
			if (encoded.empty()) {
				call.Raise("NodeGraph: the image graph has no runtime document");
			}
			if (!imagegraph::Read(encoded, document, diagnostic) ||
				!imagegraph::ToAngGraph(document, graph.Document, diagnostic)) {
				call.Raise(
					("NodeGraph: image graph refused: " + diagnostic.Node + " " + diagnostic.Message).c_str()
				);
			}
			graph.Image = std::move(document);
			ReturnOpened(call, imagegraph::GraphTypes(), std::move(graph));
		}

		// The image graph document the edited graph describes. Bindings to
		// removed nodes go with them; an output of one is refused.
		void WriteImageGraph(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			if (!graph->Image) {
				call.Raise("NodeGraph: only a graph opened from an ImageGraph can be committed");
			}
			imagegraph::Document document;
			imagegraph::Diagnostic diagnostic;
			std::string encoded;
			if (!imagegraph::FromAngGraph(graph->Document, graph->Image->Outputs, document, diagnostic)) {
				call.Raise(
					("NodeGraph: image graph refused: " + diagnostic.Node + " " + diagnostic.Message).c_str()
				);
			}
			document.Parameters = graph->Image->Parameters;
			document.Storage = graph->Image->Storage;
			for (const imagegraph::Binding &binding : graph->Image->Bindings) {
				if (graph->Document.FindNode(binding.Node) != nullptr) {
					document.Bindings.push_back(binding);
				}
			}
			if (!imagegraph::Write(document, encoded, diagnostic)) {
				call.Raise(
					("NodeGraph: image graph refused: " + diagnostic.Node + " " + diagnostic.Message).c_str()
				);
			}
			call.ReturnString(encoded);
		}

		void OpenProcedural(ScriptCall &call) {
			const ecs::Store &store = call.World();
			const auto *authority = ecs::Components::Assigned<ProceduralAuthority>().IsValid()
										? store.Resource<ProceduralAuthority>()
										: nullptr;
			procedural::Recipe recipe;
			procedural::Diagnostic diagnostic;
			GraphObject graph{.ReadOnly = true};
			if (authority == nullptr || authority->RecipeBytes.empty() ||
				procedural::DecodeRecipe(authority->RecipeBytes, recipe, diagnostic) !=
					procedural::Status::Ok ||
				!procedural::ToAngGraph(recipe, graph.Document)) {
				call.Raise("NodeGraph: this world has no procedural recipe");
			}
			ReturnOpened(call, procedural::RecipeGraphTypes(), std::move(graph));
		}

		void HostGraphNames(ScriptCall &call) {
			const auto &host = StateOf(call).Host;
			ScriptValue names(ValueTag::Array);
			if (host != nullptr && host->Names) {
				for (const std::string &name : host->Names()) {
					names.Items.push_back(Text(name));
				}
			}
			call.ReturnValue(names);
		}

		void OpenHostGraph(ScriptCall &call) {
			const auto &host = StateOf(call).Host;
			const std::string name = call.AsBoundedString(0, 256);
			if (host == nullptr || !host->Open) {
				call.Raise("NodeGraph: this host shows no graphs");
			}
			ang::Registry types;
			GraphObject graph{.ReadOnly = true};
			std::string error;
			if (!host->Open(name, types, graph.Document, error)) {
				call.Raise(("NodeGraph: " + error).c_str());
			}
			ReturnOpened(call, std::move(types), std::move(graph));
		}

		ang::Domain ReadDomain(ScriptCall &call, size_t index) {
			if (call.IsNil(index)) {
				return ang::Domain::Tool;
			}
			const auto domain = ang::DomainFromName(call.AsBoundedString(index, 16));
			if (!domain) {
				call.Raise("NodeGraph: domain is Tick, Frame or Tool");
			}
			return *domain;
		}

		void Validate(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			ang::Plan plan;
			const ang::CompileResult result =
				ang::Compile(graph->Document, graph->Types->Types, {.Runs = ReadDomain(call, 1)}, plan);
			ScriptValue found(ValueTag::Array);
			for (const ang::Diagnostic &diagnostic : result.Diagnostics) {
				found.Items.push_back(MapOf({
					{"Code", Text(ang::Describe(diagnostic.Code))},
					{"Message", Text(diagnostic.Message)},
					{"Node", Text(diagnostic.Node)},
					{"Port", Text(diagnostic.Port)},
				}));
			}
			call.ReturnValue(found);
		}

		// --- runner -----------------------------------------------------------------------------

		void CreateRunner(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			auto runner = std::make_shared<RunnerObject>();
			runner->Source = graph;
			runner->Runs = ReadDomain(call, 1);
			runner->Retained->Release = StateOf(call).Release;
			runner->Run = std::make_unique<ang::Runner>(
				Compile(call, *graph, runner->Runs),
				ang::RunnerOptions{
					.MaximumMilliseconds = StateOf(call).Bounds.MaximumRunMilliseconds,
					.MaximumProducedBytes = StateOf(call).Bounds.MaximumValueBytes,
				}
			);
			runner->CompiledRevision = graph->Document.Revision();
			call.ReturnNumber(Store(call, runner));
		}

		void SetHook(ScriptCall &call) {
			auto runner = Find<RunnerObject>(call, 0, "runner");
			const std::string name = call.AsBoundedString(1, 32);
			static constexpr std::array<std::string_view, 6> NAMES{
				"PreRun", "PostRun", "PreNode", "PostNode", "PreCompute", "PostCompute"
			};
			if (std::find(NAMES.begin(), NAMES.end(), name) == NAMES.end()) {
				call.Raise(
					"NodeGraph: hooks are PreRun, PostRun, PreNode, PostNode, PreCompute and PostCompute"
				);
			}
			if (auto found = runner->Hooks.find(name); found != runner->Hooks.end()) {
				std::erase(runner->Retained->Held, found->second);
				call.ReleaseCallback(found->second);
				runner->Hooks.erase(found);
			}
			if (!call.IsNil(2)) {
				const CallbackRef hook = call.RetainCallback(2);
				runner->Retained->Held.push_back(hook);
				runner->Hooks.emplace(name, hook);
			}
		}

		// Installs ang hooks that queue events for the hooks a script set. Each
		// names its hook rather than the function, so one a script has replaced
		// since is never called.
		void ArmHooks(RunnerObject &runner) {
			ang::Hooks &hooks = runner.Run->Callbacks();
			hooks = {};
			const auto queue = [queue = runner.Queue](const char *name, ScriptValue info) {
				const std::lock_guard lock(queue->Lock);
				queue->Events.push_back({name, std::move(info)});
			};
			const auto hook = [&](const char *name) { return runner.Hooks.contains(name); };
			if (hook("PreRun")) {
				hooks.PreRun = [queue](const ang::RunInfo &info) {
					queue("PreRun", MapOf({{"RunNumber", Number(static_cast<double>(info.RunNumber))}}));
				};
			}
			if (hook("PostRun")) {
				hooks.PostRun = [queue](const ang::RunInfo &info) {
					queue(
						"PostRun",
						MapOf(
							{{"Milliseconds", Number(info.Report->Milliseconds)},
							 {"RunNumber", Number(static_cast<double>(info.RunNumber))}}
						)
					);
				};
			}
			if (hook("PreNode")) {
				hooks.PreNode = [queue](const ang::NodeInfo &info) { queue("PreNode", NodeInfo(info)); };
			}
			if (hook("PostNode")) {
				hooks.PostNode = [queue](const ang::NodeInfo &info, ang::NodeStatus status) {
					ScriptValue entry = NodeInfo(info);
					entry.Entries.emplace_back("Status", Text(ang::Describe(status)));
					queue("PostNode", std::move(entry));
				};
			}
			if (hook("PreCompute")) {
				hooks.PreCompute = [queue](const ang::NodeInfo &info) {
					queue("PreCompute", NodeInfo(info));
				};
			}
			if (hook("PostCompute")) {
				hooks.PostCompute = [queue](const ang::NodeInfo &info, ang::ComputeResult &result) {
					ScriptValue entry = NodeInfo(info);
					entry.Entries.emplace_back("Status", Text(ang::Describe(result.Status)));
					queue("PostCompute", std::move(entry));
				};
			}
		}

		// Restores the enclosing call when a run ends, a raise included.
		struct ActiveScope {
			NodeGraphLibrary::State &State;
			ScriptCall *Previous;
			RunnerObject &Runner;
			ActiveScope(NodeGraphLibrary::State &state, ScriptCall &call, RunnerObject &runner)
				: State(state), Previous(state.ActiveCall), Runner(runner) {
				State.ActiveCall = &call;
				Runner.Running = true;
			}
			~ActiveScope() {
				State.ActiveCall = Previous;
				Runner.Running = false;
			}
		};

		void Run(ScriptCall &call) {
			auto runner = Find<RunnerObject>(call, 0, "runner");
			NodeGraphLibrary::State &state = StateOf(call);
			if (runner->Running) {
				call.Raise("NodeGraph: a runner cannot run itself from inside its own run");
			}
			if (runner->CompiledRevision != runner->Source->Document.Revision()) {
				runner->Run->SetPlan(Compile(call, *runner->Source, runner->Runs));
				runner->CompiledRevision = runner->Source->Document.Revision();
			}
			ang::RunInputs inputs;
			const ScriptValue given = Read(call, 1);
			if (IsRecord(given)) {
				const ang::Plan &plan = runner->Run->CurrentPlan();
				for (const auto &[node, ports] : given.Entries) {
					const int64_t index = plan.Find(node);
					if (index < 0 || !IsRecord(ports)) {
						continue;
					}
					for (const auto &[port, value] : ports.Entries) {
						std::string_view type;
						for (const ang::PlannedInput &input : plan.Nodes[static_cast<size_t>(index)].Inputs) {
							if (input.Port == port) {
								type = input.Type;
							}
						}
						inputs[node][port] = FromScript(call, value, type);
					}
				}
			}
			ArmHooks(*runner);
			ang::RunReport report;
			{
				const ActiveScope scope(state, call, *runner);
				report = runner->Run->Run(inputs);
			}
			// Deliver the hook events on the script thread, in firing order.
			std::vector<HookEvent> events;
			{
				const std::lock_guard lock(runner->Queue->Lock);
				events.swap(runner->Queue->Events);
			}
			for (const HookEvent &event : events) {
				const auto hook = runner->Hooks.find(event.Name);
				if (hook == runner->Hooks.end()) {
					continue;
				}
				ScriptValue ignored;
				std::string error;
				const std::array arguments{event.Info};
				if (!call.InvokeCallback(hook->second, arguments, ignored, error, false)) {
					call.Raise(("NodeGraph: hook failed: " + error).c_str());
				}
			}
			if (!report.LimitReached.empty()) {
				call.Raise(
					("NodeGraph: the run was stopped at its " + report.LimitReached + " limit").c_str()
				);
			}
			call.ReturnValue(Report(runner->Run->CurrentPlan(), report));
		}

		void Output(ScriptCall &call) {
			auto runner = Find<RunnerObject>(call, 0, "runner");
			const ang::Value *value =
				runner->Run->Output(call.AsBoundedString(1, 128), call.AsBoundedString(2, 128));
			if (value == nullptr) {
				call.ReturnNil();
				return;
			}
			call.ReturnValue(ToScript(StateOf(call), *value));
		}

		// --- view -------------------------------------------------------------------------------

		angview::Theme ReadTheme(ScriptCall &call, const ScriptValue &options, angview::Theme theme) {
			if (!IsRecord(options)) {
				return theme;
			}
			const std::string preset = FieldText(call, options, "Theme");
			if (!preset.empty()) {
				const auto found = angview::Theme::Preset(preset);
				if (!found) {
					call.Raise("NodeGraph: themes are Dark, HighContrast, Blueprint and Minimal");
				}
				theme = *found;
			}
			const auto colour = [&](std::string_view key, angview::Rgba &out) {
				if (const ScriptValue *value = Field(options, key); value && value->Tag == ValueTag::Color3) {
					out = angview::FromColor3(value->Colour, out.A);
				}
			};
			colour("Background", theme.Background);
			colour("NodeBody", theme.NodeBody);
			colour("NodeHeader", theme.NodeHeader);
			colour("NodeBorder", theme.NodeBorder);
			colour("Text", theme.Text);
			theme.ShowGrid = FieldBoolean(options, "ShowGrid", theme.ShowGrid);
			theme.ShowTimings = FieldBoolean(options, "ShowTimings", theme.ShowTimings);
			theme.NodeWidth = static_cast<float>(FieldNumber(options, "NodeWidth", theme.NodeWidth));
			const std::string wires = FieldText(call, options, "Wires");
			if (wires == "Bezier") {
				theme.Wires = angview::WireStyle::Bezier;
			} else if (wires == "Straight") {
				theme.Wires = angview::WireStyle::Straight;
			} else if (wires == "Orthogonal") {
				theme.Wires = angview::WireStyle::Orthogonal;
			}
			return theme;
		}

		void CreateView(ScriptCall &call) {
			auto graph = Find<GraphObject>(call, 0, "graph");
			auto view = std::make_shared<ViewObject>();
			view->Source = graph;
			view->Interaction = std::make_unique<angview::View>(
				graph->Document, graph->Types->Types, ReadTheme(call, Read(call, 1), angview::Theme::Dark())
			);
			call.ReturnNumber(Store(call, view));
		}

		void SetViewTheme(ScriptCall &call) {
			auto view = Find<ViewObject>(call, 0, "view");
			view->Interaction->SetTheme(ReadTheme(call, Read(call, 1), view->Interaction->CurrentTheme()));
		}

		void SetViewRun(ScriptCall &call) {
			auto view = Find<ViewObject>(call, 0, "view");
			auto runner = Find<RunnerObject>(call, 1, "runner");
			const ang::RunTimeline &timeline = runner->Run->Timeline();
			view->Interaction->SetTimeline(timeline);
			// Rebuild the per-node statuses from the latest timeline samples.
			ang::RunReport report;
			report.Nodes.resize(runner->Run->CurrentPlan().Nodes.size());
			if (const ang::TimelineRun *latest = timeline.Latest()) {
				for (const ang::TimelineSample &sample : latest->Samples) {
					if (sample.Node < report.Nodes.size()) {
						report.Nodes[sample.Node].Status = sample.Outcome;
						report.Nodes[sample.Node].Milliseconds =
							static_cast<double>(sample.EndNanoseconds - sample.BeginNanoseconds) / 1.0e6;
					}
				}
			}
			view->Interaction->SetRunReport(runner->Run->CurrentPlan(), report);
		}

		const char *KindName(angview::DrawKind kind) {
			switch (kind) {
			case angview::DrawKind::Rect:
				return "Rect";
			case angview::DrawKind::Line:
				return "Line";
			case angview::DrawKind::Bezier:
				return "Bezier";
			case angview::DrawKind::Circle:
				return "Circle";
			case angview::DrawKind::Diamond:
				return "Diamond";
			case angview::DrawKind::Text:
				return "Text";
			case angview::DrawKind::Image:
				return "Image";
			case angview::DrawKind::PushClip:
				return "PushClip";
			case angview::DrawKind::PopClip:
				return "PopClip";
			}
			return "Rect";
		}

		void GetDrawCommands(ScriptCall &call) {
			auto view = Find<ViewObject>(call, 0, "view");
			view->Interaction->SetViewport(
				{static_cast<float>(call.AsNumber(1)), static_cast<float>(call.AsNumber(2))}
			);
			ScriptValue commands(ValueTag::Array);
			for (const angview::DrawCommand &command : view->Interaction->Paint()) {
				commands.Items.push_back(MapOf({
					{"Ax", Number(command.A.X)},
					{"Ay", Number(command.A.Y)},
					{"Bx", Number(command.B.X)},
					{"By", Number(command.B.Y)},
					{"Cx", Number(command.C.X)},
					{"Cy", Number(command.C.Y)},
					{"Dx", Number(command.D.X)},
					{"Dy", Number(command.D.Y)},
					{"Fill", Colour({command.Fill.R, command.Fill.G, command.Fill.B})},
					{"FillAlpha", Number(command.Fill.A)},
					{"Kind", Text(KindName(command.Kind))},
					{"Radius", Number(command.Radius)},
					{"Stroke", Colour({command.Stroke.R, command.Stroke.G, command.Stroke.B})},
					{"StrokeAlpha", Number(command.Stroke.A)},
					{"Text", Text(command.Text)},
					{"TextSize", Number(command.TextSize)},
					{"Thickness", Number(command.Thickness)},
				}));
			}
			call.ReturnValue(commands);
		}

		// --- view on a NodeCanvas -----------------------------------------------------------------

		// A new child of the canvas. One under a client's own canvas belongs to
		// that client, so it never takes a row the authority may assign.
		ecs::Entity
		MakeChild(ecs::Store &store, ecs::Entity parent, std::string_view klass, std::string_view name) {
			const ecs::ClassId id = ecs::Classes::Find(core::Name(klass));
			const bool local = ecs::IsClientLocalInstance(store, parent);
			const ecs::Entity made =
				local ? store.CreatePredictedInstance(id, name) : store.CreateInstance(id, name);
			if (local) {
				store.Set(made, ecs::ClientLocal{});
			}
			store.SetParent(made, parent);
			return made;
		}

		ecs::Entity
		ChildNamed(ecs::Store &store, ecs::Entity parent, std::string_view name, std::string_view klass) {
			ecs::Entity found = ecs::NULL_ENTITY;
			store.EachChild(parent, [&](ecs::Entity child) {
				if (found == ecs::NULL_ENTITY && store.IsA(child, ecs::Classes::Find(core::Name(klass))) &&
					store.Get<ecs::InstanceName>(child) != nullptr &&
					store.Get<ecs::InstanceName>(child)->Value.Text() == name) {
					found = child;
				}
			});
			if (found != ecs::NULL_ENTITY) {
				return found;
			}
			return MakeChild(store, parent, klass, name);
		}

		void Box(ecs::Store &store, ecs::Entity entity, core::Vector2 position, core::Vector2 size) {
			gui::Element element = store.Get<gui::Element>(entity) != nullptr
									   ? *store.Get<gui::Element>(entity)
									   : gui::Element{};
			element.Position = core::UDim2{0.0f, position.X, 0.0f, position.Y};
			element.Size = core::UDim2{0.0f, size.X, 0.0f, size.Y};
			store.Set(entity, element);
		}

		void Fill(
			ecs::Store &store,
			ecs::Entity entity,
			const angview::Rgba &colour,
			const angview::Rgba &border,
			int32_t borderPixels
		) {
			gui::Background background = store.Get<gui::Background>(entity) != nullptr
											 ? *store.Get<gui::Background>(entity)
											 : gui::Background{};
			background.Color = {colour.R, colour.G, colour.B};
			background.Transparency = 1.0f - colour.A;
			background.BorderColor = {border.R, border.G, border.B};
			background.BorderSizePixel = borderPixels;
			store.Set(entity, background);
		}

		// Takes the canvas edits made since the last render into the graph:
		// moved nodes, wires a player made and wires a player removed.
		void Pull(ViewObject &view, ecs::Store &store) {
			if (view.Source->ReadOnly || view.Canvas == ecs::NULL_ENTITY || !store.Alive(view.Canvas)) {
				return;
			}
			ang::Graph &document = view.Source->Document;
			store.EachChild(view.Canvas, [&](ecs::Entity child) {
				const gui::NodeCanvasNode *node = store.Get<gui::NodeCanvasNode>(child);
				const gui::Element *element = store.Get<gui::Element>(child);
				if (node == nullptr || element == nullptr) {
					return;
				}
				const std::string id(node->Id.Text());
				const auto rendered = view.RenderedPositions.find(id);
				const core::Vector2 now{element->Position.X.Offset, element->Position.Y.Offset};
				if (rendered != view.RenderedPositions.end() &&
					(rendered->second.X != now.X || rendered->second.Y != now.Y) &&
					document.FindNode(id) != nullptr) {
					document.Apply(ang::edit::MoveNode{id, now});
				}
			});
			std::vector<ang::Link> onCanvas;
			store.EachChild(view.Canvas, [&](ecs::Entity child) {
				if (const gui::NodeCanvasLink *link = store.Get<gui::NodeCanvasLink>(child)) {
					onCanvas.push_back(
						{std::string(link->FromNode.Text()),
						 std::string(link->FromPort.Text()),
						 std::string(link->ToNode.Text()),
						 std::string(link->ToPort.Text())}
					);
				}
			});
			for (const ang::Link &wire : view.RenderedLinks) {
				if (std::find(onCanvas.begin(), onCanvas.end(), wire) == onCanvas.end()) {
					document.Apply(ang::edit::RemoveLink{wire});
				}
			}
			for (const ang::Link &wire : onCanvas) {
				const auto links = document.Links();
				const bool known = std::find(links.begin(), links.end(), wire) != links.end();
				if (!known && ang::CheckNewLink(document, view.Source->Types->Types, wire).Ok) {
					document.Apply(ang::edit::AddLink{wire});
				}
			}
		}

		// Writes the graph onto a NodeCanvas as its nodes, ports and links,
		// coloured by the view's theme; stale children are removed.
		void Push(ViewObject &view, ecs::Store &store) {
			const angview::Theme &theme = view.Interaction->CurrentTheme();
			const angview::Layout &layout = view.Interaction->Geometry();
			if (gui::NodeCanvas *canvas = store.GetMutable<gui::NodeCanvas>(view.Canvas)) {
				canvas->GridVisible = theme.ShowGrid;
				canvas->GridSize = theme.GridSpacing;
			}
			Fill(store, view.Canvas, theme.Background, theme.Background, 0);
			std::unordered_set<std::string> nodes;
			view.RenderedPositions.clear();
			for (const angview::NodeLayout &node : layout.Nodes()) {
				nodes.insert(node.Id);
				const ecs::Entity entity = ChildNamed(store, view.Canvas, node.Id, "NodeCanvasNode");
				const core::Vector2 size = node.Bounds.Max - node.Bounds.Min;
				Box(store, entity, node.Bounds.Min, size);
				view.RenderedPositions[node.Id] = node.Bounds.Min;
				Fill(
					store, entity, theme.NodeBody, theme.NodeBorder, static_cast<int32_t>(theme.BorderWidth)
				);
				gui::NodeCanvasNode state = store.Get<gui::NodeCanvasNode>(entity) != nullptr
												? *store.Get<gui::NodeCanvasNode>(entity)
												: gui::NodeCanvasNode{};
				state.Id = core::Name(node.Id);
				state.Title = node.Title;
				store.Set(entity, state);
				const ecs::Entity title = ChildNamed(store, entity, "Title", "TextLabel");
				Box(store,
					title,
					{theme.NodePadding, 0.0f},
					{size.X - theme.NodePadding * 2.0f, theme.HeaderHeight});
				Fill(
					store,
					title,
					theme.ShowHeaders ? theme.NodeHeader : angview::Rgba{0, 0, 0, 0},
					theme.NodeHeader,
					0
				);
				gui::Label label =
					store.Get<gui::Label>(title) != nullptr ? *store.Get<gui::Label>(title) : gui::Label{};
				std::string text = node.Title;
				if (const angview::NodeStatusView *status = view.Interaction->StatusOf(node.Id);
					status != nullptr && theme.ShowTimings) {
					char timing[32];
					std::snprintf(timing, sizeof(timing), "  %.2f ms", status->Milliseconds);
					text += timing;
				}
				label.Text = text;
				label.Color = {theme.Text.R, theme.Text.G, theme.Text.B};
				label.Size = static_cast<int32_t>(theme.FontSize * theme.FontScale);
				label.XAlignment = gui::TextXAlignment::Left;
				store.Set(title, label);
				std::unordered_set<std::string> ports;
				for (const auto *list : {&node.Inputs, &node.Outputs}) {
					for (const angview::PortLayout &port : *list) {
						const bool input = port.Dir == ang::Direction::Input;
						const std::string name = (input ? "in:" : "out:") + port.Name;
						ports.insert(name);
						const ecs::Entity portEntity = ChildNamed(store, entity, name, "NodeCanvasPort");
						const float radius = theme.PortRadius;
						Box(store,
							portEntity,
							port.Centre - node.Bounds.Min - core::Vector2{radius, radius},
							{radius * 2.0f, radius * 2.0f});
						const ang::DataType *type = view.Source->Types->Types.FindType(port.Type);
						const angview::Rgba colour = type != nullptr && port.Type != ang::ANY_TYPE
														 ? angview::FromColor3(type->Wire)
														 : theme.WireFallback;
						Fill(store, portEntity, colour, colour, 0);
						if (gui::Element *element = store.GetMutable<gui::Element>(portEntity)) {
							element->Active = true;
						}
						store.Set(
							portEntity,
							gui::NodeCanvasPort{
								core::Name(port.Name),
								core::Name(port.Type),
								input ? gui::NodePortDirection::Input : gui::NodePortDirection::Output
							}
						);
					}
				}
				std::vector<ecs::Entity> stale;
				store.EachChild(entity, [&](ecs::Entity child) {
					const auto *name = store.Get<ecs::InstanceName>(child);
					if (store.Get<gui::NodeCanvasPort>(child) != nullptr &&
						(name == nullptr || !ports.contains(std::string(name->Value.Text())))) {
						stale.push_back(child);
					}
				});
				for (const ecs::Entity child : stale) {
					store.DestroyInstance(child);
				}
			}
			std::vector<ecs::Entity> stale;
			store.EachChild(view.Canvas, [&](ecs::Entity child) {
				const gui::NodeCanvasNode *node = store.Get<gui::NodeCanvasNode>(child);
				if ((node != nullptr && !nodes.contains(std::string(node->Id.Text()))) ||
					store.Get<gui::NodeCanvasLink>(child) != nullptr) {
					stale.push_back(child);
				}
			});
			for (const ecs::Entity child : stale) {
				store.DestroyInstance(child);
			}
			view.RenderedLinks.clear();
			for (const ang::Link &wire : view.Source->Document.Links()) {
				const ecs::Entity link = MakeChild(store, view.Canvas, "NodeCanvasLink", "Link");
				gui::NodeCanvasLink state;
				state.FromNode = core::Name(wire.FromNode);
				state.FromPort = core::Name(wire.FromPort);
				state.ToNode = core::Name(wire.ToNode);
				state.ToPort = core::Name(wire.ToPort);
				const ang::TypeId type = ang::PortType(
					view.Source->Document,
					view.Source->Types->Types,
					wire.FromNode,
					wire.FromPort,
					ang::Direction::Output
				);
				const ang::DataType *data = view.Source->Types->Types.FindType(type);
				state.LineColor =
					data != nullptr && type != ang::ANY_TYPE
						? data->Wire
						: core::Color3{theme.WireFallback.R, theme.WireFallback.G, theme.WireFallback.B};
				state.LineThickness = theme.WireWidth;
				store.Set(link, state);
				view.RenderedLinks.push_back(wire);
			}
		}

		void RenderView(ScriptCall &call) {
			auto view = Find<ViewObject>(call, 0, "view");
			const ecs::Entity canvas = call.AsInstance(1);
			ecs::Store &store = call.World();
			if (store.Get<gui::NodeCanvas>(canvas) == nullptr) {
				call.Raise("NodeGraph: a view is parented to a NodeCanvas");
			}
			const bool client = (call.Role().Client && !call.Role().Server) || store.AdoptOnly();
			const bool local = ecs::IsClientLocalInstance(store, canvas);
			if (client != local) {
				call.Raise(
					client ? "NodeGraph: client scripts may only draw on their own local canvas"
						   : "NodeGraph: server scripts cannot draw on a client-local canvas"
				);
			}
			if (view->Canvas != canvas) {
				view->RenderedPositions.clear();
				view->RenderedLinks.clear();
				view->Canvas = canvas;
			} else {
				Pull(*view, store);
			}
			view->Interaction->GraphChanged();
			Push(*view, store);
		}

		void SyncView(ScriptCall &call) {
			auto view = Find<ViewObject>(call, 0, "view");
			Pull(*view, call.World());
		}

		// Forgets what the view last rendered, so the next render takes the graph
		// over the canvas instead of reading the canvas as edits.
		void ResetView(ScriptCall &call) {
			Find<ViewObject>(call, 0, "view")->Canvas = ecs::NULL_ENTITY;
		}

		void Destroy(ScriptCall &call) {
			StateOf(call).Objects.erase(static_cast<uint32_t>(call.AsNumber(0)));
		}

		void Count(ScriptCall &call) {
			call.ReturnNumber(static_cast<double>(StateOf(call).Objects.size()));
		}

		constexpr std::array<ServiceMethod, 37> METHODS{{
			{"CreateRegistry", CreateRegistry},
			{"DefineType", DefineType},
			{"DefineNode", DefineNode},
			{"CreateGraph", CreateGraph},
			{"AddNode", AddNode},
			{"RemoveNode", RemoveNode},
			{"Link", Link},
			{"Unlink", Unlink},
			{"SetParam", SetParam},
			{"MoveNode", MoveNode},
			{"SetBypass", SetBypass},
			{"SetCollapsed", SetCollapsed},
			{"Save", Save},
			{"Load", Load},
			{"OpenImageGraph", OpenImageGraph},
			{"WriteImageGraph", WriteImageGraph},
			{"OpenProcedural", OpenProcedural},
			{"HostGraphNames", HostGraphNames},
			{"OpenHostGraph", OpenHostGraph},
			{"GetNodes", GetNodes},
			{"GetLinks", GetLinks},
			{"Validate", Validate},
			{"CreateRunner", CreateRunner},
			{"SetHook", SetHook},
			{"Run", Run},
			{"Output", Output},
			{"CreateView", CreateView},
			{"SetViewTheme", SetViewTheme},
			{"SetViewRun", SetViewRun},
			{"GetDrawCommands", GetDrawCommands},
			{"RenderView", RenderView},
			{"SyncView", SyncView},
			{"ResetView", ResetView},
			{"Destroy", Destroy},
			{"ObjectCount", Count},
			{"ThemeNames",
			 [](ScriptCall &call) {
				 ScriptValue names(ValueTag::Array);
				 for (const std::string &name : angview::Theme::PresetNames()) {
					 names.Items.push_back(Text(name));
				 }
				 call.ReturnValue(names);
			 }},
			{"Domains", [](ScriptCall &call) {
				 ScriptValue names(ValueTag::Array);
				 for (const ang::Domain domain : {ang::Domain::Tick, ang::Domain::Frame, ang::Domain::Tool}) {
					 names.Items.push_back(Text(ang::DomainName(domain)));
				 }
				 call.ReturnValue(names);
			 }},
		}};
	}

	const ServiceSurface &NodeGraphServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "NodeGraphService";
			surface.Methods = node_graph_detail::METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
