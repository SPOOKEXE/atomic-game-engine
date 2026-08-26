#pragma once

// The window, and everything that is not a value.
//
// **The launcher is a window over `Plan.hpp` and a spawn through
// `Supervisor.hpp`, and it links neither the client, the server, the origin nor
// the editor.** That is what keeps it a `client`-tier program with no tier
// escape at all, and it is why it works the same whether the tree beside it was
// built an hour ago or is missing half its programs.
//
// **Why a window rather than a terminal.** The launcher is the first thing a
// person opens, and the modes it offers end in a window every time - a game, an
// editor, or a server whose log you want beside them. Being the odd one out
// would have been a decision to explain.
//
// **The frame is a UI-only frame, and it still submits one `render::View`.**
// `Renderer::Render` returns without presenting when handed no views at all, so
// the empty default view below is not ceremony: it is what makes the frame
// reach the swapchain with nothing but the interface drawn into it.
//
// @tier L13 · client
// @since v0.18

#include <engine/render/Overlay.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/ui/Interface.hpp>

#include <cstdint>
#include <launcher/Catalogue.hpp>
#include <launcher/Description.hpp>
#include <launcher/Plan.hpp>
#include <launcher/Supervisor.hpp>
#include <map>
#include <memory>
#include <string>

struct SDL_Window;

namespace launcher {

	// How one run of the launcher is set up.
	//
	// @since v0.18
	struct Options {
		// Window width in pixels.
		int Width = 1100;

		// Window height in pixels.
		int Height = 720;

		// What every font and padding is multiplied by.
		float Scale = 1.0f;

		// Run with no window. **Needs `Frames`**, for the client's and the
		// editor's reason: there is nothing to close, so without a budget the
		// run would never end.
		bool Headless = false;

		// Exit after this many presented frames, or negative for no limit.
		int64_t MaximumFrames = -1;

		// The mode to open on, or empty for the front screen.
		std::string StartMode;

		// Read shaders and data from here rather than beside the binary.
		std::filesystem::path Assets;
	};

	// The launcher's window, its screens and the child it is watching.
	class Launcher {
	  public:
		Launcher();
		~Launcher();

		Launcher(const Launcher &) = delete;
		Launcher &operator=(const Launcher &) = delete;

		// Opens the window and asks every staged program what it accepts.
		//
		// @param options How to set it up.
		// @return `false` when the window, the renderer or the interface refused.
		bool Initialise(const Options &options);

		// Runs until the window is closed or the frame budget runs out.
		//
		// @return The process exit code: zero unless a child could not be
		//         started at all, which is the one failure a launcher is
		//         responsible for.
		int Run();

	  private:
		// One frame: events, then widgets, then a present.
		void Frame(float frameSeconds);

		// The front screen - one button per mode, greyed where the program is
		// not staged.
		void DrawModes();

		// One mode's form: a heading, a search, and three tabs.
		void DrawForm();

		// The pinned block - what this mode is, in five or six rows.
		void DrawCommonTab(const Mode &mode, const Description &description);

		// Every option the program declared, grouped by name prefix.
		void DrawAllTab(const Description &description);

		// `core::Flags`' declared table, grouped by dotted prefix.
		void DrawSettingsTab(const Description &description);

		// The one browse dialog every path row shares, and what it lands in.
		void DrawBrowseDialogs();

		// What a supervised child is doing, and the buttons that act on it.
		void DrawSupervision();

		// The command line the Launch button will run, and the button.
		void DrawLaunch();

		// One option's row - a checkbox, a field, and its buttons.
		//
		// Drawn inside a three-column table so a long name cannot push a value
		// field off the panel, which is what a fixed `SameLine` offset did.
		void DrawOption(const DescribedOption &option, size_t row);

		// Every row naming this option, in form order.
		//
		// A separate pass rather than an index, because `+`, `-` and a
		// multi-folder pick all change the row count *while* the form is being
		// drawn - so a loop holding an index from before the change walks off
		// the end of the vector.
		void DrawRowsOf(const DescribedOption &option);

		// Opens a mode, building its form from what its program declared.
		void Open(const Mode &mode);

		// Starts the current form's command line.
		void Launch();

		Options Settings;
		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;
		engine::render::OverlayImage Overlay;
		engine::ui::Interface Interface;

		std::vector<Mode> Catalogue;
		Descriptions Programs;
		std::filesystem::path Stage;

		// Which mode is open, or empty on the front screen.
		std::string Open_;

		// One form per mode id, kept while the launcher is open so that
		// stepping out to the front screen and back does not lose what somebody
		// typed. **Not persisted between runs** - that is a profiles feature and
		// it is not this version's.
		std::map<std::string, Form, std::less<>> Forms;

		// What the search box holds, per mode.
		std::string Query;

		// Whether the previous frame had a search running.
		//
		// A search forces every group with a hit open, because a match hidden
		// inside a collapsed header reads as no match at all. This is what lets
		// the groups close again when the box is cleared, without the override
		// running on every other frame and making the headers unclickable - see
		// `ForceHeaderState`.
		bool WasSearching = false;

		// The path a browse dialog is editing, and the row it belongs to.
		std::string BrowsePath;
		size_t BrowseRow = 0;

		// Which of the three dialogs the last Browse button asked for. One
		// dialog each, shared by every row, because only one can be open and a
		// popup per option would be forty popups.
		enum class Browsing : uint8_t { None, File, Folder, Folders };
		Browsing BrowseKind = Browsing::None;

		// Set by a Browse button, consumed by `DrawBrowseDialogs`.
		//
		// **The request is recorded rather than acted on, and it has to be.**
		// `ImGui::OpenPopup` names the popup against the id stack at the moment
		// it is called, and a Browse button is called from inside a row's
		// `PushID` inside a table inside the scrolling child - three levels
		// deeper than the `BeginPopupModal` that has to match it. Opening from
		// there produces an id nothing ever begins, so the button highlights,
		// the tooltip shows, and no dialog appears. Deferring to the one place
		// the modal lives makes both ends agree by construction.
		bool BrowseRequested = false;

		Supervisor Child;

		// The last thing that went wrong, shown until something else happens.
		std::string Failure;

		int64_t FramesDrawn = 0;
		bool Quit = false;
	};
}
