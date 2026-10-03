#pragma once

#include <engine/assets/ContentPolicy.hpp>
#include <engine/assets/LocalStore.hpp>
#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/render/ComposerSurface.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	struct ImageGraphCookCapture {
		std::string NodeId;
		engine::core::Name Owner;
		uint64_t Revision = 0, InputRevision = 0;
	};

	inline bool ImageGraphCookCaptureCurrent(
		const engine::imagegraph::Document &document,
		const ImageGraphCookCapture &captured,
		std::string_view selectedNode,
		engine::core::Name owner,
		uint64_t revision,
		uint64_t inputRevision
	) {
		return captured.NodeId == selectedNode && captured.Owner == owner && owner.IsValid() &&
			   captured.Revision == revision && captured.InputRevision == inputRevision &&
			   std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
				   return node.Id == captured.NodeId && node.Type == "pc.hlsl";
			   });
	}

	struct ImageGraphCookLibraries {
		struct Source {
			std::string Name, Text;
		};
		std::vector<Source> Sources;
		std::vector<engine::render::hlsl::Library> Views;
		uint64_t Bytes = 0;
	};
	inline std::optional<uint64_t>
	ImageGraphCookLibraryBytes(const ImageGraphCookLibraries &libraries, uint64_t maximumBytes) {
		uint64_t bytes = 0;
		const auto add = [&](uint64_t size) {
			if (size > maximumBytes - bytes) return false;
			bytes += size;
			return true;
		};
		const auto vector = [&](uint64_t count, uint64_t size) {
			return count <= (maximumBytes - bytes) / size && add(count * size);
		};
		if (!add(sizeof(libraries)) ||
			!vector(libraries.Sources.capacity(), sizeof(ImageGraphCookLibraries::Source)) ||
			!vector(libraries.Views.capacity(), sizeof(engine::render::hlsl::Library)))
			return {};
		for (const auto &source : libraries.Sources) {
			if (!add(source.Name.capacity()) || !add(1) || !add(source.Text.capacity()) || !add(1)) return {};
		}
		return bytes;
	}

	// The operation ceiling includes the previous output while the new capture is
	// prepared. File sizes and all grant names are admitted before reading sources.
	inline bool ReadImageGraphCookLibraries(
		std::span<const engine::imagegraphexport::GraphFileGrant> grants,
		std::string_view nodeId,
		ImageGraphCookLibraries &output,
		std::string &failure,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine;
		const auto prior = ImageGraphCookLibraryBytes(output, maximumBytes);
		if (!prior) {
			failure = "Previous shader libraries exceed the operation byte budget";
			return false;
		}
		const uint64_t available = maximumBytes - *prior;
		std::array<const imagegraphexport::GraphFileGrant *, 64> selected{};
		std::array<uint64_t, 64> sizes{};
		size_t count = 0;
		uint64_t textBytes = 0, minimumBytes = sizeof(ImageGraphCookLibraries);
		const auto admit = [&](uint64_t bytes) {
			if (minimumBytes > available || bytes > available - minimumBytes) return false;
			minimumBytes += bytes;
			return true;
		};
		for (const auto &grant : grants) {
			if (grant.NodeId != nodeId) continue;
			if (grant.Write || grant.Resource.empty() || grant.Resource.size() > 1024 ||
				!grant.File.is_absolute() || grant.File.native().size() > 4096 ||
				grant.File.lexically_normal() != grant.File ||
				!assets::ContentPolicy::Process(assets::ContentVerb::Handle)
					 .AllowsName(grant.File.string())) {
				failure = "Shader libraries require named exact absolute read grants";
				return false;
			}
			if (count == selected.size() ||
				std::any_of(selected.begin(), selected.begin() + count, [&](const auto *held) {
					return held->Resource == grant.Resource;
				})) {
				failure = "Shader library grants are duplicated or exceed the source limit";
				return false;
			}
			std::error_code error;
			const auto size = std::filesystem::file_size(grant.File, error);
			if (error || size > render::hlsl::MAXIMUM_SOURCE_BYTES - textBytes ||
				!admit(sizeof(ImageGraphCookLibraries::Source) + sizeof(render::hlsl::Library)) ||
				!admit(grant.Resource.size() + 1) || !admit(size + 1)) {
				failure = "Shader library source exceeds its byte budget or is unavailable";
				return false;
			}
			selected[count] = &grant;
			sizes[count++] = size;
			textBytes += size;
		}
		ImageGraphCookLibraries candidate;
		candidate.Sources.reserve(count);
		candidate.Views.reserve(count);
		if (!ImageGraphCookLibraryBytes(candidate, available)) {
			failure = "Shader library storage exceeds the operation byte budget";
			return false;
		}
		for (size_t i = 0; i < count; ++i) {
			const auto beforeName = ImageGraphCookLibraryBytes(candidate, available);
			if (!beforeName || selected[i]->Resource.size() + 1 > available - *beforeName) {
				failure = "Shader library names exceed the operation byte budget";
				return false;
			}
			auto &source = candidate.Sources.emplace_back();
			source.Name = selected[i]->Resource;
			const auto held = ImageGraphCookLibraryBytes(candidate, available);
			if (!held || sizes[i] + 1 > available - *held) {
				failure = "Shader library names exceed the operation byte budget";
				return false;
			}
			source.Text.resize(size_t(sizes[i]));
			if (!ImageGraphCookLibraryBytes(candidate, available)) {
				failure = "Shader library text storage exceeds the operation byte budget";
				return false;
			}
			std::ifstream file(selected[i]->File, std::ios::binary);
			if (!file.read(source.Text.data(), std::streamsize(sizes[i])) ||
				file.peek() != std::char_traits<char>::eof()) {
				failure = "Shader library changed during bounded read";
				return false;
			}
		}
		candidate.Bytes = *ImageGraphCookLibraryBytes(candidate, available);
		for (const auto &source : candidate.Sources)
			candidate.Views.push_back({source.Name, source.Text});
		output = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		failure = "Shader library allocation was refused";
		return false;
	}

	// Final names are immutable. A staged hard link refuses replacement, including
	// races.
	inline bool PersistImageGraphCook(
		const engine::assets::LocalPaths &paths,
		engine::core::Name name,
		const engine::assets::ShaderData &artifact,
		std::span<const std::byte> bytes,
		std::string &failure,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		using namespace engine;
		const auto final = paths.Baked / std::string(name.Text());
		std::error_code error;
		if (std::filesystem::exists(final, error)) {
			assets::ShaderData existing;
			const auto read = render::hlsl::ReadArtifact(paths.Baked, name, existing, maximumBytes);
			if (read || existing != artifact) {
				failure = read.value_or("Immutable Composer artifact differs from the prepared cook");
				return false;
			}
			return true;
		}
		if (error || !assets::EnsureLocalStore(paths)) {
			failure = "Composer artifact store is unavailable";
			return false;
		}
		// Studio serializes authoring actions. The shared publisher owns exclusive
		// sibling staging.
		const auto pending = paths.Baked / ("composer-pending-" + std::string(name.Text()));
		struct RemovePending {
			std::filesystem::path Path;
			~RemovePending() {
				std::error_code ignored;
				std::filesystem::remove(Path, ignored);
			}
		} remove{pending};
		if (!imagegraphexport::PublishGraphHostFile(
				{"composer-cook", pending, true},
				assets::ContentPolicy::Process(assets::ContentVerb::Handle),
				bytes,
				assets::Shader::MAXIMUM_BYTES,
				failure
			))
			return false;
		std::filesystem::create_hard_link(pending, final, error);
		if (error) {
			assets::ShaderData existing;
			const auto read = render::hlsl::ReadArtifact(paths.Baked, name, existing, maximumBytes);
			if (read || existing != artifact) {
				failure =
					"Immutable Composer artifact publication was refused: " + read.value_or(error.message());
				return false;
			}
		}
		return true;
	}

	// Install and history publication share one rollback boundary. An unreferenced
	// immutable artifact may remain after refusal; no saved selector can point to
	// unpublished bytes.
	template <class InstalledRevision, class Install, class Remove>
	bool ApplyImageGraphCookAction(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		const ImageGraphCookCapture &captured,
		std::string_view selectedNode,
		engine::core::Name owner,
		uint64_t revision,
		uint64_t inputRevision,
		const engine::imagegraph::EvaluationSnapshot &inputs,
		const engine::assets::LocalPaths &paths,
		std::span<const engine::render::hlsl::Library> libraries,
		std::string_view compilerIdentity,
		const InstalledRevision &installedRevision,
		const Install &install,
		const Remove &remove,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine;
		using namespace imagegraph;
		diagnostic = {};
		const auto refuse = [&](Status status, std::string message) {
			diagnostic = {
				status, captured.NodeId, std::string(render::hlsl::COOKED_SELECTOR), std::move(message)
			};
			return false;
		};
		if (!ImageGraphCookCaptureCurrent(document, captured, selectedNode, owner, revision, inputRevision))
			return refuse(Status::InvalidValue, "Cook selection, owner or inputs changed during capture");
		if (compilerIdentity.empty())
			return refuse(Status::UnsupportedExecution, "This build did not record shader cooker provenance");
		const auto bytes = DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes > maximumBytes / 2 || inputs.RetainedBytes() > maximumBytes - 2 * *bytes)
			return refuse(
				Status::LimitExceeded, "Cook document, snapshot and artifact exceed the live byte budget"
			);
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &held) {
			return held.Id == captured.NodeId;
		});
		Document candidate;
		assets::ShaderData artifact;
		// This selector never escapes the unpublished candidate. The final name
		// hashes ASH1 bytes.
		if (const auto failure = render::hlsl::CookNode(
				document,
				*node,
				inputs,
				libraries,
				compilerIdentity,
				false,
				core::Name("composer.unpublished-cook"),
				candidate,
				artifact
			))
			return refuse(Status::InvalidValue, *failure);
		const auto artifactBytes = assets::ShaderRetainedPayloadBytes(artifact, maximumBytes);
		const uint64_t available = maximumBytes - 2 * *bytes - inputs.RetainedBytes();
		if (!artifactBytes || *artifactBytes > available / 4)
			return refuse(
				Status::LimitExceeded, "Cook artifact admission copies exceed the live byte budget"
			);
		const auto encodedBytes = assets::Shader::EncodedBytes(artifact);
		if (!encodedBytes || *encodedBytes > (available - 4 * *artifactBytes) / 2)
			return refuse(Status::LimitExceeded, "Cook ASH1 serialization exceeds the live byte budget");
		// Reserve the exact final stream. Shader::Write also owns its temporary body.
		core::ByteWriter encoded{size_t(*encodedBytes), size_t(*encodedBytes)};
		if (!assets::Shader::Write(encoded, artifact))
			return refuse(Status::InvalidValue, "Cooked shader cannot be serialized");
		const core::Name name(assets::Hasher::Of(encoded.Bytes()).ToHex() + ".ashader");
		auto target = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &held) {
			return held.Id == captured.NodeId;
		});
		auto property = std::find_if(
			target->SourceProperties.begin(), target->SourceProperties.end(), [](const auto &value) {
				return value.Port == render::hlsl::COOKED_SELECTOR;
			}
		);
		if (property == target->SourceProperties.end())
			return refuse(Status::InvalidValue, "Cook did not prepare its native artifact selector");
		property->Data = std::string(name.Text());
		Plan validated;
		if (Compile(candidate, validated, diagnostic) != Status::Ok) return false;
		std::string failure;
		if (!PersistImageGraphCook(
				paths, name, artifact, encoded.Bytes(), failure, available - *artifactBytes - encoded.Size()
			))
			return refuse(Status::InvalidValue, std::move(failure));
		const uint64_t prior = installedRevision(owner, name);
		if (const auto failure = install(owner, name, artifact))
			return refuse(Status::LimitExceeded, *failure);
		const uint64_t installed = installedRevision(owner, name);
		if (!installed)
			return refuse(Status::InvalidValue, "Cook installer did not retain the prepared artifact");
		struct Rollback {
			const Remove &RemoveInstalled;
			core::Name Owner, Name;
			uint64_t Revision;
			bool Active;
			~Rollback() {
				if (Active) (void)RemoveInstalled(Owner, Name, Revision);
			}
		} rollback{remove, owner, name, installed, prior == 0};
		if (document == candidate) {
			rollback.Active = false;
			return false;
		}
		if (!history.TryRecord(document, candidate))
			return refuse(Status::LimitExceeded, "Cook cannot retain its undo transition");
		document = std::move(candidate);
		rollback.Active = false;
		return true;
	} catch (const std::length_error &) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded,
			captured.NodeId,
			std::string(engine::render::hlsl::COOKED_SELECTOR),
			"Cook encoded bytes exceeded their limit"
		};
		return false;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded,
			captured.NodeId,
			std::string(engine::render::hlsl::COOKED_SELECTOR),
			"Cook allocation was refused"
		};
		return false;
	}
} // namespace studio::detail
