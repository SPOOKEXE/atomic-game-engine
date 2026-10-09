#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.gui.playergui")

namespace {
	using namespace engine;

	struct GuiWorld {
		ecs::Store Authority{"gui.authority"};
		ecs::Store Replica{"gui.replica"};
		ecs::Entity Player;
		ecs::Entity Target;
		ecs::Entity Template;
		ecs::Entity Character;

		GuiWorld() {
			gui::RegisterGuiClasses();
			const auto instance = ecs::Classes::Find(core::Name("Instance"));
			const auto starter = Authority.CreateInstance(instance, "StarterGui");
			Player = Authority.CreateInstance(instance, "Ada");
			Target = Authority.CreateInstance(instance, "PlayerGui");
			Authority.SetParent(Target, Player);
			Template = Authority.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
			Authority.SetParent(Template, starter);
			const auto label = Authority.CreateInstance(gui::GuiClass("TextLabel"), "Status");
			Authority.SetParent(label, Template);
			Authority.GetMutable<gui::Label>(label)->Text = "template";
			Character = Authority.Create();
			REQUIRE(gui::ResetPlayerGui(Authority, Player) == 1);
			Replica.SetAdoptOnly(true);
			Adopt();
		}

		void Adopt() {
			core::ByteWriter writer;
			REQUIRE(Authority.Save(writer));
			core::ByteReader reader(writer.Bytes());
			REQUIRE(Replica.Apply(reader, ecs::ApplyMode::Authoritative));
			(void)gui::RefreshPlayerGuiProjection(Replica, Player, Character);
		}

		ecs::Entity Source() const {
			return Authority.FindFirstChild(Target, "Hud");
		}
		ecs::Entity Copy() const {
			return gui::FindPlayerGuiCopy(Replica, Source());
		}
	};
}

TEST_CASE("local GUI state survives unchanged snapshots and accepts new server values", "[gui][playergui]") {
	using namespace engine;
	GuiWorld world;
	const auto copy = world.Copy();
	REQUIRE(copy != ecs::NULL_ENTITY);
	CHECK(ecs::Store::IsPredicted(copy));
	CHECK(ecs::IsClientLocalInstance(world.Replica, copy));
	CHECK(gui::IsPlayerGuiSource(world.Replica, world.Source()));
	CHECK_FALSE(gui::IsPlayerGuiSource(world.Replica, copy));
	const auto sourceLabel = world.Authority.FindFirstChild(world.Source(), "Status");
	const auto label = world.Replica.FindFirstChild(copy, "Status");
	world.Replica.GetMutable<gui::Label>(label)->Text = "local-state";
	const auto epoch = world.Replica.ChangeVersion();
	CHECK(gui::RefreshPlayerGuiProjection(world.Replica, world.Player, world.Character) == 0);
	CHECK(world.Replica.ChangeVersion() == epoch);
	for (int snapshot = 0; snapshot < 3; ++snapshot) {
		world.Adopt();
		CHECK(world.Copy() == copy);
		CHECK(world.Replica.Get<gui::Label>(label)->Text == "local-state");
	}
	world.Authority.GetMutable<gui::Label>(sourceLabel)->Text = "server-update";
	world.Adopt();
	CHECK(world.Copy() == copy);
	CHECK(world.Replica.Get<gui::Label>(label)->Text == "server-update");
}

TEST_CASE("local ResetOnSpawn controls lifetime despite a fresh server GUI source", "[gui][playergui]") {
	using namespace engine;
	GuiWorld world;
	const auto originalSource = world.Source();
	const auto kept = world.Copy();
	const auto label = world.Replica.FindFirstChild(kept, "Status");
	world.Replica.GetMutable<gui::Layer>(kept)->ResetOnSpawn = false;
	world.Replica.GetMutable<gui::Label>(label)->Text = "kept-local";
	for (int life = 0; life < 2; ++life) {
		world.Character = world.Authority.Create();
		REQUIRE(gui::ResetPlayerGui(world.Authority, world.Player) == 1);
		world.Adopt();
		CHECK(world.Copy() == kept);
		CHECK(world.Replica.Get<gui::Label>(label)->Text == "kept-local");
		CHECK_FALSE(world.Replica.Get<gui::Layer>(kept)->ResetOnSpawn);
	}
	CHECK_FALSE(world.Replica.Alive(originalSource));
	const auto freshSource = world.Source();
	const auto sourceLabel = world.Authority.FindFirstChild(freshSource, "Status");
	world.Authority.GetMutable<gui::Label>(sourceLabel)->Text = "server-after-respawn";
	world.Adopt();
	CHECK(world.Replica.Get<gui::Label>(label)->Text == "server-after-respawn");
	world.Replica.GetMutable<gui::Layer>(kept)->ResetOnSpawn = true;
	world.Character = world.Authority.Create();
	REQUIRE(gui::ResetPlayerGui(world.Authority, world.Player) == 1);
	world.Adopt();
	CHECK_FALSE(world.Replica.Alive(kept));
	REQUIRE(world.Copy() != ecs::NULL_ENTITY);
	CHECK(world.Copy() != kept);
}

TEST_CASE("local GUI Destroy and Parent edits remain local until the next spawn", "[gui][playergui]") {
	using namespace engine;
	GuiWorld world;
	const auto copy = world.Copy();
	world.Replica.SetParent(copy, ecs::NULL_ENTITY);
	world.Adopt();
	CHECK(world.Replica.ParentOf(copy) == ecs::NULL_ENTITY);
	world.Replica.DestroyInstance(copy);
	world.Adopt();
	CHECK(world.Copy() == ecs::NULL_ENTITY);
	CHECK(world.Authority.Alive(world.Source()));
	world.Character = world.Authority.Create();
	REQUIRE(gui::ResetPlayerGui(world.Authority, world.Player) == 1);
	world.Adopt();
	REQUIRE(world.Copy() != ecs::NULL_ENTITY);
	CHECK(world.Copy() != copy);
	CHECK(world.Replica.ParentOf(world.Copy()) == world.Target);
}

TEST_CASE("server GUI edits preserve local identity and remap selection references", "[gui][playergui]") {
	using namespace engine;
	GuiWorld world;
	const auto source = world.Source();
	const auto sourceLabel = world.Authority.FindFirstChild(source, "Status");
	const auto label = world.Replica.FindFirstChild(world.Copy(), "Status");
	const auto folder = world.Authority.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "Folder");
	world.Authority.SetParent(folder, source);
	const auto button = world.Authority.CreateInstance(gui::GuiClass("TextButton"), "Next");
	world.Authority.SetParent(button, source);
	world.Adopt();
	const auto localButton = gui::FindPlayerGuiCopy(world.Replica, button);
	REQUIRE(localButton != ecs::NULL_ENTITY);
	world.Replica.GetMutable<gui::Label>(label)->Text = "local-text";
	world.Authority.SetInstanceName(sourceLabel, "Renamed");
	world.Authority.SetParent(sourceLabel, folder);
	world.Authority.GetMutable<gui::Selection>(button)->NextRight = sourceLabel;
	world.Adopt();
	CHECK(gui::FindPlayerGuiCopy(world.Replica, sourceLabel) == label);
	CHECK(world.Replica.InstanceNameOf(label).Text() == "Renamed");
	CHECK(world.Replica.ParentOf(label) == gui::FindPlayerGuiCopy(world.Replica, folder));
	CHECK(world.Replica.Get<gui::Label>(label)->Text == "local-text");
	CHECK(world.Replica.Get<gui::Selection>(localButton)->NextRight == label);
	world.Authority.DestroyInstance(sourceLabel);
	world.Adopt();
	CHECK_FALSE(world.Replica.Alive(label));
	world.Authority.DestroyInstance(source);
	world.Adopt();
	CHECK_FALSE(world.Replica.Alive(localButton));
}

TEST_CASE("GUI copies pair only archivable children and start with local layout state", "[gui][playergui]") {
	using namespace engine;
	gui::RegisterGuiClasses();
	ecs::Store store("gui.copy-state");
	const auto instance = ecs::Classes::Find(core::Name("Instance"));
	const auto player = store.CreateInstance(instance, "Ada");
	const auto target = store.CreateInstance(instance, "PlayerGui");
	store.SetParent(target, player);
	const auto source = store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
	store.SetParent(source, target);
	const auto skipped = store.CreateInstance(gui::GuiClass("TextLabel"), "NotCopied");
	store.SetParent(skipped, source);
	store.Set(skipped, ecs::NotArchivable{});
	store.GetMutable<gui::Label>(skipped)->Text = "excluded";
	const auto kept = store.CreateInstance(gui::GuiClass("TextLabel"), "Kept");
	store.SetParent(kept, source);
	store.GetMutable<gui::Label>(kept)->Text = "visible";
	store.Set(source, gui::Canvas{});
	store.Set(source, gui::CanvasTransform{});
	REQUIRE(gui::RefreshPlayerGuiProjection(store, player, ecs::NULL_ENTITY) == 1);
	const auto copy = gui::FindPlayerGuiCopy(store, source);
	REQUIRE(copy != ecs::NULL_ENTITY);
	CHECK_FALSE(store.Has<gui::Canvas>(copy));
	CHECK_FALSE(store.Has<gui::CanvasTransform>(copy));
	CHECK(gui::FindPlayerGuiCopy(store, skipped) == ecs::NULL_ENTITY);
	const auto localLabel = gui::FindPlayerGuiCopy(store, kept);
	REQUIRE(localLabel != ecs::NULL_ENTITY);
	CHECK(store.InstanceNameOf(localLabel).Text() == "Kept");
	CHECK(store.Get<gui::Label>(localLabel)->Text == "visible");
	CHECK(store.FindFirstChild(copy, "NotCopied") == ecs::NULL_ENTITY);
	(void)gui::RefreshPlayerGuiProjection(store, player, ecs::NULL_ENTITY);
	CHECK(gui::FindPlayerGuiCopy(store, kept) == localLabel);
	CHECK(store.FindFirstChild(copy, "NotCopied") == ecs::NULL_ENTITY);
}
