#pragma once

// Executes bounded imagegraph still, sequence and animation exports from command-line arguments.

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

namespace engine::imagegraph {
	struct RequestImageSource;
	struct Image;
	struct HostNodeCapture;
	class HostNodeProvider;
	struct Document;
	struct Plan;
	struct EvaluationRequest;
}

namespace engine::imagegraphexport::runner {

	// Parses, compiles and exports one selected output with explicit timeline inputs.
	//
	// @return Zero on success, two for argument errors, or one for graph, execution or file errors.
	int Run(int argc, char **argv, std::ostream &output, std::ostream &errors);
	// Host-owned decoded inputs. The runner never opens a path selected by a graph node.
	int RunWithImages(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		std::span<const engine::imagegraph::RequestImageSource> imageSources
	);
	// Host capabilities are supplied explicitly and remain process-local.
	int RunWithHostInputs(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		std::span<const engine::imagegraph::RequestImageSource> imageSources,
		std::span<const engine::imagegraph::HostNodeCapture> captures,
		engine::imagegraph::HostNodeProvider *provider
	);

	// Executes an immutable in-memory document and plan without reading the --input context path.
	int RunWithDocument(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request
	);

	// Encodes host-owned pixels. The caller stages the destination before publishing a file set.
	bool WriteStillImage(
		const std::filesystem::path &path, const engine::imagegraph::Image &image, std::string &failure
	);

}
