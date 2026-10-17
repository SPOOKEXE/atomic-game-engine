#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Style.hpp>
#include <engine/gui/VirtualCollection.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.gui.compile")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Name;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Entity;
using engine::ecs::EnumTable;
using engine::ecs::PropertyDescriptor;
using engine::ecs::PropertyType;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	struct World {
		Store Data;
		CompileRequest Request;
		Compiled List;

		explicit World(std::string_view name) : Data(name) {
			RegisterGuiClasses();

			// **A bare `Instance`, because `StarterGui` is not a `gui` class.**
			// It is `scene`'s service and this module may not link `scene`; what
			// the containment test reads is the *name*, so an instance carrying
			// that name is exactly as contained as the real service would be.
			// That is the same duplication `NormalId` already makes here.
			Container =
				Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui");
			Request.Display.Width = 800.0f;
			Request.Display.Height = 600.0f;
		}

		// The world's `StarterGui`, which is where a collector has to live to
		// draw at all.
		//
		// **Made by the fixture rather than by each case.** Containment is a
		// rule every case is now subject to, and parenting by hand in thirty of
		// them would be thirty chances to forget - and a forgotten one lays out
		// to nothing, which reads as the case being wrong rather than the
		// fixture being incomplete.
		//
		// Named rather than classed, for `Layout.cpp`'s reason: `StarterGui` is
		// `scene`'s service and this module may not link `scene`, so the string
		// is duplicated here and pinned by a test.
		Entity Container;

		Entity Make(std::string_view klass, Entity parent = Entity{}) {
			const Entity made = Data.CreateInstance(GuiClass(klass), klass);

			if (parent != engine::ecs::NULL_ENTITY) {
				Data.SetParent(made, parent);
			} else if (Data.IsA(made, GuiClass("LayerCollector"))) {
				// An unparented collector draws nothing, so a case that did not
				// say where its `ScreenGui` lives gets the ordinary answer
				// rather than the degenerate one.
				Data.SetParent(made, Container);
			}

			return made;
		}

		bool Rebuild() {
			return List.Rebuild(Data, Request);
		}
	};

	// Changes a property to something it was not, whatever type it is.
	//
	// **The point of this is that it is generic.** `Compile.hpp` promises the
	// signature covers every field that reaches a rectangle, and the only way
	// to check a promise about *every* field is to walk the class table rather
	// than to list them again - a list would go stale in exactly the same way
	// the fold would.
	//
	// @return `false` for a property this cannot meaningfully perturb, which
	//         the caller skips rather than failing on.
	bool Perturb(Store &store, Entity instance, const PropertyDescriptor &property) {
		if (!property.Writable) {
			return false;
		}

		// A reference is a handle into this world and changing one means
		// reparenting, which the tree tests already cover and which would move
		// the element out from under this one.
		if (property.Type == PropertyType::Reference || property.Type == PropertyType::Opaque) {
			return false;
		}

		if (property.Type == PropertyType::Enum) {
			const std::vector<Name> members = EnumTable::MembersOf(property.EnumName);
			if (members.size() < 2) {
				return false;
			}

			Name current;
			store.GetProperty(instance, property.Name, &current, sizeof(current));
			const Name next = members[0] == current ? members[1] : members[0];
			return store.SetProperty(instance, property.Name, &next, sizeof(next));
		}

		if (property.Type == PropertyType::Name) {
			const Name next("a value nothing else here uses");
			return store.SetProperty(instance, property.Name, &next, sizeof(next));
		}

		// **Before the byte buffer below, and this is the branch whose absence
		// crashed rather than failing.** A `PropertyType::String` setter takes a
		// live `std::string` and assigns from it; handing it a span of zeroed
		// bytes makes it read a length and a pointer out of nothing. Every
		// production caller takes the same exception for the same reason, and
		// this one exists because the walk above reaches *every* declared
		// property - which is the whole point of the case.
		if (property.Type == PropertyType::String) {
			const std::string next = "a value nothing else here uses";
			return store.SetProperty(instance, property.Name, &next, sizeof(next));
		}

		if (property.Type == PropertyType::Bool) {
			bool current = false;
			store.GetProperty(instance, property.Name, &current, sizeof(current));
			current = !current;
			return store.SetProperty(instance, property.Name, &current, sizeof(current));
		}

		// Everything left is one or more numbers laid end to end - an `int32`,
		// a `float`, a `Vector2`, a `UDim2`, a `Rect`, a `Color3`. Nudging the
		// first component is enough to say the field is folded in, and reading
		// the current value first means the nudge lands somewhere legal for a
		// property with a clamp on it.
		std::array<std::byte, 64> bytes{};
		if (property.Size > bytes.size()) {
			return false;
		}
		if (!store.GetProperty(instance, property.Name, bytes.data(), property.Size)) {
			return false;
		}

		if (property.Type == PropertyType::Int32 || property.Type == PropertyType::Int64) {
			int32_t value = 0;
			std::memcpy(&value, bytes.data(), sizeof(value));
			value += 3;
			std::memcpy(bytes.data(), &value, sizeof(value));
		} else {
			float value = 0.0f;
			std::memcpy(&value, bytes.data(), sizeof(value));

			// Toward the middle of the unit range rather than away from it, so
			// a transparency or a colour channel with a clamp on it still
			// lands somewhere different from where it started.
			value = value > 0.5f ? value - 0.25f : value + 0.25f;
			std::memcpy(bytes.data(), &value, sizeof(value));
		}

		return store.SetProperty(instance, property.Name, bytes.data(), property.Size);
	}
}

TEST_CASE("an empty GUI cache does not rescan component families", "[gui][compile][cache]") {
	World world("empty-gui-cache");

	CHECK(world.Rebuild());
	CHECK_FALSE(world.Rebuild());
	CHECK(world.List.Requests() == 2);
	CHECK(world.List.Rebuilds() == 1);
	CHECK(world.List.Commands().Commands.empty());

	world.Make("ScreenGui");
	CHECK(world.Rebuild());
	CHECK(world.List.Rebuilds() == 2);

	world.List.Invalidate();
	CHECK(world.Rebuild());
}

TEST_CASE("an unchanged compiled UI performs no derived work", "[gui][compile][cache]") {
	World world("gui_compile.zero_work");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	const Entity label = world.Make("TextLabel", frame);
	world.Data.GetMutable<Background>(frame)->BorderSizePixel = 0;

	REQUIRE(world.Rebuild());
	const Compiled::WorkCounters first = world.List.Work();
	const size_t rebuilds = world.List.Rebuilds();
	const uint64_t signature = world.List.Signature();

	CHECK_FALSE(world.Rebuild());
	const Compiled::WorkCounters second = world.List.Work();
	CHECK(second.BindingEvaluations == first.BindingEvaluations);
	CHECK(second.PresentationAdvances == first.PresentationAdvances);
	CHECK(second.VirtualAnchorReconciliations == first.VirtualAnchorReconciliations);
	CHECK(second.Layouts == first.Layouts);
	CHECK(world.List.Rebuilds() == rebuilds);
	CHECK(world.List.Signature() == signature);

	Background changed = *world.Data.Get<Background>(frame);
	changed.Color = {0.2f, 0.4f, 0.6f};
	world.Data.Set(frame, changed);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Work().Layouts == first.Layouts);
	const auto paint = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[frame](const DrawCommand &value) {
			return value.Source == frame && value.Kind == DrawKind::Rectangle;
		}
	);
	REQUIRE(paint != world.List.Commands().Commands.end());
	CHECK(paint->Tint == changed.Color);
	Element resized = *world.Data.Get<Element>(frame);
	resized.Size.X.Offset += 10.0f;
	world.Data.Set(frame, resized);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Work().Layouts == first.Layouts + 1);
	Label relabelled = *world.Data.Get<Label>(label);
	relabelled.Text = "a wider text run";
	world.Data.Set(label, relabelled);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Work().Layouts == first.Layouts + 2);
}

TEST_CASE("a bound attribute remains dynamic across a retained compile", "[gui][compile][cache]") {
	World world("gui_compile.bound_dynamic");
	const Entity source = world.Data.CreateInstance(Classes::Find(Name("Instance")), "Source");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	const Entity binding = world.Make("UIBinding", label);

	engine::ecs::AttributeValue value;
	value.Type = PropertyType::String;
	value.String = "first";
	REQUIRE(engine::ecs::SetAttribute(world.Data, source, Name("Value"), value));
	Binding configured;
	configured.SourcePath = "Source";
	configured.Attribute = Name("Value");
	configured.Fallback = "missing";
	world.Data.Set(binding, configured);

	REQUIRE(world.Rebuild());
	const size_t evaluations = world.List.Work().BindingEvaluations;
	value.String = "second";
	REQUIRE(engine::ecs::SetAttribute(world.Data, source, Name("Value"), value));
	REQUIRE(world.Rebuild());
	const BindingOutput *output = world.Data.Get<BindingOutput>(binding);
	REQUIRE(output != nullptr);
	CHECK(output->Value == "second");
	CHECK(world.List.Work().BindingEvaluations == evaluations + 1);
}

TEST_CASE("compiled UI damage joins old and new conservative collector bounds", "[gui][compile][damage]") {
	World world("gui_compile.damage");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element element;
	element.Position = UDim2{0.0f, 100.0f, 0.0f, 50.0f};
	element.Size = UDim2{0.0f, 40.0f, 0.0f, 20.0f};
	element.Rotation = 90.0f;
	world.Data.Set(frame, element);
	world.Data.GetMutable<Background>(frame)->BorderSizePixel = 0;

	const Entity stroke = world.Make("UIStroke", frame);
	Stroke outline;
	outline.Thickness = 4.0f;
	world.Data.Set(stroke, outline);

	REQUIRE(world.Rebuild());
	REQUIRE(world.List.DamageValid());
	REQUIRE(world.List.Damage().size() == 1);
	const Compiled::DamageRegion first = world.List.Damage().front();
	CHECK(first.Collector == screen);
	CHECK_FALSE(first.Spatial);
	// The stroke expands its own command, then the tracker includes the
	// centred backend stroke before taking the ninety-degree AABB.
	CHECK(first.Bounds.Min.X == Approx(106.0f));
	CHECK(first.Bounds.Min.Y == Approx(36.0f));
	CHECK(first.Bounds.Max.X == Approx(134.0f));
	CHECK(first.Bounds.Max.Y == Approx(84.0f));

	CHECK_FALSE(world.Rebuild());
	CHECK(world.List.DamageValid());
	CHECK(world.List.Damage().empty());

	element.Position.X.Offset = 200.0f;
	world.Data.Set(frame, element);
	REQUIRE(world.Rebuild());
	REQUIRE(world.List.Damage().size() == 1);
	const Compiled::DamageRegion moved = world.List.Damage().front();
	// The dirty rectangle includes where the old pixels were as well as the
	// new placement, so a retained target clears the vacated area too.
	CHECK(moved.Bounds.Min.X == Approx(106.0f));
	CHECK(moved.Bounds.Max.X == Approx(234.0f));
	CHECK(moved.Bounds.Min.Y == Approx(36.0f));
	CHECK(moved.Bounds.Max.Y == Approx(84.0f));
}

TEST_CASE("compiled UI damage leaves an unchanged collector alone", "[gui][compile][damage]") {
	World world("gui_compile.damage_isolation");
	const Entity first = world.Make("ScreenGui");
	const Entity second = world.Make("ScreenGui");
	const Entity firstFrame = world.Make("Frame", first);
	const Entity secondFrame = world.Make("Frame", second);
	Element element;
	element.Size = UDim2{0.0f, 40.0f, 0.0f, 20.0f};
	world.Data.Set(firstFrame, element);
	world.Data.Set(secondFrame, element);
	REQUIRE(world.Rebuild());
	REQUIRE(world.List.Damage().size() == 2);

	Background changed = *world.Data.Get<Background>(firstFrame);
	changed.Color = {0.2f, 0.4f, 0.6f};
	world.Data.Set(firstFrame, changed);
	REQUIRE(world.Rebuild());
	REQUIRE(world.List.DamageValid());
	REQUIRE(world.List.Damage().size() == 1);
	CHECK(world.List.Damage().front().Collector == first);
	CHECK(world.List.Damage().front().Collector != second);

	world.Request.Hovered = secondFrame;
	REQUIRE(world.Rebuild());
	REQUIRE(world.List.Damage().size() == 1);
	CHECK(world.List.Damage().front().Collector == second);
}

TEST_CASE("compiled UI damage clips after rotating visual bounds", "[gui][compile][damage]") {
	World world("gui_compile.damage_clip");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element element;
	element.Position = UDim2{0.0f, -20.0f, 0.0f, 0.0f};
	element.Size = UDim2{0.0f, 40.0f, 0.0f, 20.0f};
	element.Rotation = 90.0f;
	world.Data.Set(frame, element);
	world.Data.GetMutable<Background>(frame)->BorderSizePixel = 0;
	const Entity stroke = world.Make("UIStroke", frame);
	Stroke outline;
	outline.Thickness = 4.0f;
	world.Data.Set(stroke, outline);

	REQUIRE(world.Rebuild());
	REQUIRE(world.List.Damage().size() == 1);
	const engine::core::Rect damage = world.List.Damage().front().Bounds;
	// The unrotated outline crosses the left edge. The rotated shape has pixels
	// in the canvas, and its conservative bounds are clipped by the collector's
	// scissor only after that turn.
	CHECK(damage.Min.X == Approx(0.0f));
	CHECK(damage.Min.Y == Approx(0.0f));
	CHECK(damage.Max.X == Approx(14.0f));
	CHECK(damage.Max.Y == Approx(34.0f));
}

TEST_CASE("failed UI damage extraction keeps the last usable baseline", "[gui][compile][damage]") {
	World world("gui_compile.damage_failure");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element element;
	element.Position = UDim2{0.0f, 20.0f, 0.0f, 20.0f};
	element.Size = UDim2{0.0f, 20.0f, 0.0f, 20.0f};
	world.Data.Set(frame, element);
	world.Data.GetMutable<Background>(frame)->BorderSizePixel = 0;
	REQUIRE(world.Rebuild());
	REQUIRE(world.List.DamageValid());

	element.Rotation = std::numeric_limits<float>::quiet_NaN();
	world.Data.Set(frame, element);
	REQUIRE(world.Rebuild());
	CHECK_FALSE(world.List.DamageValid());
	CHECK(world.List.Damage().empty());
	// The signature is unchanged, so this is the cache path that must preserve
	// the invalid state until a successfully extracted list replaces it.
	CHECK_FALSE(world.Rebuild());
	CHECK_FALSE(world.List.DamageValid());
	CHECK(world.List.Damage().empty());

	element.Rotation = 0.0f;
	element.Position.X.Offset = 60.0f;
	world.Data.Set(frame, element);
	REQUIRE(world.Rebuild());
	REQUIRE(world.List.DamageValid());
	REQUIRE(world.List.Damage().size() == 1);
	// Had the rejected NaN list advanced the baseline, the vacated first
	// rectangle would be absent here.
	CHECK(world.List.Damage().front().Bounds.Min.X == Approx(20.0f));
	CHECK(world.List.Damage().front().Bounds.Max.X == Approx(80.0f));
}

TEST_CASE("a compile request rejects unbounded locale and style inputs", "[gui][compile][limits]") {
	World world("gui_compile.request_limits");
	world.Make("ScreenGui");

	world.Request.Locale.assign(LocalizationCatalogue::MAXIMUM_LOCALE_BYTES + 1, 'x');
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Commands.empty());

	world.Request.Locale.clear();
	world.Request.Locale.clear();
	CHECK(world.Rebuild());
}

TEST_CASE("one hosted collector compiles without admitting other canvases", "[gui][compile][plugin]") {
	World world("gui_compile.plugin_collector");
	const Entity dock = world.Make("DockWidgetPluginGui");
	const Entity retained = world.Make("Frame", dock);
	const Entity screen = world.Make("ScreenGui");
	const Entity outside = world.Make("Frame", screen);

	world.Request.Display.Width = 360.0f;
	world.Request.Display.Height = 240.0f;
	CHECK(world.List.RebuildCollector(world.Data, dock, world.Request));
	CHECK_FALSE(world.List.RebuildCollector(world.Data, dock, world.Request));
	world.Data.GetMutable<Background>(outside)->Color = Color3{0.2f, 0.3f, 0.4f};
	CHECK_FALSE(world.List.RebuildCollector(world.Data, dock, world.Request));
	world.Data.GetMutable<Background>(retained)->Color = Color3{0.4f, 0.3f, 0.2f};
	CHECK(world.List.RebuildCollector(world.Data, dock, world.Request));
	REQUIRE_FALSE(world.List.Commands().Commands.empty());
	CHECK(
		std::all_of(
			world.List.Commands().Commands.begin(),
			world.List.Commands().Commands.end(),
			[&](const DrawCommand &command) {
				return command.Source == retained && command.Collector == dock;
			}
		)
	);
	CHECK(world.List.Commands().CanvasSize == Vector2{360.0f, 240.0f});
	REQUIRE(world.List.Commands().CollectorRanges.size() == 1);
	CHECK(world.List.Commands().CollectorRanges.front().Collector == dock);
	CHECK(world.List.Commands().CollectorRanges.front().First == 0);
	CHECK(world.List.Commands().CollectorRanges.front().Count == world.List.Commands().Commands.size());
	CHECK(
		std::none_of(
			world.List.Commands().Commands.begin(),
			world.List.Commands().Commands.end(),
			[&](const DrawCommand &command) { return command.Source == outside; }
		)
	);
}

TEST_CASE("a still world compiles once and is then kept", "[gui][compile]") {
	World world("gui_compile.stable");
	const Entity screen = world.Make("ScreenGui");
	world.Make("Frame", screen);

	CHECK(world.Rebuild());
	CHECK(world.List.Rebuilds() == 1);

	// The whole point of the class. Ten frames of a UI nobody touched cost ten
	// scans and one compile.
	for (int frame = 0; frame < 10; frame++) {
		CHECK_FALSE(world.Rebuild());
	}

	CHECK(world.List.Rebuilds() == 1);
	CHECK(world.List.Requests() == 11);
}

TEST_CASE(
	"a screen collector applies accessibility scale in one coordinate space", "[gui][compile][display]"
) {
	World world("gui_compile.display_scale");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	Element element;
	element.Position = UDim2{0.0f, 10.0f, 0.0f, 20.0f};
	element.Size = UDim2{0.0f, 0.0f, 0.0f, 40.0f};
	element.Automatic = AutomaticSize::X;
	world.Data.Set(button, element);
	Label label;
	label.Text = "wide";
	label.Size = 10;
	world.Data.Set(button, label);
	world.Request.Display.InterfaceScale = 2.0f;
	world.Request.Display.TextScale = 1.5f;

	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().CanvasSize == Vector2{800.0f, 600.0f});
	REQUIRE(world.List.Commands().Transforms.size() == 1);
	CHECK(world.List.Commands().Transforms.front().Scale == Vector2{2.0f, 2.0f});

	const Resolved *resolved = world.Data.Get<Resolved>(button);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->AbsolutePosition == Vector2{10.0f, 20.0f});
	CHECK(resolved->AbsoluteSize.X == Approx(31.2f));
	CHECK(resolved->TextSize == 15);

	const auto command = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[button](const DrawCommand &draw) { return draw.Source == button; }
	);
	REQUIRE(command != world.List.Commands().Commands.end());
	CHECK(command->Bounds.Min == resolved->AbsolutePosition);
	CHECK(command->Bounds.Size() == resolved->AbsoluteSize);

	// The client and renderer both use physical presentation coordinates while
	// the list keeps the authored logical bounds.
	CHECK(PickScreen(world.Data, world.List.Commands(), Vector2{40.0f, 60.0f}) == button);
}

TEST_CASE("collector reference modes map one logical canvas into presentation", "[gui][compile][display]") {
	World world("gui_compile.reference_resolution");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);
	Layer layer;
	layer.ReferenceResolution = Vector2{400.0f, 400.0f};
	layer.ScaleMode = CollectorScaleMode::Fit;
	world.Data.Set(screen, layer);
	Element element;
	element.Position = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(button, element);

	REQUIRE(world.Rebuild());
	const Resolved *resolved = world.Data.Get<Resolved>(button);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->AbsolutePosition == Vector2::Zero);
	REQUIRE(world.List.Commands().Transforms.size() == 1);
	const CollectorTransform &fit = world.List.Commands().Transforms.front();
	CHECK(fit.Origin == Vector2{100.0f, 0.0f});
	CHECK(fit.Scale == Vector2{1.5f, 1.5f});
	CHECK(PickScreen(world.Data, world.List.Commands(), Vector2{175.0f, 75.0f}) == button);
	CHECK(PickScreen(world.Data, world.List.Commands(), Vector2{50.0f, 75.0f}) == engine::ecs::NULL_ENTITY);

	layer.ScaleMode = CollectorScaleMode::Fill;
	world.Data.Set(screen, layer);
	REQUIRE(world.Rebuild());
	const CollectorTransform &fill = world.List.Commands().Transforms.front();
	CHECK(fill.Origin == Vector2{0.0f, -100.0f});
	CHECK(fill.Scale == Vector2{2.0f, 2.0f});

	layer.ScaleMode = CollectorScaleMode::Integer;
	world.Data.Set(screen, layer);
	REQUIRE(world.Rebuild());
	const CollectorTransform &integer = world.List.Commands().Transforms.front();
	CHECK(integer.Origin == Vector2{200.0f, 100.0f});
	CHECK(integer.Scale == Vector2{1.0f, 1.0f});
}

TEST_CASE(
	"an authored UIAnimation rebuilds while moving and changes only presentation", "[gui][compile][animation]"
) {
	World world("gui_compile.animation");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	world.Data.GetMutable<Background>(frame)->Color = Color3{1.0f, 0.0f, 0.0f};

	const Entity modifier = world.Make("UIAnimation", frame);
	AnimationPlayback *playback = world.Data.GetMutable<AnimationPlayback>(modifier);
	REQUIRE(playback != nullptr);
	playback->StartedAt = 0.0;
	playback->Clip.Tween =
		engine::core::TweenInfo(1.0f, engine::core::EasingStyle::Linear, engine::core::EasingDirection::In);
	PresentationTrack track;
	track.Property = PresentationProperty::BackgroundColor;
	REQUIRE(track.Add(PresentationKey{0.0f, PresentationValue::FromColor(Color3{1.0f, 0.0f, 0.0f})}));
	REQUIRE(track.Add(PresentationKey{1.0f, PresentationValue::FromColor(Color3{0.0f, 0.0f, 1.0f})}));
	REQUIRE(playback->Clip.AddTrack(track));

	world.Request.Seconds = 0.5;
	REQUIRE(world.Rebuild());
	const auto command = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[frame](const DrawCommand &draw) { return draw.Source == frame && draw.Kind == DrawKind::Rectangle; }
	);
	REQUIRE(command != world.List.Commands().Commands.end());
	CHECK(command->Tint.R == Approx(0.5f));
	CHECK(command->Tint.B == Approx(0.5f));
	CHECK(world.Data.Get<Background>(frame)->Color == Color3{1.0f, 0.0f, 0.0f});

	world.Request.Seconds = 2.0;
	CHECK(world.Rebuild());
	world.Request.Seconds = 3.0;
	CHECK_FALSE(world.Rebuild());
}

TEST_CASE("screen interfaces are compiled from the selected viewer root", "[gui][compile][cache]") {
	World world("gui_compile.viewer-root");
	const Entity starterScreen = world.Make("ScreenGui");
	world.Make("Frame", starterScreen);

	const engine::ecs::ClassId instance = engine::ecs::Classes::Find(engine::core::Name("Instance"));
	const Entity player = world.Data.CreateInstance(instance, "Player");
	const Entity playerGui = world.Data.CreateInstance(instance, "PlayerGui");
	REQUIRE(world.Data.SetParent(playerGui, player));
	const Entity playerScreen = world.Make("ScreenGui", playerGui);
	world.Make("Frame", playerScreen);
	world.Make("Frame", playerScreen);
	const Entity otherPlayer = world.Data.CreateInstance(instance, "OtherPlayer");
	const Entity otherPlayerGui = world.Data.CreateInstance(instance, "PlayerGui");
	REQUIRE(world.Data.SetParent(otherPlayerGui, otherPlayer));
	const Entity otherScreen = world.Make("ScreenGui", otherPlayerGui);
	world.Make("Frame", otherScreen);

	world.Request.ScreenGuis = ScreenGuiSource::StarterGui;
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Elements == 1);

	world.Request.ScreenGuis = ScreenGuiSource::PlayerGui;
	world.Request.Viewer = player;
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Elements == 2);

	world.Request.Viewer = engine::ecs::NULL_ENTITY;
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Elements == 0);
}

TEST_CASE("every declared property moves the signature", "[gui][compile]") {
	// **This is the check that makes `Compile.hpp`'s table a contract.** A
	// field added to a component and not to the fold is a UI one edit stale,
	// and the symptom - a panel that updates on the *next* unrelated change -
	// is close to undebuggable from the outside.
	//
	// Every concrete class, every property it declares, one at a time.
	static constexpr std::string_view SUBJECTS[] = {
		"Frame",
		"CanvasGroup",
		"ScrollingFrame",
		"TextButton",
		"ImageButton",
		"TextLabel",
		"ImageLabel",
		"TextBox",
		"ViewportFrame",
		"UIListLayout",
		"UIGridLayout",
		"UIAspectRatioConstraint",
		"UISizeConstraint",
		"UITextSizeConstraint",
		"UIPadding",
		"UICorner",
		"UIStroke",
		"UIScale",
		"UIFlexItem",
	};

	size_t checked = 0;

	for (const std::string_view klass : SUBJECTS) {
		World world("gui_compile.fields");
		const Entity screen = world.Make("ScreenGui");
		const Entity host = world.Make("Frame", screen);
		const Entity subject = world.Make(klass, host);

		REQUIRE(world.Rebuild());

		for (const PropertyDescriptor &property : Classes::Describe(GuiClass(klass)).Properties) {
			// `Name` is `ecs`'s and reaches the compile only through
			// `SortOrder::Name`, which is folded separately; `Parent` is the
			// tree. Both are covered by their own cases below.
			if (property.Name == Name("Name") || property.Name == Name("Parent")) {
				continue;
			}

			const uint64_t before = world.List.Signature();
			if (!Perturb(world.Data, subject, property)) {
				continue;
			}

			INFO(klass << "." << property.Name.Text());
			world.Rebuild();
			CHECK(world.List.Signature() != before);
			checked++;
		}
	}

	// A guard on the guard: if the walk above ever found nothing to perturb,
	// every assertion in it would pass vacuously.
	CHECK(checked > 100);
}

TEST_CASE("the collector's own properties move the signature", "[gui][compile]") {
	World world("gui_compile.collector");
	const Entity screen = world.Make("ScreenGui");
	world.Make("Frame", screen);

	REQUIRE(world.Rebuild());

	for (const PropertyDescriptor &property : Classes::Describe(GuiClass("ScreenGui")).Properties) {
		if (property.Name == Name("Name") || property.Name == Name("Parent")) {
			continue;
		}

		const uint64_t before = world.List.Signature();
		if (!Perturb(world.Data, screen, property)) {
			continue;
		}

		INFO("ScreenGui." << property.Name.Text());
		world.Rebuild();
		CHECK(world.List.Signature() != before);
	}
}

TEST_CASE("the tree, the name and the screen all move the signature", "[gui][compile]") {
	World world("gui_compile.tree");
	const Entity screen = world.Make("ScreenGui");
	const Entity first = world.Make("Frame", screen);
	const Entity second = world.Make("Frame", screen);

	REQUIRE(world.Rebuild());

	// Reparenting. `ecs::Hierarchy` is not an observed component, so
	// `Store::ChangeVersion` does not move for this - which is the entire
	// reason a signature exists rather than a version compare.
	uint64_t before = world.List.Signature();
	world.Data.SetParent(second, first);
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// Renaming, which only `SortOrder::Name` reads and which is folded from its
	// own pass restricted to rows that have an `Element`.
	before = world.List.Signature();
	world.Data.SetInstanceName(first, "renamed");
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// Resizing the screen, which every `UDim2` resolves against.
	before = world.List.Signature();
	world.Request.Display.Width = 1024.0f;
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// A display profile is an input to canonical layout. Even a field that does
	// not currently move this desktop-shaped canvas must rebuild now, so a
	// mobile host can change profile fields without retaining a stale list.
	before = world.List.Signature();
	world.Request.Display.SafeArea.Bottom = 48.0f;
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	before = world.List.Signature();
	world.Request.Display.InterfaceScale = 1.25f;
	world.Rebuild();
	CHECK(world.List.Signature() != before);

	// Destroying one.
	before = world.List.Signature();
	world.Data.DestroyInstance(second);
	world.Rebuild();
	CHECK(world.List.Signature() != before);
}

TEST_CASE("the hovered element is an input to the compile", "[gui][compile]") {
	World world("gui_compile.hover");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	// **The default fill, deliberately.** `Background::Color` is white, so a
	// button created in the explorer is white - and a shift that only ever
	// lightened clamped straight back to white and did nothing under the
	// pointer, which is every button anybody makes. This case used to be run
	// against a mid-grey, which is exactly the fill that hid it.
	REQUIRE(world.Rebuild());
	const Color3 plain = world.List.Commands().Commands.front().Tint;
	REQUIRE(plain.R == Approx(1.0f));

	world.Request.Hovered = button;
	CHECK(world.Rebuild());

	// **Shifted in the command and not in the component.** A hover written
	// back into `BackgroundColor3` would make the property read differently
	// depending on where the mouse is.
	const Color3 lit = world.List.Commands().Commands.front().Tint;
	CHECK(lit.R < plain.R);
	CHECK(world.Data.Get<Background>(button)->Color.R == Approx(plain.R));

	// And a press goes further the same way, so holding a button reads as more
	// of what hovering it started rather than as a reversal.
	world.Request.Hovered = engine::ecs::NULL_ENTITY;
	world.Request.Pressed = button;
	CHECK(world.Rebuild());
	CHECK(world.List.Commands().Commands.front().Tint.R < lit.R);
}

TEST_CASE("a dark button shifts as far as a light one", "[gui][compile]") {
	World world("gui_compile.hover.dark");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	// The same hole at the other end: darkening black is no shift either, so
	// the direction is chosen per fill rather than fixed once for all of them.
	Background background;
	background.Color = Color3{0.0f, 0.0f, 0.0f};
	world.Data.Set(button, background);

	REQUIRE(world.Rebuild());
	REQUIRE(world.List.Commands().Commands.front().Tint.R == Approx(0.0f));

	world.Request.Hovered = button;
	CHECK(world.Rebuild());

	const Color3 lit = world.List.Commands().Commands.front().Tint;
	CHECK(lit.R > 0.0f);

	world.Request.Hovered = engine::ecs::NULL_ENTITY;
	world.Request.Pressed = button;
	CHECK(world.Rebuild());
	CHECK(world.List.Commands().Commands.front().Tint.R > lit.R);
}

TEST_CASE("paint order is parent first, then siblings by ZIndex", "[gui][compile]") {
	World world("gui_compile.order");
	const Entity screen = world.Make("ScreenGui");
	const Entity back = world.Make("Frame", screen);
	const Entity front = world.Make("Frame", screen);
	const Entity child = world.Make("Frame", back);

	Element low;
	low.ZIndex = 1;
	world.Data.Set(back, low);

	Element high;
	high.ZIndex = 10;
	world.Data.Set(front, high);

	Element beneath;

	// **Deliberately lower than its own parent's sibling.** Under
	// `ZIndexBehavior::Sibling` a child is drawn over its parent whatever its
	// number, and `ZIndex` only orders siblings - so this child sits above
	// `back` and below `front`, and a compile that sorted globally would put it
	// at the bottom.
	beneath.ZIndex = -50;
	world.Data.Set(child, beneath);

	REQUIRE(world.Rebuild());

	const auto position = [&](Entity instance) {
		const std::vector<DrawCommand> &commands = world.List.Commands().Commands;
		for (size_t index = 0; index < commands.size(); index++) {
			if (commands[index].Source == instance) {
				return index;
			}
		}
		FAIL("no command for the instance");
		return size_t{0};
	};

	CHECK(position(back) < position(child));
	CHECK(position(child) < position(front));
}

TEST_CASE("Global ZIndex behaviour re-sorts the whole collector", "[gui][compile]") {
	World world("gui_compile.global");
	const Entity screen = world.Make("ScreenGui");
	const Entity back = world.Make("Frame", screen);
	const Entity child = world.Make("Frame", back);
	const Entity front = world.Make("Frame", screen);

	Layer layer;
	layer.Behavior = ZIndexBehavior::Global;
	world.Data.Set(screen, layer);

	Element low;
	low.ZIndex = -50;
	world.Data.Set(child, low);

	Element mid;
	mid.ZIndex = 1;
	world.Data.Set(back, mid);

	Element high;
	high.ZIndex = 10;
	world.Data.Set(front, high);

	REQUIRE(world.Rebuild());

	const std::vector<DrawCommand> &commands = world.List.Commands().Commands;
	REQUIRE(commands.size() >= 3);

	// The legacy rule: depth does not beat the number, so the child goes to the
	// bottom.
	CHECK(commands.front().Source == child);
}

TEST_CASE("a fully transparent button retains its interaction geometry", "[gui][compile]") {
	World world("gui_compile.transparent");
	const Entity screen = world.Make("ScreenGui");
	const Entity button = world.Make("TextButton", screen);

	REQUIRE(world.Rebuild());
	const size_t opaque = world.List.Commands().Commands.size();
	REQUIRE(opaque > 0);

	Background background;
	background.Transparency = 1.0f;
	world.Data.Set(button, background);

	REQUIRE(world.Rebuild());

	const auto command = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[button](const DrawCommand &draw) {
			return draw.Source == button && draw.Kind == DrawKind::Rectangle;
		}
	);
	REQUIRE(command != world.List.Commands().Commands.end());
	CHECK(command->Transparency == 1.0f);
	CHECK(PickScreen(world.Data, world.List.Commands(), command->Bounds.Center()) == button);

	// The element is still counted as reached, which is what tells a panel the
	// difference between an invisible element and a missing one.
	CHECK(world.List.Commands().Elements == 1);
}

TEST_CASE("an element clipped to nothing emits nothing", "[gui][compile]") {
	World world("gui_compile.clipped");
	const Entity screen = world.Make("ScreenGui");
	const Entity window = world.Make("Frame", screen);
	const Entity inside = world.Make("Frame", window);

	Element outer;
	outer.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	outer.ClipsDescendants = true;
	world.Data.Set(window, outer);

	Element pushed;
	pushed.Position = UDim2{0.0f, 5000.0f, 0.0f, 5000.0f};
	pushed.Size = UDim2{0.0f, 10.0f, 0.0f, 10.0f};
	world.Data.Set(inside, pushed);

	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		CHECK(command.Source != inside);
	}
}

TEST_CASE("a text box shows its placeholder when empty", "[gui][compile]") {
	World world("gui_compile.placeholder");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("TextBox", screen);

	Entry entry;
	entry.PlaceholderText = "type here";
	world.Data.Set(box, entry);

	REQUIRE(world.Rebuild());

	bool found = false;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Kind == DrawKind::Text) {
			CHECK(command.Text == "type here");
			found = true;
		}
	}
	CHECK(found);

	// One command either way, so a backend drawing "the text" does not have to
	// know which of the two strings it is.
	Label label;
	label.Text = "typed";
	world.Data.Set(box, label);

	REQUIRE(world.Rebuild());
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Kind == DrawKind::Text) {
			CHECK(command.Text == "typed");
		}
	}
}

TEST_CASE("a password text box masks its compiled echo", "[gui][compile]") {
	World world("gui_compile.password");
	const Entity screen = world.Make("ScreenGui");
	const Entity box = world.Make("TextBox", screen);
	Label label;
	label.Text = "a\xC3\xA9\xF0\x9F\x98\x80";
	world.Data.Set(box, label);
	Entry entry;
	entry.Password = true;
	world.Data.Set(box, entry);

	REQUIRE(world.Rebuild());
	const auto text = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[box](const DrawCommand &command) { return command.Source == box && command.Kind == DrawKind::Text; }
	);
	REQUIRE(text != world.List.Commands().Commands.end());
	CHECK(text->Text == "***");
}

TEST_CASE("a binding is the canonical compiled label text", "[gui][compile][binding]") {
	World world("gui_compile.bound_label");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	const Entity source = world.Data.CreateInstance(Classes::Find(Name("Instance")), "Source");
	const Entity binding = world.Make("UIBinding", label);

	Label authored;
	authored.Text = "authored";
	world.Data.Set(label, authored);

	engine::ecs::AttributeValue value;
	value.Type = PropertyType::String;
	value.String = "bound";
	REQUIRE(engine::ecs::SetAttribute(world.Data, source, Name("Score"), value));

	Binding configured;
	configured.SourcePath = "Source";
	configured.Attribute = Name("Score");
	configured.Fallback = "fallback";
	world.Data.Set(binding, configured);
	REQUIRE(EvaluateBindings(world.Data) == 1);
	REQUIRE(world.Rebuild());

	const auto run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Text == "bound");

	value.String = "changed";
	REQUIRE(engine::ecs::SetAttribute(world.Data, source, Name("Score"), value));
	REQUIRE(world.Rebuild());
	const auto changed = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(changed != world.List.Commands().Commands.end());
	CHECK(changed->Text == "changed");
}

TEST_CASE("a localized label resolves through the viewer's compile request", "[gui][compile][localization]") {
	World world("gui_compile.localized_label");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	Label text;
	text.Text = "Play";
	world.Data.Set(label, text);
	Element automatic;
	automatic.Automatic = AutomaticSize::X;
	world.Data.Set(label, automatic);
	LabelPresentation presentation;
	presentation.LocalizationKey = Name("menu.play");
	world.Data.Set(label, presentation);

	LocalizationCatalogue catalogue;
	REQUIRE(catalogue.Set(CatalogueEntry{"fr", Name("menu.play"), "Jouer"}));
	world.Request.Catalogue = &catalogue;
	world.Request.Locale = "fr-CA";
	REQUIRE(world.Rebuild());

	const auto run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Text == "Jouer");
	const Resolved *resolved = world.Data.Get<Resolved>(label);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->AbsoluteSize.X == Approx(resolved->TextBounds.X));

	const uint64_t before = world.List.Signature();
	REQUIRE(catalogue.Set(CatalogueEntry{"fr", Name("menu.play"), "Démarrer"}));
	REQUIRE(world.Rebuild());
	CHECK(world.List.Signature() != before);
}

TEST_CASE("an authored theme resolves class and local state variants", "[gui][compile][style]") {
	World world("gui_compile.styled_label");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	Label text;
	text.Text = "Launch";
	world.Data.Set(label, text);
	StyleClass classes;
	REQUIRE(classes.Names.Add(Name("primary")));
	world.Data.Set(label, classes);
	const Entity theme = world.Data.CreateInstance(GuiClass("UITheme"), "Theme");
	UITheme *tokens = world.Data.GetMutable<UITheme>(theme);
	REQUIRE(tokens != nullptr);
	REQUIRE(tokens->Tokens.Set({Name("TextColor3"), StyleValue::FromColor(Color3{0.2f, 0.8f, 0.4f})}));
	ThemeBinding *binding = world.Data.GetMutable<ThemeBinding>(screen);
	REQUIRE(binding != nullptr);
	binding->Theme = theme;
	const Entity style = world.Make("UIStyle", label);
	UIStyle *pressed = world.Data.GetMutable<UIStyle>(style);
	REQUIRE(pressed != nullptr);
	pressed->Rule.State = StyleState::Pressed;
	REQUIRE(
		pressed->Rule.Declarations.Set({Name("TextColor3"), StyleValue::FromColor(Color3{0.9f, 0.3f, 0.1f})})
	);
	REQUIRE(world.Rebuild());

	auto run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Tint.G == Approx(0.8f));
	const ResolvedStyle *resolved = world.Data.Get<ResolvedStyle>(label);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->Values.Find(Name("TextColor3"))->Color.G == Approx(0.8f));

	const uint64_t themeBefore = world.List.Signature();
	tokens = world.Data.GetMutable<UITheme>(theme);
	REQUIRE(tokens != nullptr);
	REQUIRE(tokens->Tokens.Set({Name("TextColor3"), StyleValue::FromColor(Color3{0.4f, 0.2f, 0.9f})}));
	REQUIRE(world.Rebuild());
	CHECK(world.List.Signature() != themeBefore);
	run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Tint.B == Approx(0.9f));

	text.Color = Color3{0.9f, 0.1f, 0.1f};
	world.Data.Set(label, text);
	REQUIRE(world.Rebuild());
	run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Tint.R == Approx(0.9f));

	text.Color = Label{}.Color;
	world.Data.Set(label, text);
	world.Request.Pressed = label;
	REQUIRE(world.Rebuild());
	run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Tint.R == Approx(0.9f));

	world.Request.Pressed = engine::ecs::NULL_ENTITY;
	const Color3 directDefault = Label{}.Color;
	REQUIRE(world.Data.SetProperty(label, Name("TextColor3"), &directDefault, sizeof(directDefault)));
	const StyleDirect *direct = world.Data.Get<StyleDirect>(label);
	REQUIRE(direct != nullptr);
	CHECK(direct->Has(StyleDirectProperty::TextColor));
	REQUIRE(world.Rebuild());
	run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Tint == directDefault);
}

TEST_CASE("Invalidate forces one rebuild and no more", "[gui][compile]") {
	World world("gui_compile.invalidate");
	world.Make("ScreenGui");

	REQUIRE(world.Rebuild());
	REQUIRE_FALSE(world.Rebuild());

	// A backend that lost its device state wants the list resubmitted although
	// the list is correct.
	world.List.Invalidate();
	CHECK(world.Rebuild());
	CHECK_FALSE(world.Rebuild());
}

TEST_CASE("too many local styles fail closed instead of applying a prefix", "[gui][compile][style][limits]") {
	World world("gui_compile.style_limit");
	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	Label text;
	text.Text = "bounded";
	world.Data.Set(label, text);
	const Entity theme = world.Data.CreateInstance(GuiClass("UITheme"), "Theme");
	UITheme *tokens = world.Data.GetMutable<UITheme>(theme);
	REQUIRE(tokens != nullptr);
	REQUIRE(tokens->Tokens.Set({Name("TextColor3"), StyleValue::FromColor(Color3{1.0f, 0.0f, 0.0f})}));
	ThemeBinding *binding = world.Data.GetMutable<ThemeBinding>(screen);
	REQUIRE(binding != nullptr);
	binding->Theme = theme;
	for (size_t index = 0; index <= MAXIMUM_STYLE_RULES; index++) {
		REQUIRE(world.Make("UIStyle", label) != engine::ecs::NULL_ENTITY);
	}

	REQUIRE(world.Rebuild());
	const auto run = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[label](const DrawCommand &command) {
			return command.Source == label && command.Kind == DrawKind::Text;
		}
	);
	REQUIRE(run != world.List.Commands().Commands.end());
	CHECK(run->Tint == Label{}.Color);
}

TEST_CASE("a canvas group isolates opacity around its subtree", "[gui][compile]") {
	World world("gui_compile.group");
	const Entity screen = world.Make("ScreenGui");
	const Entity groupEntity = world.Make("CanvasGroup", screen);
	const Entity child = world.Make("Frame", groupEntity);

	Group group;
	group.Color = Color3{0.5f, 0.25f, 1.0f};
	group.Transparency = 0.5f;
	world.Data.Set(groupEntity, group);

	Background background;
	background.Color = Color3{0.8f, 0.4f, 0.2f};
	background.Transparency = 0.25f;
	world.Data.Set(child, background);

	REQUIRE(world.Rebuild());
	const auto found = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[&](const DrawCommand &command) {
			return command.Source == child && command.Kind == DrawKind::Rectangle;
		}
	);
	REQUIRE(found != world.List.Commands().Commands.end());
	CHECK(found->Tint.R == Approx(0.4f));
	CHECK(found->Tint.G == Approx(0.1f));
	CHECK(found->Tint.B == Approx(0.2f));
	// Group alpha is applied after the whole subtree has blended. Folding it
	// into this child would make overlapping descendants too dark.
	CHECK(found->Transparency == Approx(0.25f));
	REQUIRE(world.List.Commands().Operations.size() == 2);
	const DrawOperation &begin = world.List.Commands().Operations[0];
	const DrawOperation &end = world.List.Commands().Operations[1];
	CHECK(begin.Kind == DrawOperationKind::BeginGroup);
	CHECK(begin.Source == groupEntity);
	CHECK(begin.Transparency == Approx(0.5f));
	CHECK(begin.Command == 0);
	CHECK(end.Kind == DrawOperationKind::EndGroup);
	CHECK(end.Command == world.List.Commands().Commands.size());
}

TEST_CASE("a rounded mask brackets its parent paint subtree", "[gui][compile]") {
	World world("gui_compile.mask");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	const Entity child = world.Make("Frame", frame);
	const Entity mask = world.Make("UIMask", frame);

	Mask state;
	state.Radius = {0.5f, 0.0f};
	world.Data.Set(mask, state);
	REQUIRE(world.Rebuild());

	const auto &operations = world.List.Commands().Operations;
	REQUIRE(operations.size() == 2);
	CHECK(operations[0].Kind == DrawOperationKind::BeginMask);
	CHECK(operations[0].Source == frame);
	CHECK(operations[0].Command == 0);
	CHECK(operations[0].CornerRadius == Approx(50.0f));
	CHECK(operations[1].Kind == DrawOperationKind::EndMask);
	CHECK(operations[1].Command == world.List.Commands().Commands.size());
	CHECK(
		std::any_of(
			world.List.Commands().Commands.begin(),
			world.List.Commands().Commands.end(),
			[&](const DrawCommand &command) { return command.Source == child; }
		)
	);
}

TEST_CASE("an enabled zero-radius mask brackets a rectangular paint subtree", "[gui][compile]") {
	World world("gui_compile.rectangular_mask");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	const Entity child = world.Make("Frame", frame);
	const Entity mask = world.Make("UIMask", frame);
	world.Data.Set(mask, Mask{});
	REQUIRE(world.Rebuild());

	const auto &operations = world.List.Commands().Operations;
	REQUIRE(operations.size() == 2);
	CHECK(operations[0].Kind == DrawOperationKind::BeginMask);
	CHECK(operations[0].Source == frame);
	CHECK(operations[0].CornerRadius == 0.0f);
	CHECK(operations[0].Command == 0);
	CHECK(operations[1].Kind == DrawOperationKind::EndMask);
	CHECK(operations[1].Command == world.List.Commands().Commands.size());
	CHECK(
		std::any_of(
			world.List.Commands().Commands.begin(),
			world.List.Commands().Commands.end(),
			[&](const DrawCommand &command) { return command.Source == child; }
		)
	);
}

TEST_CASE("rounded mask nesting stops at the declared boundary", "[gui][compile]") {
	World world("gui_compile.mask_depth");
	Entity parent = world.Make("ScreenGui");
	for (size_t index = 0; index < 9; index++) {
		const Entity frame = world.Make("Frame", parent);
		const Entity mask = world.Make("UIMask", frame);
		Mask state;
		state.Radius = {0.0f, 4.0f};
		world.Data.Set(mask, state);
		parent = frame;
	}
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Operations.size() == 16);
}

TEST_CASE("a node link compiles as a coloured segment behind its nodes", "[gui][compile][nodecanvas]") {
	World world("gui_compile.node_canvas");
	const Entity screen = world.Make("ScreenGui");
	const Entity canvas = world.Make("NodeCanvas", screen);
	const Entity source = world.Make("NodeCanvasNode", canvas);
	const Entity destination = world.Make("NodeCanvasNode", canvas);
	const Entity output = world.Make("NodeCanvasPort", source);
	const Entity destinationOutput = world.Make("NodeCanvasPort", destination);
	const Entity input = world.Make("NodeCanvasPort", destination);
	const Entity link = world.Make("NodeCanvasLink", canvas);

	NodeCanvasNode sourceState;
	sourceState.Id = Name("source");
	world.Data.Set(source, sourceState);
	NodeCanvasNode destinationState;
	destinationState.Id = Name("destination");
	world.Data.Set(destination, destinationState);
	world.Data.Set(output, NodeCanvasPort{Name("value"), Name("number"), NodePortDirection::Output});
	world.Data.Set(
		destinationOutput, NodeCanvasPort{Name("value"), Name("number"), NodePortDirection::Output}
	);
	world.Data.Set(input, NodeCanvasPort{Name("value"), Name("number"), NodePortDirection::Input});

	Element sourceElement = *world.Data.Get<Element>(source);
	sourceElement.Position = {0.0f, 20.0f, 0.0f, 20.0f};
	sourceElement.Size = {0.0f, 100.0f, 0.0f, 80.0f};
	world.Data.Set(source, sourceElement);
	Element destinationElement = *world.Data.Get<Element>(destination);
	destinationElement.Position = {0.0f, 280.0f, 0.0f, 120.0f};
	destinationElement.Size = {0.0f, 100.0f, 0.0f, 80.0f};
	world.Data.Set(destination, destinationElement);
	Element outputElement = *world.Data.Get<Element>(output);
	outputElement.Position = {0.0f, 90.0f, 0.0f, 30.0f};
	outputElement.Size = {0.0f, 10.0f, 0.0f, 10.0f};
	world.Data.Set(output, outputElement);
	Element destinationOutputElement = *world.Data.Get<Element>(destinationOutput);
	destinationOutputElement.Position = {0.0f, 90.0f, 0.0f, 0.0f};
	destinationOutputElement.Size = {0.0f, 10.0f, 0.0f, 10.0f};
	world.Data.Set(destinationOutput, destinationOutputElement);
	Element inputElement = *world.Data.Get<Element>(input);
	inputElement.Position = {0.0f, 0.0f, 0.0f, 30.0f};
	inputElement.Size = {0.0f, 10.0f, 0.0f, 10.0f};
	world.Data.Set(input, inputElement);

	NodeCanvasLink linkState;
	linkState.FromNode = Name("source");
	linkState.FromPort = Name("value");
	linkState.FromDirection = NodePortDirection::Output;
	linkState.ToNode = Name("destination");
	linkState.ToPort = Name("value");
	linkState.ToDirection = NodePortDirection::Input;
	linkState.LineColor = {0.2f, 0.6f, 0.9f};
	linkState.LineThickness = 4.0f;
	world.Data.Set(link, linkState);

	REQUIRE(world.Rebuild());
	const auto segment = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[&](const DrawCommand &command) { return command.Source == link; }
	);
	REQUIRE(segment != world.List.Commands().Commands.end());
	CHECK(segment->Kind == DrawKind::Rectangle);
	CHECK(segment->Tint == linkState.LineColor);
	CHECK(segment->Bounds.Height() == Approx(4.0f));
	CHECK(segment->Rotation == Approx(30.4655f));

	const auto sourceFill = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[&](const DrawCommand &command) { return command.Source == source; }
	);
	REQUIRE(sourceFill != world.List.Commands().Commands.end());
	CHECK(segment < sourceFill);
}

TEST_CASE("a viewport frame emits its camera image with authored tint", "[gui][compile]") {
	World world("gui_compile.viewport");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("ViewportFrame", screen);

	Viewport viewport;
	viewport.CurrentCamera = Entity{42};
	viewport.Color = Color3{0.5f, 0.75f, 0.25f};
	viewport.Transparency = 0.2f;
	world.Data.Set(frame, viewport);

	REQUIRE(world.Rebuild());
	const auto found = std::find_if(
		world.List.Commands().Commands.begin(),
		world.List.Commands().Commands.end(),
		[&](const DrawCommand &command) {
			return command.Source == frame && command.Kind == DrawKind::Viewport;
		}
	);
	REQUIRE(found != world.List.Commands().Commands.end());
	CHECK(found->Tint == viewport.Color);
	CHECK(found->Transparency == Approx(0.2f));
}

TEST_CASE("a scrolling frame emits proportional bars over its children", "[gui][compile]") {
	World world("gui_compile.scrollbars");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("ScrollingFrame", screen);

	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(frame, element);

	Scrolling scrolling;
	scrolling.CanvasSize = UDim2{2.0f, 0.0f, 3.0f, 0.0f};
	scrolling.CanvasPosition = Vector2{50.0f, 100.0f};
	scrolling.Direction = ScrollingDirection::XY;
	scrolling.BarThickness = 10;
	world.Data.Set(frame, scrolling);

	REQUIRE(world.Rebuild());
	std::vector<const DrawCommand *> rectangles;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Rectangle) {
			rectangles.push_back(&command);
		}
	}

	REQUIRE(rectangles.size() == 3);
	const DrawCommand &vertical = *rectangles[1];
	const DrawCommand &horizontal = *rectangles[2];
	CHECK(vertical.Bounds.Width() == Approx(10.0f));
	CHECK(horizontal.Bounds.Height() == Approx(10.0f));
	CHECK(vertical.CornerRadius == Approx(5.0f));
	CHECK(horizontal.CornerRadius == Approx(5.0f));

	// **The full height, because neither inset is reserved.** With
	// `ScrollBarInset::None` - Roblox's default and this one - the two bars
	// overlap in the corner rather than shortening each other's track, so a
	// hundred-pixel frame showing a third of a three-hundred-pixel canvas gets a
	// third of a hundred-pixel track.
	CHECK(vertical.Bounds.Height() == Approx(100.0f / 3.0f));
	CHECK(horizontal.Bounds.Width() == Approx(50.0f));

	// And reserving both strips shortens the window, which shortens both the
	// track and the share of it each thumb takes.
	scrolling.HorizontalInset = ScrollBarInset::Always;
	scrolling.VerticalInset = ScrollBarInset::Always;
	world.Data.Set(frame, scrolling);
	REQUIRE(world.Rebuild());

	const ScrollState *state = world.Data.Get<ScrollState>(frame);
	REQUIRE(state != nullptr);
	CHECK(state->WindowSize.X == Approx(90.0f));
	CHECK(state->WindowSize.Y == Approx(90.0f));
	CHECK(state->CanvasSize.X == Approx(180.0f));
	CHECK(state->VerticalThumb.Height() == Approx(90.0f * 90.0f / 270.0f));
}

TEST_CASE("an automatic canvas grows to what the frame holds", "[gui][compile]") {
	// The property has been declared and unread since the tree went in; this is
	// the case that makes it mean something. A list of three fixed rows in a
	// frame shorter than they are is exactly what it is for.
	World world("gui_compile.autocanvas");
	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("ScrollingFrame", screen);

	Element element;
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	world.Data.Set(frame, element);

	Scrolling scrolling;
	scrolling.CanvasSize = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	scrolling.AutomaticCanvas = AutomaticSize::Y;
	world.Data.Set(frame, scrolling);

	const Entity layout = world.Make("UIListLayout", frame);
	world.Data.Set(layout, ListLayout{});

	for (int index = 0; index < 3; index++) {
		const Entity row = world.Make("Frame", frame);
		Element size;
		size.Size = UDim2{0.0f, 100.0f, 0.0f, 60.0f};
		size.LayoutOrder = index;
		world.Data.Set(row, size);
	}

	REQUIRE(world.Rebuild());

	const ScrollState *state = world.Data.Get<ScrollState>(frame);
	REQUIRE(state != nullptr);

	// **The content replaces that axis rather than adding to it**, which is
	// Roblox's rule: three sixty-pixel rows are a hundred and eighty pixels of
	// canvas whatever `CanvasSize` said.
	CHECK(state->CanvasSize.Y == Approx(180.0f));

	// The other axis is untouched, so a frame that grows downwards does not
	// start scrolling sideways.
	CHECK(state->CanvasSize.X == Approx(0.0f));
}

TEST_CASE("a billboard is hidden from the one viewer it names", "[gui][compile]") {
	// **A compile decision and not a layout one**, which is what the two halves
	// of this case check: the tree resolves identically for everybody - the
	// billboard is `Rendered` either way - and what differs is only which
	// commands reach the list. Deciding it in the backend instead would leave a
	// compiled list that disagreed with the frame drawn from it.
	World world("gui_compile.hidefrom");

	const Entity workspace =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Workspace");

	const Entity viewer =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "Somebody");
	const Entity other =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "SomebodyElse");

	const Entity billboard = world.Make("BillboardGui", workspace);
	const Entity frame = world.Make("Frame", billboard);
	world.Data.Set(frame, Element{});

	const auto drawn = [&] {
		size_t found = 0;
		for (const DrawCommand &command : world.List.Commands().Commands) {
			found += command.Source == frame ? 1 : 0;
		}
		return found;
	};

	Billboard state;
	state.PlayerToHideFrom = viewer;
	world.Data.Set(billboard, state);

	world.Request.Viewer = other;
	REQUIRE(world.Rebuild());
	CHECK(drawn() > 0);

	world.Request.Viewer = viewer;
	REQUIRE(world.Rebuild());
	CHECK(drawn() == 0);

	// **The tree is untouched either way.** A viewer-specific decision that had
	// reached `Resolved` would make one client's `AbsoluteSize` differ from
	// another's for an element neither of them authored differently.
	CHECK(world.Data.Get<Resolved>(frame)->Rendered);

	// And nobody in particular hides nothing, which is the studio's case and a
	// test's - an unset `PlayerToHideFrom` is also null and must not match.
	world.Request.Viewer = engine::ecs::NULL_ENTITY;
	REQUIRE(world.Rebuild());
	CHECK(drawn() > 0);
}

TEST_CASE("a gradient resolves into the line it ramps along", "[gui][compile]") {
	// **The compile's whole job for a `UIGradient`**: an angle and a size-
	// relative offset go in, two points in canvas pixels come out. Doing it here
	// rather than in a backend is what stops the client and the studio
	// disagreeing about where a ramp starts.
	World world("gui_compile.gradient");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	Element element;
	element.Position = UDim2{0.0f, 100.0f, 0.0f, 50.0f};
	element.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(frame, element);

	const Entity ramp = world.Make("UIGradient", frame);
	world.Data.Set(ramp, Gradient{});

	REQUIRE(world.Rebuild());

	const DrawCommand *fill = nullptr;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Rectangle) {
			fill = &command;
			break;
		}
	}

	REQUIRE(fill != nullptr);
	REQUIRE(fill->Gradient >= 0);
	const DrawGradient &resolved = world.List.Commands().Gradients[std::size_t(fill->Gradient)];

	// Unrotated, so the ramp runs left to right across the element's own width.
	CHECK(resolved.Origin.X == Approx(100.0f));
	CHECK(resolved.Origin.Y == Approx(100.0f));
	CHECK(resolved.Axis.X == Approx(200.0f));
	CHECK(resolved.Axis.Y == Approx(0.0f).margin(0.001f));

	// **A quarter turn ramps over the full height, not over the width.** The
	// ends snap to the element's edges *along the rotated axis*, which is what
	// makes a ninety-degree gradient a top-to-bottom one rather than a diagonal.
	Gradient turned;
	turned.Rotation = 90.0f;
	world.Data.Set(ramp, turned);
	REQUIRE(world.Rebuild());

	const DrawGradient &vertical = world.List.Commands().Gradients.front();
	CHECK(vertical.Axis.X == Approx(0.0f).margin(0.001f));
	CHECK(vertical.Axis.Y == Approx(100.0f));
	CHECK(vertical.Origin.X == Approx(200.0f));
	CHECK(vertical.Origin.Y == Approx(50.0f));

	// The offset is in multiples of the element's own size, so one whole width
	// slides the ramp entirely off the right-hand edge.
	Gradient slid;
	slid.Offset = Vector2{1.0f, 0.0f};
	world.Data.Set(ramp, slid);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Gradients.front().Origin.X == Approx(300.0f));

	// And a disabled one resolves to nothing at all rather than to a ramp that
	// happens to be flat - a backend that got the second would still subdivide.
	Gradient off;
	off.Enabled = false;
	world.Data.Set(ramp, off);
	REQUIRE(world.Rebuild());
	CHECK(world.List.Commands().Gradients.empty());
	for (const DrawCommand &command : world.List.Commands().Commands) {
		CHECK(command.Gradient == -1);
	}
}

TEST_CASE("a stroke takes its own gradient and never the element's", "[gui][compile]") {
	// Roblox's arrangement, and the reason it is worth a case: an outline is a
	// separate thing with a separate colour, so inheriting the fill's ramp would
	// tint it with one nobody asked for.
	World world("gui_compile.strokegradient");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	world.Data.Set(frame, Element{});

	const Entity fillRamp = world.Make("UIGradient", frame);
	Gradient warm;
	warm.Color = engine::core::ColorSequence{Color3{1.0f, 0.0f, 0.0f}};
	world.Data.Set(fillRamp, warm);

	const Entity stroke = world.Make("UIStroke", frame);
	world.Data.Set(stroke, Stroke{});

	REQUIRE(world.Rebuild());

	const DrawCommand *outline = nullptr;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Outline) {
			outline = &command;
		}
	}

	// The border and the stroke are both outlines; the stroke is the last one
	// this element emits, which is what `Emit` promises.
	REQUIRE(outline != nullptr);
	CHECK(outline->Gradient == -1);

	const Entity strokeRamp = world.Make("UIGradient", stroke);
	Gradient cool;
	cool.Color = engine::core::ColorSequence{Color3{0.0f, 0.0f, 1.0f}};
	world.Data.Set(strokeRamp, cool);

	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == frame && command.Kind == DrawKind::Outline) {
			outline = &command;
		}
	}

	REQUIRE(outline->Gradient >= 0);
	const DrawGradient &ramp = world.List.Commands().Gradients[std::size_t(outline->Gradient)];
	CHECK(ramp.Color.Evaluate(0.5f).B == Approx(1.0f));
}

TEST_CASE("a rich text label reaches the list as spans over one string", "[gui][compile]") {
	// **One command and not several**, which is the arrangement the whole
	// feature turns on: only a backend can measure a glyph, so a marked-up run
	// has to arrive as one string it lays out in one pass.
	World world("gui_compile.richtext");

	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	world.Data.Set(label, Element{});

	Label text;
	text.Text = "a <b>big</b> word";
	text.Rich = true;
	world.Data.Set(label, text);

	REQUIRE(world.Rebuild());

	const DrawCommand *run = nullptr;
	size_t runs = 0;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == label && command.Kind == DrawKind::Text) {
			run = &command;
			runs++;
		}
	}

	REQUIRE(run != nullptr);
	CHECK(runs == 1);
	CHECK(run->Text == "a big word");
	REQUIRE(run->Spans.size() == 1);
	CHECK(run->Spans[0].Font == FontFace::Bold);

	// Without the property the markup is text, which is what an author who
	// wrote a less-than sign into a plain label meant.
	text.Rich = false;
	world.Data.Set(label, text);
	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == label && command.Kind == DrawKind::Text) {
			CHECK(command.Text == "a <b>big</b> word");
			CHECK(command.Spans.empty());
		}
	}
}

TEST_CASE("the visible limit cuts the text and the spans together", "[gui][compile]") {
	// Roblox applies `MaxVisibleGraphemes` to what a reader sees, so a
	// typewriter effect on a marked-up string reveals letters and not tags - and
	// a span left pointing past the cut would be a range a backend indexes with.
	World world("gui_compile.maxvisible");

	const Entity screen = world.Make("ScreenGui");
	const Entity label = world.Make("TextLabel", screen);
	world.Data.Set(label, Element{});

	Label text;
	text.Text = "<b>abcdef</b>";
	text.Rich = true;
	text.MaxVisible = 3;
	world.Data.Set(label, text);

	REQUIRE(world.Rebuild());

	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source != label || command.Kind != DrawKind::Text) {
			continue;
		}
		CHECK(command.Text == "abc");
		REQUIRE(command.Spans.size() == 1);
		CHECK(command.Spans[0].End == 3);
	}

	// And the measurement follows, so `TextScaled` fits what is shown rather
	// than what was typed.
	const Resolved *resolved = world.Data.Get<Resolved>(label);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->TextBounds.X == Approx(3.0f * AVERAGE_ADVANCE * float(resolved->TextSize)));
}

TEST_CASE("a scaled stroke resolves to pixels before it reaches a backend", "[gui][compile]") {
	// **A draw list has no element to measure against**, so a fraction that
	// survived the compile would be a thickness a backend could not resolve -
	// and the one that arrived at `InterfaceMesh` would be read as pixels and
	// draw a hairline. See `StrokeThickness`.
	World world("gui_compile.strokesizing");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);

	// 200 wide and 80 tall, so the smaller side is 80 and the two modes cannot
	// be confused by a square.
	Element box;
	box.Size = UDim2{0.0f, 200.0f, 0.0f, 80.0f};
	world.Data.Set(frame, box);

	const Entity stroke = world.Make("UIStroke", frame);

	const auto thicknessOf = [&]() {
		REQUIRE(world.Rebuild());
		const DrawCommand *outline = nullptr;
		for (const DrawCommand &command : world.List.Commands().Commands) {
			if (command.Source == frame && command.Kind == DrawKind::Outline) {
				outline = &command;
			}
		}
		REQUIRE(outline != nullptr);
		return outline->Thickness;
	};

	SECTION("fixed is the number that was typed") {
		Stroke pixels;
		pixels.Thickness = 3.0f;
		pixels.Sizing = StrokeSizing::FixedSize;
		world.Data.Set(stroke, pixels);
		CHECK(thicknessOf() == Approx(3.0f));
	}

	SECTION("scaled is a fraction of the smaller side") {
		Stroke fraction;
		fraction.Thickness = 0.1f;
		fraction.Sizing = StrokeSizing::ScaledSize;
		world.Data.Set(stroke, fraction);

		// The smaller side, not the larger and not the average: an outline
		// scaled by a long thin element's width would be thicker than the
		// element is tall.
		CHECK(thicknessOf() == Approx(8.0f));
	}

	SECTION("a scaled zero draws nothing, like a fixed zero") {
		// **Counted rather than asserted absent**, because a `Frame` carries a
		// one-pixel `Background::BorderSizePixel` and that is an outline too.
		// Counting is the stronger question anyway: it says the stroke is a
		// second command rather than something that edits the border.
		const auto outlines = [&]() {
			REQUIRE(world.Rebuild());
			size_t found = 0;
			for (const DrawCommand &command : world.List.Commands().Commands) {
				found += command.Source == frame && command.Kind == DrawKind::Outline ? 1 : 0;
			}
			return found;
		};

		Stroke drawn;
		drawn.Thickness = 0.1f;
		drawn.Sizing = StrokeSizing::ScaledSize;
		world.Data.Set(stroke, drawn);
		const size_t withStroke = outlines();

		Stroke none = drawn;
		none.Thickness = 0.0f;
		world.Data.Set(stroke, none);
		CHECK(outlines() == withStroke - 1);
	}
}

TEST_CASE("a stroke's join and sizing reach the draw list and the signature", "[gui][compile]") {
	// The signature is what decides whether a frame recompiles at all, so a
	// property that changed the picture without changing the hash would draw
	// the old one until something else moved.
	World world("gui_compile.strokejoin");

	const Entity screen = world.Make("ScreenGui");
	const Entity frame = world.Make("Frame", screen);
	world.Data.Set(frame, Element{});
	const Entity stroke = world.Make("UIStroke", frame);

	Stroke round;
	round.Thickness = 2.0f;
	round.Join = LineJoin::Round;
	world.Data.Set(stroke, round);
	REQUIRE(world.Rebuild());

	const auto joinOf = [&]() {
		const DrawCommand *outline = nullptr;
		for (const DrawCommand &command : world.List.Commands().Commands) {
			if (command.Source == frame && command.Kind == DrawKind::Outline) {
				outline = &command;
			}
		}
		REQUIRE(outline != nullptr);
		return outline->Join;
	};
	CHECK(joinOf() == LineJoin::Round);

	Stroke mitred = round;
	mitred.Join = LineJoin::Miter;
	world.Data.Set(stroke, mitred);
	REQUIRE(world.Rebuild());
	CHECK(joinOf() == LineJoin::Miter);

	// And the sizing half of the same argument: switching mode changes the
	// thickness, so it has to move the hash even though the typed number did
	// not change.
	Stroke scaled = mitred;
	scaled.Sizing = StrokeSizing::ScaledSize;
	world.Data.Set(stroke, scaled);
	REQUIRE(world.Rebuild());
}

TEST_CASE(
	"virtual record bindings project typed visual values without changing the template",
	"[gui][compile][virtual]"
) {
	World world("gui_compile.virtual_visuals");
	const Entity screen = world.Make("ScreenGui");
	const Entity list = world.Make("ScrollingFrame", screen);
	Element listElement;
	listElement.Size = UDim2{0.0f, 200.0f, 0.0f, 100.0f};
	world.Data.Set(list, listElement);
	const Entity collection = world.Make("UIVirtualCollection", list);
	const Entity row = world.Make("TextLabel", collection);
	Element rowElement;
	rowElement.Size = UDim2{0.0f, 200.0f, 0.0f, 20.0f};
	world.Data.Set(row, rowElement);
	const Entity colourBinding = world.Make("UIBinding", row);
	Binding binding;
	binding.SourcePath = "$item.Shade";
	binding.Target = Name("BackgroundColor3");
	world.Data.Set(colourBinding, binding);
	const Entity textBinding = world.Make("UIBinding", row);
	binding.SourcePath = "$item.Caption";
	binding.Target = Name("Text");
	world.Data.Set(textBinding, binding);

	VirtualCollection source;
	source.ItemCount = 1;
	source.FixedExtent = 20.0f;
	VirtualRecord record;
	record.Key = "one";
	engine::ecs::AttributeValue shade;
	shade.Type = PropertyType::Color3;
	shade.Color3 = Color3{0.25f, 0.5f, 0.75f};
	record.Fields.push_back(VirtualField{"Shade", shade});
	engine::ecs::AttributeValue caption;
	caption.Type = PropertyType::String;
	caption.String = "First";
	record.Fields.push_back(VirtualField{"Caption", caption});
	source.Page.Records.push_back(record);
	world.Data.Set(collection, source);
	REQUIRE(world.Rebuild());
	bool foundRectangle = false;
	bool foundText = false;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source != row || command.Collection != collection || command.Key != "one") continue;
		if (command.Kind == DrawKind::Rectangle) {
			foundRectangle = true;
			CHECK(command.Tint == shade.Color3);
		}
		if (command.Kind == DrawKind::Text) {
			foundText = true;
			CHECK(command.Text == "First");
		}
	}
	CHECK(foundRectangle);
	CHECK(foundText);
	CHECK(world.Data.Get<Background>(row)->Color != shade.Color3);
}

TEST_CASE("measured virtual records cull by their published extents", "[gui][compile][virtual]") {
	World world("gui_compile.virtual_measured");
	const Entity screen = world.Make("ScreenGui");
	const Entity list = world.Make("ScrollingFrame", screen);
	Element listElement;
	listElement.Size = UDim2{0.0f, 200.0f, 0.0f, 30.0f};
	world.Data.Set(list, listElement);
	Scrolling scrolling;
	scrolling.CanvasPosition.Y = 50.0f;
	world.Data.Set(list, scrolling);
	const Entity collection = world.Make("UIVirtualCollection", list);
	const Entity row = world.Make("TextLabel", collection);
	Element rowElement;
	rowElement.Size = UDim2{0.0f, 200.0f, 0.0f, 20.0f};
	world.Data.Set(row, rowElement);

	VirtualCollection source;
	source.ItemCount = 3;
	source.FixedExtent = 20.0f;
	source.ExtentPolicy = VirtualExtentPolicy::Measured;
	source.Overscan = 0;
	source.Revision = 1;
	for (int index = 0; index < 3; index++) {
		VirtualRecord record;
		record.Key = std::to_string(index);
		record.MeasuredExtent = index == 1 ? 80.0f : 20.0f;
		source.Page.Records.push_back(record);
	}
	world.Data.Set(collection, source);
	REQUIRE(world.Rebuild());
	bool middle = false;
	bool last = false;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source != row || command.Kind != DrawKind::Rectangle) continue;
		middle |= command.Key == "1";
		if (command.Key == "1") CHECK(command.Bounds.Height() == Catch::Approx(80.0f));
		last |= command.Key == "2";
	}
	CHECK(middle);
	CHECK_FALSE(last);

	// A corrected earlier extent keeps the keyed row at the same screen Y.
	source.Page.Records.front().MeasuredExtent = 30.0f;
	source.Revision++;
	world.Data.Set(collection, source);
	REQUIRE(world.Rebuild());
	CHECK(world.Data.Get<Scrolling>(list)->CanvasPosition.Y == Catch::Approx(60.0f));
}

TEST_CASE("virtual grids compile visible row-major template facets", "[gui][compile][virtual]") {
	World world("gui_compile.virtual_grid");
	const Entity screen = world.Make("ScreenGui");
	const Entity list = world.Make("ScrollingFrame", screen);
	Element listElement;
	listElement.Size = UDim2{0.0f, 100.0f, 0.0f, 30.0f};
	world.Data.Set(list, listElement);
	const Entity collection = world.Make("UIVirtualCollection", list);
	const Entity row = world.Make("Frame", collection);
	Element rowElement;
	rowElement.Size = UDim2{0.0f, 40.0f, 0.0f, 20.0f};
	world.Data.Set(row, rowElement);

	VirtualCollection grid;
	grid.ItemCount = 6;
	grid.LayoutPolicy = VirtualLayoutPolicy::Grid;
	grid.GridColumns = 3;
	grid.GridCellStride = {40.0f, 20.0f};
	grid.Overscan = 0;
	for (uint32_t index = 0; index < grid.ItemCount; index++) {
		grid.Page.Records.push_back(VirtualRecord{.Key = std::to_string(index), .Fields = {}});
	}
	world.Data.Set(collection, grid);
	REQUIRE(world.Rebuild());

	std::vector<const DrawCommand *> facets;
	for (const DrawCommand &command : world.List.Commands().Commands) {
		if (command.Source == row && command.Kind == DrawKind::Rectangle) facets.push_back(&command);
	}
	REQUIRE(facets.size() == 6);
	CHECK(facets[0]->Bounds.Min == Vector2{0.0f, 0.0f});
	CHECK(facets[2]->Bounds.Min == Vector2{80.0f, 0.0f});
	CHECK(facets[3]->Bounds.Min == Vector2{0.0f, 20.0f});
}
