#pragma once

#include <engine/audio/Device.hpp>

#include <functional>
#include <memory>
#include <studio/ImageGraph.hpp>

namespace studio {
	bool SyncImageGraphWavTimeline(
		engine::imagegraph::Document &document,
		const std::string &nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		const ImageGraphPlayback &playback,
		engine::imagegraph::Diagnostic &diagnostic
	);

	// Exclusive audio owner for the Image Composer. Opening is an explicit user action.
	// Sources remain pinned until a paused owner has drained and unbound every voice.
	class ImageGraphWavPreview {
	  public:
		using DeviceFactory =
			std::function<std::unique_ptr<engine::audio::Device>(const engine::audio::DeviceSettings &)>;
		explicit ImageGraphWavPreview(DeviceFactory factory = {});
		~ImageGraphWavPreview();
		ImageGraphWavPreview(const ImageGraphWavPreview &) = delete;
		ImageGraphWavPreview &operator=(const ImageGraphWavPreview &) = delete;
		bool Enabled() const;
		bool Enable(engine::imagegraph::Diagnostic &diagnostic);
		bool Update(
			const engine::imagegraph::Document &document,
			std::span<const engine::imagegraph::AudioClipSource> sources,
			std::span<const engine::imagegraph::AudioCaptureFrame> captures,
			const ImageGraphPlayback &playback,
			uint64_t documentRevision,
			uint64_t inputRevision,
			engine::imagegraph::Diagnostic &diagnostic
		);
		bool LoadSource(
			std::vector<engine::imagegraph::AudioClipSource> &sources,
			ImageGraphPreviewCache &cache,
			std::string_view sourceId,
			const std::filesystem::path &path,
			engine::imagegraph::Diagnostic &diagnostic
		);
		// Polls only explicitly loaded paths; no audio device is opened.
		bool CheckFiles(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::EvaluationRequest &request,
			uint64_t hostFrame,
			std::vector<engine::imagegraph::AudioClipSource> &sources,
			ImageGraphPreviewCache &cache,
			size_t &reloaded,
			engine::imagegraph::Diagnostic &diagnostic
		);
		bool RemoveSource(
			std::vector<engine::imagegraph::AudioClipSource> &sources,
			ImageGraphPreviewCache &cache,
			std::string_view sourceId,
			engine::imagegraph::Diagnostic &diagnostic
		);
		void Close();

	  private:
		bool LoadSourceWithWorkspace(
			std::vector<engine::imagegraph::AudioClipSource> &sources,
			ImageGraphPreviewCache &cache,
			std::string_view sourceId,
			const std::filesystem::path &path,
			uint64_t workspaceBytes,
			engine::imagegraph::Diagnostic &diagnostic
		);
		struct Owner;
		std::unique_ptr<Owner> Audio;
	};
}
