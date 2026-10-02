#pragma once

#include <engine/core/Name.hpp>
#include <engine/imagegraph/Vector2Presentation.hpp>

#include <functional>
#include <nodegraph/Editor.hpp>
#include <optional>
#include <studio/ImageGraph.hpp>
#include <studio/Vector2Editor.hpp>
#include <unordered_map>

namespace engine::render {
	class Renderer;
}

namespace studio {
	// Presentation state belongs to a durable node ID, independently of canvas rebuilds.
	class Vector2Panel {
	  public:
		void Attach(
			nodegraph::Canvas &canvas,
			engine::imagegraph::Document &document,
			ImageGraphCanvasIds &ids,
			ImageGraphHistory &history,
			std::function<void()> changed
		);
		void Refresh(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::EvaluationRequest &request,
			uint64_t revision,
			uint64_t inputs
		);
		void DrawOverlay(
			engine::render::Renderer &renderer,
			Vector2EditorRect image,
			double previewScale,
			std::string_view selectedNode
		);
		void DrawSnapControls();
		void FinishPointer(bool leftDown, bool middleDown);
		void PumpRetired(engine::render::Renderer &renderer);
		void Close(engine::render::Renderer &renderer);
		const engine::imagegraph::Vector2Presentation *Presentation(std::string_view node) const;
		const engine::imagegraph::Diagnostic &Diagnostic() const {
			return Error;
		}

	  private:
		struct Row {
			Vector2PadView View;
			engine::imagegraph::Vector2Presentation Value;
			bool Valid = false;
			bool ScalarAxes = true;
			engine::imagegraph::Diagnostic Error;
			uint64_t Revision = 0, Inputs = 0, Tick = 0;
			double Subframe = 0;
			bool NegativeFrame = false;
			engine::core::Name Texture, Retired;
			uint64_t TextureHash = 0, TextureSerial = 0;
			size_t TextureBytes = 0, RetiredBytes = 0;
		};
		std::unordered_map<std::string, Row> Rows;
		std::vector<std::pair<engine::core::Name, size_t>> PendingDrop;
		engine::imagegraph::Document *Document = nullptr;
		ImageGraphCanvasIds *Ids = nullptr;
		ImageGraphHistory *History = nullptr;
		std::function<void()> Changed;
		engine::imagegraph::Diagnostic Error;
		std::string GestureNode, MenuNode;
		std::optional<engine::imagegraph::Document> Before, SettingsBefore;
		uint32_t SettingsId = 0;
		uint64_t GestureTick = 0;
		double GestureSubframe = 0;
		bool GestureNegativeFrame = false;
		Vector2PreviewSnap CurrentSnap() const;
		void FinishSettings();
		bool Panning = false, OverlayDragging = false;
		engine::imagegraph::Vector2 DragMouse, DragValue;
		std::string Id(nodegraph::NodeId canvas) const;
		void Begin(std::string_view node, bool overlay);
		void End();
		bool Edit(std::string_view node, engine::imagegraph::Vector2 coordinate);
		bool InputBody(const nodegraph::Node &node, const nodegraph::BodyFrame &frame);
		void DrawBody(const nodegraph::Node &node, const nodegraph::BodyFrame &frame);
		bool Upload(Row &row, std::string_view node, engine::render::Renderer &renderer);
		size_t HeldSpriteBytes() const;
	};
}
