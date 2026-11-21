// The native half of `plugin.SaveFile`, `OpenFile`, `SaveFolder` and the
// clipboard calls: SDL's dialogs, and the reading and writing done once a
// dialog answers. `PluginSurface.cpp` checks the arguments and suspends the
// caller; this file finishes the call from the plugin beat.

#include "PluginSurfaceInternal.hpp"

#include <engine/core/Log.hpp>
#include <engine/script/Runtime.hpp>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>
#include <system_error>

namespace studio {

	using engine::script::HostTag;
	using engine::script::HostValue;

	namespace {
		// One dialog in flight. SDL needs the filters alive until it answers,
		// so they live here beside the answer and go when it runs.
		struct DialogCall {
			std::vector<PluginFileFilter> Owned;
			std::vector<SDL_DialogFileFilter> Filters;
			PluginNative::Answer Respond;
		};

		void SDLCALL DialogAnswered(void *userdata, const char *const *files, int) {
			const std::unique_ptr<DialogCall> call(static_cast<DialogCall *>(userdata));
			if (files == nullptr) {
				ENGINE_WARN("plugin file dialog failed: {}", SDL_GetError());
				call->Respond(std::nullopt);
				return;
			}
			if (files[0] == nullptr) {
				call->Respond(std::nullopt);
				return;
			}
			call->Respond(std::string(files[0]));
		}

		DialogCall *NewCall(std::vector<PluginFileFilter> filters, PluginNative::Answer answer) {
			auto *call = new DialogCall{std::move(filters), {}, std::move(answer)};
			for (const PluginFileFilter &filter : call->Owned) {
				call->Filters.push_back({filter.Name.c_str(), filter.Pattern.c_str()});
			}
			return call;
		}

		class SdlPluginNative final : public PluginNative {
		  public:
			explicit SdlPluginNative(SDL_Window *const *window) : Window(window) {}

			void
			SaveFile(std::string_view name, std::vector<PluginFileFilter> filters, Answer answer) override {
				DialogCall *call = NewCall(std::move(filters), std::move(answer));
				const std::string location(name);
				SDL_ShowSaveFileDialog(
					DialogAnswered,
					call,
					Parent(),
					call->Filters.data(),
					static_cast<int>(call->Filters.size()),
					location.c_str()
				);
			}

			void OpenFile(std::vector<PluginFileFilter> filters, Answer answer) override {
				DialogCall *call = NewCall(std::move(filters), std::move(answer));
				SDL_ShowOpenFileDialog(
					DialogAnswered,
					call,
					Parent(),
					call->Filters.data(),
					static_cast<int>(call->Filters.size()),
					nullptr,
					false
				);
			}

			void OpenFolder(Answer answer) override {
				SDL_ShowOpenFolderDialog(
					DialogAnswered, NewCall({}, std::move(answer)), Parent(), nullptr, false
				);
			}

			bool SetClipboard(const std::string &text) override {
				return SDL_SetClipboardText(text.c_str());
			}

			std::string GetClipboard() override {
				char *text = SDL_GetClipboardText();
				std::string out = text != nullptr ? text : "";
				SDL_free(text);
				return out;
			}

		  private:
			SDL_Window *Parent() const {
				return Window != nullptr ? *Window : nullptr;
			}

			SDL_Window *const *Window;
		};

		bool WriteWhole(const std::filesystem::path &path, const std::string &bytes, std::string &failure) {
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!out) {
				failure = "could not write " + path.filename().string();
				return false;
			}
			return true;
		}

		// Does what the request asked with the path its dialog chose.
		bool Complete(
			const LoadedPlugin::FileRequest &request,
			const std::filesystem::path &chosen,
			HostValue &answer,
			std::string &failure
		) {
			using Kind = LoadedPlugin::FileRequest::Kind;
			std::error_code error;
			if (request.Call == Kind::Save) {
				if (!WriteWhole(chosen, request.Bytes, failure)) return false;
				answer = HostValue::Of(std::string_view(chosen.filename().string()));
				return true;
			}
			if (request.Call == Kind::Open) {
				const uintmax_t size = std::filesystem::file_size(chosen, error);
				if (error) {
					failure = "could not read " + chosen.filename().string();
					return false;
				}
				if (size > PLUGIN_FILE_MAX_BYTES) {
					failure = "a file is limited to 256 MiB";
					return false;
				}
				std::string bytes(static_cast<size_t>(size), '\0');
				std::ifstream in(chosen, std::ios::binary);
				in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
				if (!in) {
					failure = "could not read " + chosen.filename().string();
					return false;
				}
				answer = HostValue(HostTag::Map);
				answer.Entries.emplace_back(
					"Name", HostValue::Of(std::string_view(chosen.filename().string()))
				);
				answer.Entries.emplace_back(
					"Data",
					request.AsText ? HostValue::Of(std::string_view(bytes))
								   : HostValue::Bytes(std::move(bytes))
				);
				return true;
			}

			const std::filesystem::path folder = chosen / request.Folder;
			for (const auto &[relative, bytes] : request.Files) {
				const std::filesystem::path path = folder / relative;
				std::filesystem::create_directories(path.parent_path(), error);
				if (error) {
					failure = "could not make the folder for " + relative;
					return false;
				}
				if (!WriteWhole(path, bytes, failure)) return false;
			}
			answer = HostValue::Of(std::string_view(request.Folder));
			return true;
		}
	}

	std::unique_ptr<PluginNative> MakeSdlPluginNative(SDL_Window *const *window) {
		return std::make_unique<SdlPluginNative>(window);
	}

	void Editor::ResolvePluginFiles(std::vector<LoadedPlugin> &plugins) {
		std::vector<std::pair<uint64_t, std::optional<std::string>>> ready;
		{
			const std::lock_guard<std::mutex> held(PluginDialogMailbox->Lock);
			ready.swap(PluginDialogMailbox->Ready);
		}
		for (auto &[token, path] : ready) {
			LoadedPlugin *owner = nullptr;
			for (LoadedPlugin &plugin : plugins) {
				if (plugin.Filing.Token == token) owner = &plugin;
			}
			if (owner == nullptr) continue;
			const LoadedPlugin::FileRequest request = std::move(owner->Filing);
			owner->Filing = {};
			if (!owner->Running || owner->Vm == nullptr) continue;

			// A cancelled dialog answers nil.
			HostValue answer;
			std::string failure;
			const bool done = !path.has_value() || Complete(request, *path, answer, failure);
			owner->Vm->ResolveHostCall(token, done, std::move(answer), std::move(failure));
		}
	}
}
