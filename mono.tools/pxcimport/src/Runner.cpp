#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <pxcimport/Runner.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pxcimport {
	namespace {
		struct Options {
			std::filesystem::path Input;
			std::filesystem::path Output;
			std::filesystem::path ReferenceRgba;
			std::string OutputId;
			std::string ExtractNode;
		};

		void PrintUsage(std::ostream &stream) {
			stream << "usage: pxcimport --input project.pxc --output project.graph "
				   << "[--output-id id | --extract-node id] [--reference-rgba thumbnail.rgba]\n"
				   << "--extract-node writes only a compilable mapped upstream subgraph, "
				   << "not the Pixel Composer project output\n";
		}

		bool Parse(int argc, char **argv, Options &options, std::ostream &errors) {
			for (int index = 1; index < argc; index++) {
				const std::string_view option = argv[index];
				if ((option == "--input" || option == "--output" || option == "--output-id" ||
					 option == "--extract-node" || option == "--reference-rgba") &&
					index + 1 < argc && argv[index + 1][0] != '-') {
					const std::string value = argv[++index];
					if (option == "--input" && options.Input.empty())
						options.Input = value;
					else if (option == "--output" && options.Output.empty())
						options.Output = value;
					else if (option == "--output-id" && options.OutputId.empty())
						options.OutputId = value;
					else if (option == "--extract-node" && options.ExtractNode.empty())
						options.ExtractNode = value;
					else if (option == "--reference-rgba" && options.ReferenceRgba.empty())
						options.ReferenceRgba = value;
					else {
						errors << "duplicate option: " << option << '\n';
						return false;
					}
				} else {
					errors << "invalid option: " << option << '\n';
					return false;
				}
			}
			if (options.Input.empty() || options.Output.empty()) {
				PrintUsage(errors);
				return false;
			}
			if (!options.OutputId.empty() && !options.ExtractNode.empty()) {
				errors << "--output-id and --extract-node are mutually exclusive\n";
				return false;
			}
			return true;
		}

		bool
		ReadBounded(const std::filesystem::path &path, std::vector<std::byte> &bytes, std::ostream &errors) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input) {
				errors << "cannot open PXCX input: " << path << '\n';
				return false;
			}
			const std::streampos end = input.tellg();
			if (end < 0 || static_cast<uint64_t>(end) > engine::bake::PxcxLimits::MaximumArchiveBytes) {
				errors << "PXCX input exceeds 64 MiB file limit\n";
				return false;
			}
			bytes.resize(static_cast<size_t>(end));
			input.seekg(0);
			if (!input.read(
					reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
				)) {
				errors << "cannot read PXCX input\n";
				return false;
			}
			return true;
		}
	}

	int Run(int argc, char **argv, std::ostream &output, std::ostream &errors) {
		if (argc == 2 && std::string_view(argv[1]) == "--help") {
			PrintUsage(output);
			return 0;
		}
		Options options;
		if (!Parse(argc, argv, options, errors)) return 2;
		if (std::filesystem::absolute(options.Input).lexically_normal() ==
			std::filesystem::absolute(options.Output).lexically_normal()) {
			errors << "input and output paths must differ\n";
			return 2;
		}
		if (!options.ReferenceRgba.empty() &&
			(std::filesystem::absolute(options.Input).lexically_normal() ==
				 std::filesystem::absolute(options.ReferenceRgba).lexically_normal() ||
			 std::filesystem::absolute(options.Output).lexically_normal() ==
				 std::filesystem::absolute(options.ReferenceRgba).lexically_normal())) {
			errors << "reference RGBA path must differ from input and graph output\n";
			return 2;
		}

		std::vector<std::byte> bytes;
		if (!ReadBounded(options.Input, bytes, errors)) return 1;
		engine::bake::PxcxArchive archive;
		std::string failure;
		if (!engine::bake::ReadPxcx(bytes, archive, failure)) {
			errors << "PXCX parse failed: " << failure << '\n';
			return 1;
		}
		engine::imagegraphio::PxcxImport imported;
		if (!engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure)) {
			errors << "PXCX import failed: " << failure << '\n';
			return 1;
		}
		const auto reference = imported.ReferencePreview();
		if (!options.ReferenceRgba.empty() && !reference) {
			errors << "PXCX has no saved source-reference thumbnail\n";
			return 1;
		}

		engine::imagegraph::Document graph = imported.Graph;
		if (!options.ExtractNode.empty()) {
			auto selected = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, options.ExtractNode);
			for (const auto &cut : selected.Cuts)
				errors << "cut node=\"" << cut.NodeId << "\" port=\"" << cut.Port << "\": " << cut.Message
					   << '\n';
			if (selected.Compilation != engine::imagegraph::Status::Ok) {
				errors << "selected subgraph cannot compile: " << selected.Diagnostic.Message << '\n';
				return 1;
			}
			graph = std::move(selected.Graph);
			output << "partial_subgraph selected=\"" << options.ExtractNode
				   << "\" cuts=" << selected.Cuts.size() << '\n';
		} else if (!options.OutputId.empty()) {
			const auto selected = std::find_if(
				graph.Outputs.begin(), graph.Outputs.end(), [&](const engine::imagegraph::Output &entry) {
					return entry.Id == options.OutputId;
				}
			);
			if (selected == graph.Outputs.end()) {
				errors << "unknown output id: " << options.OutputId << '\n';
				return 1;
			}
			const engine::imagegraph::Output output = *selected;
			graph.Outputs.assign(1, output);
		}

		const std::string native = engine::imagegraph::Write(graph);
		engine::imagegraph::Document checked;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::Read(native, checked, diagnostic) != engine::imagegraph::Status::Ok) {
			errors << "native graph serialization failed: " << diagnostic.Message << '\n';
			return 1;
		}
		std::ofstream file(options.Output, std::ios::binary | std::ios::trunc);
		if (!file || !file.write(native.data(), static_cast<std::streamsize>(native.size())) ||
			!file.flush()) {
			errors << "cannot write native graph: " << options.Output << '\n';
			return 1;
		}
		if (!options.ReferenceRgba.empty()) {
			std::ofstream rgba(options.ReferenceRgba, std::ios::binary | std::ios::trunc);
			if (!rgba ||
				!rgba.write(
					reinterpret_cast<const char *>(reference->Rgba.data()),
					static_cast<std::streamsize>(reference->Rgba.size())
				) ||
				!rgba.flush()) {
				errors << "cannot write source-reference RGBA: " << options.ReferenceRgba << '\n';
				return 1;
			}
			output << "source_reference width=" << reference->Width << " height=" << reference->Height
				   << " format=rgba8 bytes=" << reference->Rgba.size() << " hash=0x" << std::hex
				   << std::setw(16) << std::setfill('0') << reference->Hash << std::dec << '\n';
		}
		for (const auto &entry : imported.Diagnostics)
			errors << "diagnostic node=\"" << entry.NodeId << "\": " << entry.Message << '\n';
		const size_t opaque = options.ExtractNode.empty() ? graph.Nodes.size() - imported.NativeNodes : 0;
		output << "ok nodes=" << graph.Nodes.size() << " native=" << graph.Nodes.size() - opaque
			   << " opaque=" << opaque << " outputs=" << graph.Outputs.size() << '\n';
		if (opaque > 0)
			output << "opaque nodes block native graph evaluation; source reference remains available\n";
		output << "native graph is a projection; keep the original PXCX for lossless source data\n";
		return 0;
	}
}
