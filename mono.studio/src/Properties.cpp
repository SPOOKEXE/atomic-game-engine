#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/game/Values.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Enums.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <imgui.h>
#include <optional>
#include <studio/Assets.hpp>
#include <studio/Editor.hpp>
#include <studio/PropertySelection.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	// The one asset picker in this panel, opened for whichever property
	// asked. One id because one modal can be up at a time.
	static constexpr const char *ASSET_PICKER = "Choose content";

	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::ClassId;
	using engine::ecs::ComponentId;
	using engine::ecs::ComponentKind;
	using engine::ecs::Components;
	using engine::ecs::FieldDescriptor;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::PropertyDescriptor;
	using engine::ecs::PropertyType;
	using engine::ecs::Schema;
	using engine::ecs::Schemas;
	using engine::ecs::Store;
	using engine::ecs::TypeDescriptor;
	using engine::game::FormatValue;
	using engine::game::ParseValue;
	using engine::game::PropertyValue;
	using engine::game::ReadProperty;
	using engine::game::WriteProperty;

	namespace {
		// Drags at a rate that suits how big the number already is.
		//
		// A fixed step makes a part's `Transparency` unusable at 0.01 per pixel
		// and its `Position` unusable at 1.0 per pixel. Roblox solves this with
		// per-property increments in the API dump; scaling with the value is the
		// version of that which needs no table.
		float StepFor(float value) {
			const float magnitude = std::abs(value);
			if (magnitude < 1.0f) {
				return 0.005f;
			}
			if (magnitude < 100.0f) {
				return 0.05f;
			}
			return 0.5f;
		}

		bool ReadSchemaValue(const void *component, const FieldDescriptor &field, PropertyValue &value) {
			alignas(8) std::array<std::byte, 8> scratch{};
			const void *source = Schemas::ReadField(component, field, scratch.data());
			if (source == nullptr) return false;
			value = PropertyValue{};
			value.Type = field.Type;
			switch (field.Type) {
			case PropertyType::Bool:
				value.Bool = *reinterpret_cast<const bool *>(source);
				return true;
			case PropertyType::Int32:
				value.Int32 = *reinterpret_cast<const int32_t *>(source);
				return true;
			case PropertyType::Int64:
				value.Int64 = *reinterpret_cast<const int64_t *>(source);
				return true;
			case PropertyType::Float:
				value.Float = *reinterpret_cast<const float *>(source);
				return true;
			case PropertyType::Double:
				value.Double = *reinterpret_cast<const double *>(source);
				return true;
			case PropertyType::Name:
			case PropertyType::Enum:
				value.Name = *reinterpret_cast<const Name *>(source);
				return true;
			case PropertyType::String:
				value.String = *reinterpret_cast<const std::string *>(source);
				return true;
			case PropertyType::Vector3:
				value.Vector3 = *reinterpret_cast<const engine::core::Vector3 *>(source);
				return true;
			case PropertyType::CFrame:
				value.CFrame = *reinterpret_cast<const engine::core::CFrame *>(source);
				return true;
			case PropertyType::Color3:
				value.Color3 = *reinterpret_cast<const engine::core::Color3 *>(source);
				return true;
			case PropertyType::Vector2:
				value.Vector2 = *reinterpret_cast<const engine::core::Vector2 *>(source);
				return true;
			case PropertyType::UDim:
				value.UDim = *reinterpret_cast<const engine::core::UDim *>(source);
				return true;
			case PropertyType::UDim2:
				value.UDim2 = *reinterpret_cast<const engine::core::UDim2 *>(source);
				return true;
			case PropertyType::Rect:
				value.Rect = *reinterpret_cast<const engine::core::Rect *>(source);
				return true;
			case PropertyType::NumberRange:
				value.NumberRange = *reinterpret_cast<const engine::core::NumberRange *>(source);
				return true;
			case PropertyType::NumberSequence:
				value.NumberSequence = *reinterpret_cast<const engine::core::NumberSequence *>(source);
				return true;
			case PropertyType::ColorSequence:
				value.ColorSequence = *reinterpret_cast<const engine::core::ColorSequence *>(source);
				return true;
			case PropertyType::Reference:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		bool WriteSchemaValue(void *component, const FieldDescriptor &field, const PropertyValue &value) {
			if (value.Type != field.Type) return false;
			switch (field.Type) {
			case PropertyType::Bool:
				return Schemas::WriteField(component, field, &value.Bool);
			case PropertyType::Int32:
				return Schemas::WriteField(component, field, &value.Int32);
			case PropertyType::Int64:
				return Schemas::WriteField(component, field, &value.Int64);
			case PropertyType::Float:
				return Schemas::WriteField(component, field, &value.Float);
			case PropertyType::Double:
				return Schemas::WriteField(component, field, &value.Double);
			case PropertyType::Name:
			case PropertyType::Enum:
				return Schemas::WriteField(component, field, &value.Name);
			case PropertyType::String:
				return Schemas::WriteField(component, field, &value.String);
			case PropertyType::Vector3:
				return Schemas::WriteField(component, field, &value.Vector3);
			case PropertyType::CFrame:
				return Schemas::WriteField(component, field, &value.CFrame);
			case PropertyType::Color3:
				return Schemas::WriteField(component, field, &value.Color3);
			case PropertyType::Vector2:
				return Schemas::WriteField(component, field, &value.Vector2);
			case PropertyType::UDim:
				return Schemas::WriteField(component, field, &value.UDim);
			case PropertyType::UDim2:
				return Schemas::WriteField(component, field, &value.UDim2);
			case PropertyType::Rect:
				return Schemas::WriteField(component, field, &value.Rect);
			case PropertyType::NumberRange:
				return Schemas::WriteField(component, field, &value.NumberRange);
			case PropertyType::NumberSequence:
				return Schemas::WriteField(component, field, &value.NumberSequence);
			case PropertyType::ColorSequence:
				return Schemas::WriteField(component, field, &value.ColorSequence);
			case PropertyType::Reference:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}
	}

	void Editor::DrawShaderCapabilities(
		Entity shader, const engine::scene::ShaderSource &source, engine::core::Name name
	) {
		if (InspectedShaderWorld != SelectionWorld || InspectedShaderEntity != shader ||
			InspectedShaderRevision != source.Revision) {
			ShaderInspector.SetOptimise(true);
			InspectedShader =
				ShaderInspector.Compile(source.Code, engine::render::ShaderStage::Fragment, name.Text());
			InspectedShaderWorld = SelectionWorld;
			InspectedShaderEntity = shader;
			InspectedShaderRevision = source.Revision;
		}

		ImGui::SeparatorText("Shader capabilities");
		if (InspectedShader.Failed) {
			ImGui::TextWrapped("Compile failed: %s", InspectedShader.Error.c_str());
			return;
		}

		const engine::render::ShaderCapabilities &caps = InspectedShader.Capabilities;
		ImGui::Text(
			"%s, %u instructions, %.2f KiB SPIR-V",
			engine::render::Describe(caps.Stage),
			caps.Instructions,
			static_cast<double>(caps.SpirVBytes) / 1024.0
		);
		ImGui::TextDisabled(
			"static estimate: %u arithmetic, %u texture, %u memory, %u control flow",
			caps.ArithmeticInstructions,
			caps.TextureInstructions,
			caps.MemoryInstructions,
			caps.ControlFlowInstructions
		);
		ImGui::TextDisabled(
			"%u inputs, %u outputs, %zu resources, %llu minimum buffer bytes",
			caps.Inputs,
			caps.Outputs,
			caps.Resources.size(),
			static_cast<unsigned long long>(caps.DeclaredBufferBytes)
		);
		if (!caps.RequiredCapabilities.empty()) {
			ImGui::TextDisabled("SPIR-V capabilities:");
			for (const uint32_t capability : caps.RequiredCapabilities) {
				ImGui::SameLine();
				ImGui::TextDisabled("[%s]", engine::render::ShaderCapabilityName(capability).c_str());
			}
		}
		if (caps.Stage == engine::render::ShaderStage::Compute) {
			ImGui::TextDisabled("workgroup: %u x %u x %u", caps.WorkgroupX, caps.WorkgroupY, caps.WorkgroupZ);
		}

		if (!caps.Resources.empty() &&
			ImGui::BeginTable(
				"##shader-resources", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
			)) {
			ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 54.0f);
			ImGui::TableSetupColumn("Min bytes", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableHeadersRow();
			for (const engine::render::ShaderResourceEstimate &resource : caps.Resources) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(resource.Name.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(engine::render::Describe(resource.Kind));
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u:%u", resource.Set, resource.Binding);
				ImGui::TableSetColumnIndex(3);
				if (resource.MinimumBytes > 0) {
					ImGui::Text("%llu", static_cast<unsigned long long>(resource.MinimumBytes));
				} else {
					ImGui::TextDisabled("runtime");
				}
			}
			ImGui::EndTable();
		}

		for (const engine::render::ShaderOptimizationStep &step : InspectedShader.Optimizations) {
			ImGui::BulletText(
				"%s: %u -> %u instructions%s",
				engine::render::Describe(step.Kind),
				step.BeforeInstructions,
				step.AfterInstructions,
				step.Changed ? "" : " (no opportunity)"
			);
		}
	}

	void Editor::DrawUniverseProperties() {
		ImGui::TextUnformatted("Universe");
		ImGui::Separator();

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##property-filter", PropertyFilter, "filter properties");
		ImGui::Separator();

		const auto visible = [this](const char *label) {
			if (PropertyFilter.empty()) {
				return true;
			}

			int score = 0;
			return FuzzyMatch(PropertyFilter, label, score);
		};

		if (!ImGui::CollapsingHeader("Universe", ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}

		if (!ImGui::BeginTable("Universe", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
			return;
		}

		ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

		const auto row = [](const char *name) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(name);
			ImGui::TableSetColumnIndex(1);
			ImGui::PushID(name);
			ImGui::SetNextItemWidth(-1.0f);
		};

		if (visible("Name")) {
			row("Name");
			TextField("##v", UniverseNameDraft);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				if (UniverseNameDraft.empty()) {
					UniverseNameDraft = std::string(GameName.Text());
				} else if (!GameName.IsValid() || UniverseNameDraft != GameName.Text()) {
					// Intern once after editing, not once per keystroke. `Name` keeps
					// every spelling it interns for the life of the process.
					GameName = Name(UniverseNameDraft);
					MarkModified();
				}
			}
			if (UniverseNameDraft.empty() && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("A universe name cannot be empty");
			}
			ImGui::PopID();
		}

		const engine::world::UniverseSettings &settings = Universe->Settings();

		if (visible("Execution Mode")) {
			row("Execution Mode");
			int mode = settings.Mode == engine::world::ExecutionMode::WorldParallel ? 0 : 1;
			if (ImGui::Combo("##v", &mode, "World Parallel\0World Serial\0")) {
				Universe->SetMode(
					mode == 0 ? engine::world::ExecutionMode::WorldParallel
							  : engine::world::ExecutionMode::WorldSerial
				);
				MarkModified();
			}
			ImGui::PopID();
		}

		if (visible("Maximum Catch-Up Ticks")) {
			row("Maximum Catch-Up Ticks");
			int catchUp = settings.MaximumCatchUpTicks;
			if (ImGui::InputInt("##v", &catchUp)) {
				Universe->SetMaximumCatchUpTicks(catchUp);
				MarkModified();
			}
			ImGui::PopID();
		}

		if (visible("Bus Budget Per Tick")) {
			row("Bus Budget Per Tick");
			uint32_t budget = settings.BusBudgetPerTick;
			if (ImGui::InputScalar("##v", ImGuiDataType_U32, &budget)) {
				Universe->SetBusBudgetPerTick(budget);
				MarkModified();
			}
			ImGui::PopID();
		}

		if (visible("Channel Queue Limit")) {
			row("Channel Queue Limit");
			uint32_t queueLimit = settings.ChannelQueueLimit;
			if (ImGui::InputScalar("##v", ImGuiDataType_U32, &queueLimit)) {
				Universe->SetChannelQueueLimit(queueLimit);
				MarkModified();
			}
			ImGui::PopID();
		}

		if (visible("Channels Per World")) {
			row("Channels Per World");
			uint32_t channels = settings.ChannelsPerWorld;
			if (ImGui::InputScalar("##v", ImGuiDataType_U32, &channels)) {
				Universe->SetChannelsPerWorld(channels);
				MarkModified();
			}
			ImGui::PopID();
		}

		// --- diagnostics, read-only ------------------------------------
		//
		// Read every frame rather than cached, which is this program's rule
		// about anything the universe owns - a cached count is wrong for one
		// frame after a world is created, and one frame is enough to see.
		const engine::world::UniverseStatistics statistics = Universe->Statistics();

		if (visible("Federated Mode")) {
			row("Federated Mode");
			ImGui::TextUnformatted(settings.Federated ? "Federated" : "Local");
			ImGui::PopID();
		}

		if (visible("World Count")) {
			row("World Count");
			ImGui::Text("%zu", Universe->Count());
			ImGui::PopID();
		}

		if (visible("World States")) {
			row("World States");
			ImGui::Text(
				"%zu active · %zu suspended · %zu remote",
				statistics.ActiveWorlds,
				statistics.Suspended,
				statistics.Remote
			);
			ImGui::PopID();
		}

		if (visible("Fault Counts")) {
			row("Fault Counts");

			// Summed here rather than kept by the universe: a per-world fault
			// tally already exists on each world's statistics, and a second
			// running total would be the two-copies drift rule 2 names.
			uint32_t tickFaults = 0;
			for (const engine::world::WorldId world : Universe->Worlds()) {
				tickFaults += Universe->StatisticsOf(world).Faults;
			}
			ImGui::Text("%zu faulted worlds · %u tick faults", statistics.Faulted, tickFaults);
			ImGui::PopID();
		}

		if (visible("Tick Cost")) {
			row("Tick Cost");
			ImGui::Text("%.2f ms", statistics.LastTickMilliseconds);
			ImGui::PopID();
		}

		if (visible("Bus Traffic")) {
			row("Bus Traffic");
			ImGui::Text(
				"%llu operations · %llu deliveries",
				static_cast<unsigned long long>(statistics.BusOperations),
				static_cast<unsigned long long>(statistics.Deliveries)
			);
			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	void Editor::DrawWorldProperties(WorldId world) {
		const Name name = Universe->NameOf(world);

		ImGui::Text("%s", name.IsValid() ? Label(name) : "?");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted("(world)");
		ImGui::PopStyleColor();
		ImGui::Separator();

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##property-filter", PropertyFilter, "filter properties");
		ImGui::Separator();

		const auto visible = [this](const char *label) {
			if (PropertyFilter.empty()) {
				return true;
			}

			int score = 0;
			return FuzzyMatch(PropertyFilter, label, score);
		};

		if (!ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}

		if (!ImGui::BeginTable("World", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
			return;
		}

		ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

		const auto row = [](const char *label) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::PushID(label);
			ImGui::SetNextItemWidth(-1.0f);
		};

		// **Read every frame rather than held in a draft.** The universe panel's
		// rule and for its reason: these are the world's own numbers, a Play run
		// or a game file load can change them underneath this panel, and a
		// cached copy would show the old one until somebody clicked away.
		engine::world::WorldSettings settings = Universe->SettingsOf(world);
		bool edited = false;

		// **The name is not editable here.** The registry is keyed on it and
		// `Universe::Reconfigure` ignores it outright; renaming is Rename Scene
		// in the explorer's own context menu, which does the registry half.
		if (visible("Name")) {
			row("Name");
			ImGui::TextUnformatted(name.IsValid() ? Label(name) : "?");
			ImGui::PopID();
		}

		if (visible("State")) {
			row("State");
			ImGui::TextUnformatted(engine::world::Describe(Universe->StateOf(world)));
			ImGui::PopID();
		}

		// --- the three rates, which is what this panel exists for ------------

		if (visible("Tick Rate")) {
			row("Tick Rate");
			double rate = settings.TickRate;
			if (ImGui::InputDouble("##v", &rate, 0.0, 0.0, "%.1f Hz")) {
				// **Floored at one rather than left at what was typed.**
				// `FixedTimestep::SetRate` already refuses zero and substitutes
				// one, so a zero typed here would be shown back as zero and run
				// at 1 Hz - a panel disagreeing with the clock it is reporting.
				settings.TickRate = std::max(rate, 1.0);
				edited = true;
			}
			ImGui::PopID();
		}

		if (visible("Idle Tick Rate")) {
			row("Idle Tick Rate");
			double rate = settings.IdleTickRate;
			if (ImGui::InputDouble("##v", &rate, 0.0, 0.0, "%.1f Hz")) {
				settings.IdleTickRate = std::max(rate, 1.0);
				edited = true;
			}
			ImGui::PopID();
		}

		if (visible("Physics Tick Rate")) {
			row("Physics Tick Rate");
			double rate = settings.PhysicsTickRate;
			if (ImGui::InputDouble("##v", &rate, 0.0, 0.0, "%.1f Hz")) {
				// Zero is meaningful here and is the default: it follows
				// whichever tick rate is in force. Negative is not, and
				// `physics::SanePhysicsRate` would turn it into zero anyway -
				// clamping here means the panel shows what the clock will do.
				settings.PhysicsTickRate = std::max(rate, 0.0);
				edited = true;
			}
			ImGui::PopID();

			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Physics steps per second. 0 follows the tick rate.\n"
					"Lower it to make a heavy world affordable: the solver is the most\n"
					"expensive thing in a tick and the least sensitive to running slower."
				);
			}
		}

		if (visible("Replication Tick Rate")) {
			row("Replication Tick Rate");
			double rate = settings.ReplicationTickRate;
			if (ImGui::InputDouble("##v", &rate, 0.0, 0.0, "%.1f Hz")) {
				settings.ReplicationTickRate = std::max(rate, 0.0);
				edited = true;
			}
			ImGui::PopID();

			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Snapshots published per second. 0 publishes on every tick.\n"
					"Change bits are held across the ticks between two published ones,\n"
					"so a property written on a skipped tick still reaches the wire."
				);
			}
		}

		if (visible("Simulated Latency")) {
			row("Simulated Latency");
			double latency = settings.GlobalSimulatedNetworkLatency;
			if (ImGui::InputDouble("##v", &latency, 0.0, 0.0, "%.1f ms")) {
				settings.GlobalSimulatedNetworkLatency = std::max(latency, 0.0);
				edited = true;
			}
			ImGui::PopID();
		}

		if (visible("Fault Limit")) {
			row("Fault Limit");
			int limit = static_cast<int>(settings.FaultLimit);
			if (ImGui::InputInt("##v", &limit)) {
				settings.FaultLimit = static_cast<uint32_t>(std::max(limit, 1));
				edited = true;
			}
			ImGui::PopID();
		}

		// --- what a world is, rather than how fast it runs -------------------

		if (visible("Isolation")) {
			row("Isolation");
			ImGui::TextUnformatted(engine::world::Describe(settings.IsolationLevel));
			ImGui::PopID();
		}

		// **Read-only here and editable in the render pipeline panel**, which is
		// the one place that knows which profiles the universe actually has. Two
		// editors for one name would be two ways to set it to something that
		// does not exist.
		if (visible("Rendering Profile")) {
			row("Rendering Profile");
			ImGui::TextUnformatted(
				settings.RenderingProfile.IsValid() ? Label(settings.RenderingProfile) : "-"
			);
			ImGui::PopID();
		}

		// --- diagnostics, read-only ------------------------------------------

		const engine::world::WorldStatistics statistics = Universe->StatisticsOf(world);

		if (visible("Ticks")) {
			row("Ticks");
			ImGui::Text("%llu", static_cast<unsigned long long>(statistics.Ticks));
			ImGui::PopID();
		}

		if (visible("Tick Cost")) {
			row("Tick Cost");
			ImGui::Text(
				"%.2f ms · slowest %.2f ms",
				statistics.LastTickMilliseconds,
				statistics.SlowestTickMilliseconds
			);
			ImGui::PopID();
		}

		// **Dropped ticks beside the rate that produces them.** A number that
		// keeps climbing is a world that cannot keep up with its own tick rate,
		// which is the exact reading that should send somebody to the physics
		// rate two rows above.
		if (visible("Dropped Ticks")) {
			row("Dropped Ticks");
			if (statistics.DroppedTicks > 0) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
				ImGui::Text("%llu", static_cast<unsigned long long>(statistics.DroppedTicks));
				ImGui::PopStyleColor();
			} else {
				ImGui::TextUnformatted("0");
			}
			ImGui::PopID();
		}

		if (visible("Replication Ticks")) {
			row("Replication Ticks");
			ImGui::Text("%llu", static_cast<unsigned long long>(statistics.ReplicationTicks));
			ImGui::PopID();
		}

		if (visible("Faults")) {
			row("Faults");
			ImGui::Text("%u", statistics.Faults);
			ImGui::PopID();
		}

		ImGui::EndTable();

		// **Applied after the table rather than inside it**, because
		// `ApplyWorldSettings` enters the world and a panel that entered while
		// drawing a row would be inside `Universe::Enter` twice over the course
		// of one table. The edit is one frame of numbers either way.
		if (edited) {
			ApplyWorldSettings(world, settings);
		}
	}

	void Editor::DrawProperties() {
		if (!ShowProperties) {
			return;
		}

		if (!ImGui::Begin("Properties", &ShowProperties)) {
			ImGui::End();
			return;
		}

		if (UniverseSelected) {
			DrawUniverseProperties();
			ImGui::End();
			return;
		}

		// **After the universe and before the instance branch**, which is the
		// order the explorer's tree is in. The three are mutually exclusive
		// because `ClearRootSelection` is what any other selection goes through.
		if (SelectedWorldRow.IsValid()) {
			DrawWorldProperties(SelectedWorldRow);
			ImGui::End();
			return;
		}

		if (Selection.empty() || !SelectionWorld.IsValid()) {
			ImGui::TextDisabled("nothing selected");
			ImGui::End();
			return;
		}

		if (Selection.size() > 1) {
			// **Edits apply to every selected instance.** Stated rather than
			// left to be discovered, because the alternative reading - that it
			// edits the first one - is equally plausible and differs by however
			// many objects somebody just changed.
			ImGui::TextDisabled("%zu selected - an edit applies to all of them", Selection.size());
			ImGui::Separator();
		}

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##property-filter", PropertyFilter, "filter properties");
		ImGui::Separator();

		struct Edit {
			Name Property;
			ClassId Owner;
			PropertyType Type = PropertyType::Opaque;
			PropertyValue Value;
			bool Wanted = false;
		};
		Edit edit;
		struct SelectedShader {
			Entity Instance;
			engine::scene::ShaderSource Source;
			Name Label;
		};
		std::optional<SelectedShader> selectedShader;

		const bool authoritative = AuthorityOf(SelectionWorld) == EditAuthority::Authoritative;
		Universe->Enter(SelectionWorld, [&](Store &store) {
			const auto primary = std::find_if(Selection.begin(), Selection.end(), [&](Entity instance) {
				return store.Alive(instance) && store.ClassOf(instance).IsValid();
			});
			if (primary == Selection.end()) {
				ImGui::TextDisabled("the selection is gone");
				return;
			}

			const ClassId primaryClass = store.ClassOf(*primary);
			const engine::ecs::ClassInfo &info = Classes::Describe(primaryClass);
			const bool oneClass = std::all_of(Selection.begin(), Selection.end(), [&](Entity instance) {
				return !store.Alive(instance) || store.ClassOf(instance) == primaryClass;
			});

			if (oneClass) {
				ImGui::Text("%s", Label(info.Name));
			} else {
				ImGui::Text("Mixed classes");
			}
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("in %s", Label(Universe->NameOf(SelectionWorld)));
			ImGui::PopStyleColor();

			// **Where the edit lands, said before it is made rather than
			// discovered after.** The panel looks the same for a scene and for a
			// `Play` run's client view, and a write to the second reaches one
			// client and is undone by the next delta. Warning-coloured for that
			// case only: an author editing a scene is doing the ordinary thing
			// and does not need a badge shouting at them for it.
			const EditAuthority authority = AuthorityOf(SelectionWorld);
			if (authority == EditAuthority::ClientLocal) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
				ImGui::Text("[%s]", Describe(authority));
				ImGui::PopStyleColor();

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"This is a client's view of a run. An edit here changes that one\n"
						"client, is never sent, and is overwritten the next time the server\n"
						"replicates this row. Edit the scene itself to change every client."
					);
				}
			}

			ImGui::Separator();
			if (Selection.size() == 1 && oneClass && primaryClass == engine::scene::ShaderScriptClass()) {
				if (const engine::scene::ShaderSource *source =
						store.Get<engine::scene::ShaderSource>(*primary)) {
					selectedShader = SelectedShader{*primary, *source, store.InstanceNameOf(*primary)};
				}
			}

			const std::vector<SelectionPropertyGroup> groups = BuildPropertySelection(store, Selection);

			for (const auto &group : groups) {
				const engine::ecs::ClassInfo &owner = Classes::Describe(group.Owner);
				std::string heading(Label(owner.Name));
				if (group.Applicable != Selection.size()) {
					heading += " (" + std::to_string(group.Applicable) + " of " +
							   std::to_string(Selection.size()) + ")";
				}

				if (!ImGui::CollapsingHeader(heading.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
					continue;
				}

				if (!ImGui::BeginTable(
						heading.c_str(), 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
					)) {
					continue;
				}

				ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
				ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

				for (const SelectionPropertyRow &row : group.Rows) {
					const PropertyDescriptor *descriptor = row.Descriptor;
					if (descriptor == nullptr) {
						continue;
					}
					if (!PropertyFilter.empty()) {
						int score = 0;
						if (!FuzzyMatch(PropertyFilter, Label(descriptor->Name), score)) {
							continue;
						}
					}
					const PropertyValue &value = row.Value;
					const bool readable = row.Readable != 0;

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(Label(descriptor->Name));

					ImGui::TableSetColumnIndex(1);
					ImGui::PushID(Label(descriptor->Name));
					ImGui::SetNextItemWidth(-1.0f);

					if (!readable) {
						// The instance does not carry what the getter reads.
						// Shown as unavailable rather than hidden, because a
						// property that vanishes from the list when a part is
						// anchored reads as the editor losing it.
						ImGui::TextDisabled("-");
						ImGui::PopID();
						continue;
					}

					const bool locked = !descriptor->Writable;
					ImGui::BeginDisabled(locked);

					PropertyValue changed = value;
					bool wrote = false;

					if (row.Mixed && descriptor->Type != PropertyType::Reference &&
						descriptor->Type != PropertyType::Opaque) {
						// A mixed selection has no truthful primary value. An empty
						// field says that directly and accepts the same textual form a
						// scene file does; once valid, the one value is written to every
						// selected instance that actually declares this property.
						std::string text;
						if (TextField("##v", text)) {
							PropertyValue parsed;
							std::string reason;
							if (ParseValue(descriptor->Type, text, parsed, reason)) {
								changed = parsed;
								wrote = true;
							}
						}
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("selected instances have different values");
						}
						ImGui::EndDisabled();
						if (wrote && !locked) {
							edit.Property = descriptor->Name;
							edit.Owner = group.Owner;
							edit.Type = descriptor->Type;
							edit.Value = changed;
							edit.Wanted = true;
						}
						ImGui::PopID();
						continue;
					}

					switch (descriptor->Type) {
					case PropertyType::Bool:
						wrote = ImGui::Checkbox("##v", &changed.Bool);
						break;

					case PropertyType::Int32:
						wrote = ImGui::DragInt("##v", &changed.Int32);
						break;

					case PropertyType::Int64: {
						int narrowed = static_cast<int>(changed.Int64);
						if (ImGui::DragInt("##v", &narrowed)) {
							changed.Int64 = narrowed;
							wrote = true;
						}
						break;
					}

					case PropertyType::Float:
						wrote = ImGui::DragFloat("##v", &changed.Float, StepFor(changed.Float));
						break;

					case PropertyType::Double: {
						auto narrowed = static_cast<float>(changed.Double);
						if (ImGui::DragFloat("##v", &narrowed, StepFor(narrowed))) {
							changed.Double = narrowed;
							wrote = true;
						}
						break;
					}

					case PropertyType::Vector3: {
						float parts[3]{changed.Vector3.X, changed.Vector3.Y, changed.Vector3.Z};
						if (ImGui::DragFloat3("##v", parts, StepFor(parts[0]))) {
							changed.Vector3 = engine::core::Vector3{parts[0], parts[1], parts[2]};
							wrote = true;
						}
						break;
					}

					case PropertyType::Color3: {
						float parts[3]{changed.Color3.R, changed.Color3.G, changed.Color3.B};
						if (ImGui::ColorEdit3("##v", parts, ImGuiColorEditFlags_Float)) {
							changed.Color3 = engine::core::Color3{parts[0], parts[1], parts[2]};
							wrote = true;
						}
						break;
					}

					case PropertyType::CFrame: {
						// Position only. The rotation has its own property -
						// `Orientation`, in degrees, which is what an author
						// actually wants - and offering a raw quaternion
						// beside it would be four numbers nobody can edit by
						// hand next to three that are obvious.
						float parts[3]{
							changed.CFrame.Position.X,
							changed.CFrame.Position.Y,
							changed.CFrame.Position.Z,
						};
						if (ImGui::DragFloat3("##v", parts, StepFor(parts[0]))) {
							changed.CFrame.Position = engine::core::Vector3{parts[0], parts[1], parts[2]};
							wrote = true;
						}
						break;
					}

					case PropertyType::Vector2: {
						float parts[2]{changed.Vector2.X, changed.Vector2.Y};
						if (ImGui::DragFloat2("##v", parts, StepFor(parts[0]))) {
							changed.Vector2 = engine::core::Vector2{parts[0], parts[1]};
							wrote = true;
						}
						break;
					}

					case PropertyType::UDim: {
						// **Scale and offset get different steps.** Scale is
						// a fraction, so a drag in whole numbers moves it
						// past the parent in one pixel of mouse travel;
						// offset is pixels and a hundredth of one is a drag
						// that never arrives. Two controls rather than a
						// `DragFloat2` is what buys the two steps.
						float scale = changed.UDim.Scale;
						float offset = changed.UDim.Offset;
						const float half = ImGui::GetContentRegionAvail().x * 0.5f - 2.0f;

						ImGui::SetNextItemWidth(half);
						if (ImGui::DragFloat("##s", &scale, 0.01f)) {
							changed.UDim.Scale = scale;
							wrote = true;
						}
						ImGui::SameLine(0.0f, 4.0f);
						ImGui::SetNextItemWidth(half);
						if (ImGui::DragFloat("##o", &offset, 1.0f, 0.0f, 0.0f, "%.0f")) {
							changed.UDim.Offset = offset;
							wrote = true;
						}
						break;
					}

					case PropertyType::UDim2: {
						// Two rows of the pair above, X then Y - which is
						// the shape Roblox's own property grid uses, and the
						// one an author reading `UDim2.new(0.5, -8, 0, 24)`
						// already has in their head.
						const float half = ImGui::GetContentRegionAvail().x * 0.5f - 2.0f;
						float axes[4]{
							changed.UDim2.X.Scale,
							changed.UDim2.X.Offset,
							changed.UDim2.Y.Scale,
							changed.UDim2.Y.Offset,
						};

						ImGui::SetNextItemWidth(half);
						if (ImGui::DragFloat("##xs", &axes[0], 0.01f)) {
							wrote = true;
						}
						ImGui::SameLine(0.0f, 4.0f);
						ImGui::SetNextItemWidth(half);
						if (ImGui::DragFloat("##xo", &axes[1], 1.0f, 0.0f, 0.0f, "%.0f")) {
							wrote = true;
						}
						ImGui::SetNextItemWidth(half);
						if (ImGui::DragFloat("##ys", &axes[2], 0.01f)) {
							wrote = true;
						}
						ImGui::SameLine(0.0f, 4.0f);
						ImGui::SetNextItemWidth(half);
						if (ImGui::DragFloat("##yo", &axes[3], 1.0f, 0.0f, 0.0f, "%.0f")) {
							wrote = true;
						}

						if (wrote) {
							changed.UDim2 = engine::core::UDim2{axes[0], axes[1], axes[2], axes[3]};
						}
						break;
					}

					case PropertyType::Rect: {
						// Four pixel offsets into an image, so one step and
						// one control. `%.0f` because a fractional texel in
						// a slice centre is an author's typo rather than an
						// intent.
						float parts[4]{
							changed.Rect.Min.X,
							changed.Rect.Min.Y,
							changed.Rect.Max.X,
							changed.Rect.Max.Y,
						};
						if (ImGui::DragFloat4("##v", parts, 1.0f, 0.0f, 0.0f, "%.0f")) {
							changed.Rect = engine::core::Rect{parts[0], parts[1], parts[2], parts[3]};
							wrote = true;
						}
						break;
					}

					case PropertyType::Enum: {
						// **The registered set, not a text field.** That is
						// the whole reason `PropertyType::Enum` exists: a
						// typed `AlphaMode = "Clipp"` is refused where it
						// was written rather than landing in the component
						// and surfacing as a part drawn with the default.
						// A combo makes the typo impossible rather than
						// caught.
						// **Asked for inside the open combo, not beside it.**
						// `MembersOf` takes the enum registry's lock and
						// returns its member list *by value*, and the list is
						// read nowhere else - so a closed combo, which is
						// every combo on almost every frame, paid a lock and
						// a heap allocation per enum row for a vector it
						// threw away.
						const char *current = changed.Name.IsValid() ? Label(changed.Name) : "";
						if (ImGui::BeginCombo("##v", current)) {
							const std::vector<Name> members =
								engine::ecs::EnumTable::MembersOf(descriptor->EnumName);
							for (const Name member : members) {
								if (ImGui::Selectable(Label(member), member == changed.Name)) {
									changed.Name = member;
									wrote = true;
								}
							}
							ImGui::EndCombo();
						}
						break;
					}

					case PropertyType::Name: {
						std::string text =
							changed.Name.IsValid() ? std::string(Label(changed.Name)) : std::string{};

						// **A picker for the handful of properties that name
						// content, and a plain field for every other `Name`.**
						// `Mesh`, `TextureID`, `Texture`, `SoundId` and `Image`
						// take a string a publisher wrote - rule 4 - and getting
						// one wrong has no visible failure: an unknown mesh draws
						// the missing-mesh marker, which is also what a mesh that
						// has not streamed in yet looks like. Every other `Name`
						// property is an ordinary label, and a modal over one
						// would be a dialog in the way.
						const engine::assets::AssetKind content = ContentKindOfProperty(descriptor->Spelling);

						if (content == engine::assets::AssetKind::Unknown) {
							if (TextField("##v", text)) {
								changed.Name = text.empty() ? Name{} : Name(text);
								wrote = true;
							}
							break;
						}

						// **The field stays, narrowed.** Somebody who knows the
						// name should still be able to paste it, and a property
						// only settable through a modal would be one a script can
						// write and a person cannot.
						const float browse = engine::ui::Scaled(28.0f);
						ImGui::SetNextItemWidth(-(browse + ImGui::GetStyle().ItemSpacing.x));
						if (TextField("##v", text)) {
							changed.Name = text.empty() ? Name{} : Name(text);
							wrote = true;
						}

						ImGui::SameLine();
						if (ImGui::Button("...", ImVec2(browse, 0.0f)) && !locked) {
							// **Opened after the loop rather than here.** An
							// `OpenPopup` inside the table is inside this
							// property's `PushID`, so its id would not be the one
							// `BeginPopupModal` computes at the window's root, and
							// the popup would never appear. Recorded here and
							// opened where the modal is drawn.
							PickerWanted = true;
							PickerKind = content;
							PickerProperty = descriptor->Name;
							PickerOwner = group.Owner;
							PickerType = descriptor->Type;
							PickerChoice = text;
						}
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("choose from the content store");
						}
						break;
					}

					case PropertyType::String:
						// **The same widget as `Name` and a different
						// meaning**, which is the whole of the distinction
						// showing up in the one place a person can see it:
						// the field is edited character by character, and
						// each keystroke used to intern a string that never
						// went away. Typing a sentence into a `Name`
						// property leaks it a letter at a time.
						if (TextField("##v", changed.String)) {
							wrote = true;
						}
						break;

					case PropertyType::Reference: {
						// Read-only for now, and it says so rather than
						// offering a control that does nothing. Picking a
						// reference means a target picker over the tree,
						// which is `mono.studio/AGENTS.md`'s deferred list.
						const Name target = changed.Reference == NULL_ENTITY
												? Name{}
												: store.InstanceNameOf(changed.Reference);
						ImGui::TextDisabled("%s", target.IsValid() ? Label(target) : "(none)");
						break;
					}

					case PropertyType::NumberRange: {
						float parts[2]{changed.NumberRange.Minimum, changed.NumberRange.Maximum};
						if (ImGui::DragFloat2("##v", parts, StepFor(parts[1]))) {
							// **Not clamped so the minimum stays below the
							// maximum.** Dragging either handle past the other
							// is how a person *inverts* a range, and a widget
							// that silently pushed the other end along would
							// make that impossible to express and impossible
							// to notice. `game::ParseValue` refuses to reorder
							// for the same reason.
							changed.NumberRange = engine::core::NumberRange{parts[0], parts[1]};
							wrote = true;
						}
						break;
					}

					// --- the two curves ---------------------------------
					//
					// **A text field, and a curve editor is deliberately not
					// here.** `game::FormatValue` already writes a sequence as
					// `0, 1, 0; 1, 0, 0` and `ParseValue` reads it back, so a
					// text field is a complete, round-tripping editor for
					// about six lines - and the alternative is a spline widget
					// with keypoint dragging, which is a panel rather than a
					// row and belongs beside the emitter preview rather than
					// in the generic property list.
					//
					// What the text field is *not* is comfortable, and that is
					// the honest trade rather than a claim it is fine. The
					// curve editor is `mono.studio/AGENTS.md`'s deferred list.
					//
					// **Parsed on commit rather than per keystroke**, because
					// half a typed gradient is a parse failure and writing one
					// per character would fight the person typing it.
					case PropertyType::NumberSequence:
					case PropertyType::ColorSequence: {
						std::string text = FormatValue(changed);
						if (TextField("##v", text)) {
							PropertyValue parsed;
							std::string reason;
							if (ParseValue(descriptor->Type, text, parsed, reason)) {
								changed = parsed;
								wrote = true;
							}
						}
						break;
					}

					case PropertyType::Opaque:
						ImGui::TextDisabled("(not readable as a value)");
						break;
					}

					ImGui::EndDisabled();

					if (wrote && !locked) {
						// Recorded rather than applied. Writing here works for
						// one instance and not for a multi-selection, and doing
						// it in two places is how the two get different rules.
						edit.Property = descriptor->Name;
						edit.Owner = group.Owner;
						edit.Type = descriptor->Type;
						edit.Value = changed;
						edit.Wanted = true;
					}

					ImGui::PopID();
				}

				ImGui::EndTable();
			}
		});
		if (selectedShader) {
			DrawShaderCapabilities(selectedShader->Instance, selectedShader->Source, selectedShader->Label);
		}

		// **Drawn at the window's root, which is the only place its id
		// matches the `OpenPopup` beside it.** See the `...` button for why
		// the open is deferred rather than done where it is clicked.
		if (PickerWanted) {
			ImGui::OpenPopup(ASSET_PICKER);
			PickerWanted = false;
		}

		if (DrawAssetPicker(ASSET_PICKER, PickerKind, PickerChoice) && PickerProperty.IsValid()) {
			// The picker spans frames, so what it confirms feeds the same
			// `edit` a widget would have - one path applies a property write
			// and it stays one path, including undo.
			edit.Property = PickerProperty;
			edit.Owner = PickerOwner;
			edit.Type = PickerType;

			// **`ChosenContentValue` and not four lines here**, because those
			// four lines left `Type` at `Opaque` for a whole version and
			// `game::WriteProperty` refused every one of them without a word.
			// The function takes the type, so there is no longer a way to write
			// this and forget it.
			edit.Value = ChosenContentValue(PickerType, PickerChoice);
			edit.Wanted = true;
			PickerProperty = Name{};
			PickerOwner = ClassId{};
		}

		ImGui::End();

		if (!edit.Wanted) {
			return;
		}

		// Applied to every selected instance, in its own `Enter`. A property
		// another instance does not have is refused by `SetProperty` rather
		// than skipped here, which keeps the rule in one place.
		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : Selection) {
				if (!store.Alive(instance)) {
					continue;
				}

				const ClassId klass = store.ClassOf(instance);
				if (!klass.IsValid()) {
					continue;
				}

				if (!SelectionPropertyApplies(klass, edit.Owner, edit.Property, edit.Type)) {
					continue;
				}
				for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
					if (descriptor.Name == edit.Property && descriptor.Type == edit.Type) {
						// **One command per instance, because the write is per
						// instance.** A multi-selection whose members held
						// different values before cannot be reversed by one
						// entry carrying one "before" - undo would give every
						// one of them whatever the first happened to have.
						//
						// `RecordProperty` drops a write that changed nothing,
						// which is what keeps the members that already agreed
						// off the stack.
						PropertyValue before;
						const bool had = ReadProperty(store, instance, descriptor, before);

						if (engine::game::WriteAuthoredProperty(store, instance, descriptor, edit.Value) &&
							had && authoritative && Commands != nullptr) {
							Commands->RecordProperty(
								SelectionWorld,
								instance,
								descriptor.Name,
								before,
								edit.Value,
								"Set " + std::string(Label(descriptor.Name))
							);
						}
						break;
					}
				}
			}
		});

		// **A mesh that is already loaded gets no arrival to hang the fit on.**
		// `DrainContent` reshapes parts when geometry lands, which covers the
		// ordinary case of naming a mesh nothing had fetched yet. Picking one a
		// previous part already pulled in fires nothing at all, so the part would
		// keep whatever box it had and stretch the new mesh into it.
		if (edit.Property == Name("MeshId")) {
			engine::core::Vector3 extent;
			if (Renderer.MeshExtentOf(edit.Value.Name, extent)) {
				FitPartsToMesh(edit.Value.Name, extent);
			}
		}

		if (authoritative) {
			MarkModified();
		}
	}

	void Editor::DrawComponents() {
		if (!ShowComponents) {
			return;
		}

		if (!ImGui::Begin("Components", &ShowComponents)) {
			ImGui::End();
			return;
		}

		if (Selection.empty() || !SelectionWorld.IsValid()) {
			ImGui::TextDisabled("nothing selected");
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##component-filter", ComponentFilter, "filter components");
		ImGui::Separator();

		Universe->Enter(SelectionWorld, [&](Store &store) {
			const Entity instance = Selection.front();
			if (!store.Alive(instance)) {
				ImGui::TextDisabled("the selection is gone");
				return;
			}

			for (const ComponentId component : store.ComponentsOf(instance)) {
				const TypeDescriptor &descriptor = Components::Describe(component);
				int score = 0;
				if (!ComponentFilter.empty() && !FuzzyMatch(ComponentFilter, Label(descriptor.Name), score)) {
					continue;
				}

				ImGui::PushID(component.Index);
				if (ImGui::CollapsingHeader(Label(descriptor.Name), ImGuiTreeNodeFlags_DefaultOpen)) {
					const void *componentValue = store.GetComponent(instance, component);
					ImGui::TextDisabled(
						"%s, %u bytes",
						descriptor.Kind == ComponentKind::Tag ? "tag" : "data",
						descriptor.Size
					);

					if (const Schema *schema = Schemas::Of(component); schema != nullptr) {
						uint32_t logicalBytes = 0;
						for (const FieldDescriptor &field : schema->Fields()) {
							logicalBytes += Schemas::SizeOf(field.Type);
						}
						if (logicalBytes != schema->Size()) {
							ImGui::TextDisabled(
								"packing: %u logical bytes -> %u resident bytes", logicalBytes, schema->Size()
							);
						}
						const std::vector<std::string> tags = schema->Tags();
						if (!tags.empty()) {
							ImGui::TextDisabled("tags:");
							for (const std::string &tag : tags) {
								ImGui::SameLine();
								ImGui::TextDisabled("[%s]", tag.c_str());
							}
						}
						for (const FieldDescriptor &field : schema->Fields()) {
							if (!field.Exposed || componentValue == nullptr) {
								ImGui::BulletText(
									"%s: %s / %s, %u bits at %u:%u",
									field.Spelling.data(),
									engine::ecs::Describe(field.Type),
									engine::ecs::Describe(field.Packing),
									field.StorageBits,
									field.Offset,
									field.BitOffset
								);
								continue;
							}
							PropertyValue value;
							if (!ReadSchemaValue(componentValue, field, value)) {
								continue;
							}
							ImGui::TextUnformatted(field.Spelling.data());
							ImGui::SameLine();
							ImGui::TextDisabled("[config]");
							ImGui::SameLine();
							ImGui::TextDisabled("[%s]", engine::ecs::Describe(field.Packing));
							ImGui::SameLine();
							ImGui::PushID(field.Name.Id());
							std::string key =
								std::string(descriptor.Name.Text()) + "." + std::string(field.Spelling);
							std::string &draft = ComponentConfigDrafts[key];
							if (!ImGui::IsItemActive() && draft.empty()) {
								draft = FormatValue(value);
							}
							ImGui::SetNextItemWidth(-1.0f);
							TextField("##config", draft);
							if (ImGui::IsItemDeactivatedAfterEdit()) {
								PropertyValue parsed;
								std::string reason;
								if (ParseValue(field.Type, draft, parsed, reason) &&
									WriteSchemaValue(
										store.GetComponentMutable(instance, component), field, parsed
									)) {
									MarkModified();
								}
								draft.clear();
							}
							ImGui::PopID();
							for (const std::string &tag : field.Tags) {
								ImGui::SameLine();
								ImGui::TextDisabled("[%s]", tag.c_str());
							}
						}
					}

					for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
						if (property.Reads == nullptr || !property.Reads->Contains(component)) {
							continue;
						}

						PropertyValue value;
						ImGui::Text("%s", Label(property.Name));
						if (ReadProperty(store, instance, property, value)) {
							ImGui::SameLine();
							ImGui::TextDisabled("%s", FormatValue(value).c_str());
						}
					}
				}
				ImGui::PopID();
			}
		});

		ImGui::End();
	}
}
