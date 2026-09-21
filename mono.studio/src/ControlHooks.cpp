// Studio-owned composition for optional control rows.

#include <engine/control/features/DataScene.hpp>

#include <stdexcept>
#include <studio/Editor.hpp>

namespace studio {

	void Editor::ActivateControlHooks() {
		std::string failure;
		StudioControlHook = ControlSurface.ActivateHook(
			{
				.Id = "studio.product",
				.Revision = "v1",
				.Purpose = "Editor-only selection, automation, and play controls.",
				.Dependencies = {},
				.Limits = {},
			},
			[this](engine::control::HookRegistration &registration) { RegisterControlTools(registration); },
			failure
		);
		if (!failure.empty()) throw std::runtime_error("could not activate Studio control hook: " + failure);

		StudioSceneRenderingHook = ControlSurface.ActivateHook(
			{
				.Id = "studio.scene-rendering",
				.Revision = "v1",
				.Purpose = "Reads camera calibration for Studio's rendered scene.",
				.Dependencies = {},
				.Limits = {},
			},
			[this](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::features::CameraRenderingDataTool(*Universe));
			},
			failure
		);
		if (!failure.empty()) {
			StudioControlHook.Close();
			throw std::runtime_error("could not activate Studio scene-rendering control hook: " + failure);
		}
	}
}
