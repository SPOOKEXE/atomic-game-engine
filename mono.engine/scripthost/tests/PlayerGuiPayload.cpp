#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/PlayerGui.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scripthost.playerguipayload")

TEST_CASE("server graph inputs inside retained GUI reach the same local graph", "[playergui][imagegraph]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store authority("gui.graph-authority"), replica("gui.graph-replica");
	scene::InstallServices(authority);
	const auto player = scene::AddPlayer(authority, "Ada");
	const auto secondPlayer = scene::AddPlayer(authority, "Grace");
	const auto screen = authority.CreateInstance(gui::GuiClass("ScreenGui"), "Persistent");
	authority.SetParent(screen, authority.FindFirstRoot("StarterGui"));
	authority.GetMutable<gui::Layer>(screen)->ResetOnSpawn = false;
	const auto graph = authority.CreateInstance(scene::ImageGraphClass(), "Glow");
	authority.SetParent(graph, screen);
	REQUIRE(scene::SetImageGraphInstanceKey(authority, graph, core::Name("hud-glow")));
	REQUIRE(scene::SetImageGraphAsset(authority, graph, core::Name("hud.aimagegraph")));
	scene::ImageGraphInput input;
	input.Name = core::Name("strength");
	input.Number = 0.25;
	REQUIRE(scene::SetImageGraphInput(authority, graph, input));
	const auto label = authority.CreateInstance(gui::GuiClass("ImageLabel"), "GlowImage");
	authority.SetParent(label, screen);
	gui::Picture picture;
	picture.Image = scene::ImageGraphContentName(authority, graph);
	authority.Set(label, picture);
	REQUIRE(script::ResetPlayerGui(authority, player) == 1);
	REQUIRE(script::ResetPlayerGui(authority, secondPlayer) == 1);
	const auto sourceRoot =
		authority.FindFirstChild(authority.FindFirstChild(player, "PlayerGui"), "Persistent");
	const auto source = authority.FindFirstChild(sourceRoot, "Glow");
	const auto secondRoot =
		authority.FindFirstChild(authority.FindFirstChild(secondPlayer, "PlayerGui"), "Persistent");
	const auto secondSource = authority.FindFirstChild(secondRoot, "Glow");
	const auto firstKey = authority.Get<scene::ImageGraph>(source)->InstanceKey;
	const auto secondKey = authority.Get<scene::ImageGraph>(secondSource)->InstanceKey;
	REQUIRE(firstKey.IsValid());
	REQUIRE(secondKey.IsValid());
	CHECK(firstKey != secondKey);
	CHECK(firstKey != core::Name("hud-glow"));
	CHECK(scene::ImageGraphContentName(authority, source).IsValid());
	CHECK(scene::ImageGraphContentName(authority, secondSource).IsValid());
	const auto copiedLabel = authority.FindFirstChild(sourceRoot, "GlowImage");
	const auto copiedPicture = authority.Get<gui::Picture>(copiedLabel);
	REQUIRE(copiedPicture != nullptr);
	CHECK(copiedPicture->Image == scene::ImageGraphContentName(authority, source));
	const auto secondLabel = authority.FindFirstChild(secondRoot, "GlowImage");
	const auto secondPicture = authority.Get<gui::Picture>(secondLabel);
	REQUIRE(secondPicture != nullptr);
	CHECK(secondPicture->Image == scene::ImageGraphContentName(authority, secondSource));
	replica.SetAdoptOnly(true);
	const auto adopt = [&] {
		core::ByteWriter writer;
		REQUIRE(authority.Save(writer));
		core::ByteReader reader(writer.Bytes());
		REQUIRE(replica.Apply(reader, ecs::ApplyMode::Authoritative));
		(void)gui::RefreshPlayerGuiProjection(replica, player, ecs::NULL_ENTITY);
		(void)gui::RefreshPlayerGuiProjection(replica, secondPlayer, ecs::NULL_ENTITY);
	};
	adopt();
	const auto copy = gui::FindPlayerGuiCopy(replica, source);
	REQUIRE(copy != ecs::NULL_ENTITY);
	CHECK(ecs::Store::IsPredicted(copy));
	const auto secondCopy = gui::FindPlayerGuiCopy(replica, secondSource);
	REQUIRE(secondCopy != ecs::NULL_ENTITY);
	CHECK(scene::ImageGraphContentName(replica, copy).IsValid());
	CHECK(scene::ImageGraphContentName(replica, secondCopy).IsValid());
	CHECK(replica.Get<scene::ImageGraph>(copy)->InstanceKey == firstKey);
	CHECK(replica.Get<scene::ImageGraph>(secondCopy)->InstanceKey == secondKey);
	const auto localLabel = gui::FindPlayerGuiCopy(replica, copiedLabel);
	REQUIRE(localLabel != ecs::NULL_ENTITY);
	const auto localPicture = replica.Get<gui::Picture>(localLabel);
	REQUIRE(localPicture != nullptr);
	CHECK(localPicture->Image == scene::ImageGraphContentName(replica, copy));
	const auto localSecondLabel = gui::FindPlayerGuiCopy(replica, secondLabel);
	REQUIRE(localSecondLabel != ecs::NULL_ENTITY);
	const auto localSecondPicture = replica.Get<gui::Picture>(localSecondLabel);
	REQUIRE(localSecondPicture != nullptr);
	CHECK(localSecondPicture->Image == scene::ImageGraphContentName(replica, secondCopy));
	scene::ImageGraphInput observed;
	REQUIRE(scene::GetImageGraphInput(replica, copy, input.Name, observed));
	CHECK(observed.Number == 0.25);
	input.Number = 0.75;
	REQUIRE(scene::SetImageGraphInput(authority, source, input));
	REQUIRE(scene::SetImageGraphAsset(authority, source, core::Name("hud-updated.aimagegraph")));
	adopt();
	CHECK(gui::FindPlayerGuiCopy(replica, source) == copy);
	REQUIRE(scene::GetImageGraphInput(replica, copy, input.Name, observed));
	CHECK(observed.Number == 0.75);
	CHECK(replica.Get<scene::ImageGraph>(copy)->Graph == core::Name("hud-updated.aimagegraph"));
	CHECK(replica.Get<scene::ImageGraph>(copy)->InstanceKey == firstKey);
	CHECK(scene::ImageGraphContentName(replica, copy).IsValid());
}

TEST_CASE(
	"retained GUI attributes follow authored changes without erasing local values", "[playergui][attributes]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store authority("gui.attribute-authority"), replica("gui.attribute-replica");
	scene::InstallServices(authority);
	const auto player = scene::AddPlayer(authority, "Ada");
	const auto screen = authority.CreateInstance(gui::GuiClass("ScreenGui"), "Persistent");
	authority.SetParent(screen, authority.FindFirstRoot("StarterGui"));
	authority.GetMutable<gui::Layer>(screen)->ResetOnSpawn = false;
	REQUIRE(gui::ResetPlayerGui(authority, player) == 1);
	const auto source = authority.FindFirstChild(authority.FindFirstChild(player, "PlayerGui"), "Persistent");
	const core::Name theme("theme"), callback("client-callback"), removed("temporary");
	const auto text = [](const char *value) {
		ecs::AttributeValue attribute;
		attribute.Type = ecs::PropertyType::String;
		attribute.String = value;
		return attribute;
	};
	REQUIRE(ecs::SetAttribute(authority, source, theme, text("server-first")));
	REQUIRE(ecs::SetAttribute(authority, source, removed, text("present")));
	replica.SetAdoptOnly(true);
	const auto adopt = [&] {
		core::ByteWriter writer;
		REQUIRE(authority.Save(writer));
		core::ByteReader reader(writer.Bytes());
		REQUIRE(replica.Apply(reader, ecs::ApplyMode::Authoritative));
		(void)gui::RefreshPlayerGuiProjection(replica, player, ecs::NULL_ENTITY);
	};
	adopt();
	const auto copy = gui::FindPlayerGuiCopy(replica, source);
	REQUIRE(copy != ecs::NULL_ENTITY);
	ecs::AttributeValue observed;
	REQUIRE(ecs::GetAttribute(replica, copy, theme, observed));
	CHECK(observed.String == "server-first");
	REQUIRE(ecs::SetAttribute(replica, copy, theme, text("local-choice")));
	REQUIRE(ecs::SetAttribute(replica, copy, callback, text("still-connected")));
	adopt();
	REQUIRE(ecs::GetAttribute(replica, copy, theme, observed));
	CHECK(observed.String == "local-choice");
	REQUIRE(ecs::GetAttribute(replica, copy, callback, observed));
	CHECK(observed.String == "still-connected");
	REQUIRE(ecs::SetAttribute(authority, source, theme, text("server-second")));
	REQUIRE(ecs::SetAttribute(authority, source, removed, {}));
	adopt();
	CHECK(gui::FindPlayerGuiCopy(replica, source) == copy);
	REQUIRE(ecs::GetAttribute(replica, copy, theme, observed));
	CHECK(observed.String == "server-second");
	CHECK_FALSE(ecs::GetAttribute(replica, copy, removed, observed));
	REQUIRE(ecs::GetAttribute(replica, copy, callback, observed));
	CHECK(observed.String == "still-connected");
}

TEST_CASE(
	"GUI image caching preserves local pixels and observes native source edits", "[playergui][editableimage]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store authority("gui.pixel-authority"), replica("gui.pixel-replica");
	scene::InstallServices(authority);
	const auto player = scene::AddPlayer(authority, "Ada");
	const auto root = authority.CreateInstance(gui::GuiClass("ScreenGui"), "Persistent");
	authority.SetParent(root, authority.FindFirstChild(player, "PlayerGui"));
	const auto image = authority.CreateInstance(scene::EditableImageClass(), "Pixels");
	authority.SetParent(image, root);
	auto *sourcePixels = authority.GetMutable<scene::EditableImage>(image);
	sourcePixels->Width = 2;
	sourcePixels->Height = 2;
	sourcePixels->Pixels.assign(16, uint8_t{0x20});
	replica.SetAdoptOnly(true);
	const auto refresh = [&] { (void)gui::RefreshPlayerGuiProjection(replica, player, ecs::NULL_ENTITY); };
	const auto adopt = [&] {
		core::ByteWriter writer;
		REQUIRE(authority.Save(writer));
		core::ByteReader reader(writer.Bytes());
		REQUIRE(replica.Apply(reader, ecs::ApplyMode::Authoritative));
		refresh();
	};
	adopt();
	const auto copy = gui::FindPlayerGuiCopy(replica, image);
	REQUIRE(copy != ecs::NULL_ENTITY);
	CHECK(replica.Get<scene::EditableImage>(copy)->Pixels.front() == 0x20);
	replica.GetMutable<scene::EditableImage>(copy)->Pixels.front() = 0xA0;
	refresh();
	CHECK(replica.Get<scene::EditableImage>(copy)->Pixels.front() == 0xA0);
	const auto stable = replica.ChangeVersion();
	refresh();
	CHECK(replica.ChangeVersion() == stable);
	// Native mutable writes must invalidate even without an image API revision bump.
	replica.GetMutable<scene::EditableImage>(image)->Pixels.front() = 0x40;
	refresh();
	CHECK(gui::FindPlayerGuiCopy(replica, image) == copy);
	CHECK(replica.Get<scene::EditableImage>(copy)->Pixels.front() == 0x40);
	authority.GetMutable<scene::EditableImage>(image)->Pixels.front() = 0x60;
	adopt();
	CHECK(replica.Get<scene::EditableImage>(copy)->Pixels.front() == 0x60);
	replica.GetMutable<scene::EditableImage>(copy)->Pixels.front() = 0xB0;
	adopt();
	CHECK(replica.Get<scene::EditableImage>(copy)->Pixels.front() == 0xB0);
}
