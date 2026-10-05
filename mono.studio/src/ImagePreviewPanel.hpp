#pragma once

#include "ImageGraphGroupHost.hpp"
#include "ImageGraphPreviewProvider.hpp"

#include <engine/core/Name.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>

#include <nodegraph/Editor.hpp>
#include <studio/ImageGraph.hpp>
#include <unordered_map>

namespace engine::render {
	class Renderer;
}
namespace studio::detail {
	struct ImagePreviewKey {
		uint64_t Revision = 0, Inputs = 0, Refresh = 0;
		engine::imagegraph::FrameTime Frame;
		uint8_t Playback = 0;
		bool operator==(const ImagePreviewKey &) const = default;
	};
	bool MakeImagePreviewDocument(
		const engine::imagegraph::Document &source,
		std::string_view node,
		engine::imagegraph::Document &preview
	);
	class ImagePreviewPanel {
	  public:
		void Attach(nodegraph::Canvas &canvas, const ImageGraphCanvasIds &ids);
		void Request() {
			++RefreshGeneration;
		}
		void Refresh(
			const engine::imagegraph::Document &document,
			engine::imagegraph::EvaluationRequest request,
			ImageGraphHost &host,
			engine::render::Renderer &renderer,
			uint64_t revision,
			uint64_t inputs,
			uint8_t playback
		);
		void Close(engine::render::Renderer &renderer);
		std::string_view Message(std::string_view node) const {
			const auto found = Rows.find(std::string(node));
			return found == Rows.end() ? std::string_view{} : found->second.Message;
		}

	  private:
		struct Row {
			ImagePreviewKey Key;
			bool Evaluated = false, Pending = false;
			uint64_t Hash = 0, TextureBytes = 0, RetiredBytes = 0;
			uint32_t Width = 0, Height = 0;
			engine::core::Name Texture, Retired;
			uint8_t Slot = 1;
			void *Handle = nullptr;
			std::string Message;
			ImageGraphPreviewObservations Observations;
			engine::imagegraph::CapturedFeedbackHost Feedback;
		};
		ImageGraphGroupHost Groups;
		uint64_t RefreshGeneration = 0;
		std::unordered_map<std::string, Row> Rows;
	};
}
