#pragma once

// A plugin written to a scratch folder, and declarations small enough to read.

#include <engine/testing/Suite.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fixture {

	constexpr const char *DECLARATIONS = R"(
declare extern type Instance with
	Name: string
end

declare script: any
declare plugin: Instance
)";

	// plugin/                       a plugin (plugin.json) and the root
	//   src/main.luau               passes a string where core/Add wants a number
	//   src/core/Add.luau
	//   src/Chain.luau              the same mistake through script.Parent
	//   src/Escape.luau             requires a file beside the plugin
	// outside.luau                  not part of the plugin
	// tests/lune.luau               not part of the plugin, so never checked
	struct Plugin {
		std::filesystem::path Folder;

		Plugin() {
			std::random_device random;
			Folder = std::filesystem::temp_directory_path() / ("scriptcheck-" + std::to_string(random()));
			Write("plugin/plugin.json", R"({ "name": "Fixture", "id": "fixture", "main": "src/main.luau" })");
			Write(
				"plugin/src/core/Add.luau",
				"--!strict\nreturn { add = function(a: number, b: number): number return a + b end }\n"
			);
			Write(
				"plugin/src/main.luau",
				"--!strict\nlocal Add = require(\"./core/Add\")\nlocal total: number = Add.add(\"one\", "
				"2)\nprint(total)\n"
			);
			Write(
				"plugin/src/Chain.luau",
				"--!strict\nlocal Add = require(script.Parent.core.Add)\nprint(Add.add(true, 2))\n"
			);
			Write(
				"plugin/src/Escape.luau",
				"--!strict\nlocal Outside = require(\"../../outside\")\nprint(Outside)\n"
			);
			Write("outside.luau", "return 1\n");
			Write("tests/lune.luau", "local fs = require(\"@lune/fs\")\nfs.nothing()\n");
		}

		~Plugin() {
			std::error_code ignored;
			std::filesystem::remove_all(Folder, ignored);
		}

		void Write(const std::string &relative, const std::string &text) const {
			const std::filesystem::path path = Folder / relative;
			std::filesystem::create_directories(path.parent_path());
			std::ofstream(path, std::ios::binary) << text;
		}

		std::filesystem::path At(const std::string &relative) const {
			return Folder / relative;
		}
	};
}
