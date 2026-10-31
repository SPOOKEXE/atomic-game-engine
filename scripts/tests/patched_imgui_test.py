"""Exercise the content-addressed ImGui cache using local Git repositories."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]


def run(command: list[str], cwd: Path, env: dict[str, str], *, check: bool = True):
    return subprocess.run(
        command, cwd=cwd, env=env, check=check, capture_output=True, text=True
    )


def main() -> None:
    env = os.environ.copy()
    local_variables = subprocess.run(
        ["git", "rev-parse", "--local-env-vars"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    for variable in local_variables:
        env.pop(variable, None)
    env.update(
        GIT_AUTHOR_NAME="Test",
        GIT_AUTHOR_EMAIL="test@example.invalid",
        GIT_COMMITTER_NAME="Test",
        GIT_COMMITTER_EMAIL="test@example.invalid",
    )
    with tempfile.TemporaryDirectory(prefix="imgui cmake ") as temporary:
        base = Path(temporary)
        upstream = base / "upstream"
        root = base / "project with spaces"
        upstream.mkdir()
        root.mkdir()
        run(["git", "init", "-q"], upstream, env)
        for name in ("imgui.h", "imgui_widgets.cpp"):
            shutil.copy(REPOSITORY / "mono.vendor/imgui" / name, upstream / name)
        run(["git", "add", "."], upstream, env)
        run(["git", "commit", "-qm", "fixture"], upstream, env)
        run(["git", "init", "-q"], root, env)
        run(
            [
                "git",
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "add",
                "-q",
                str(upstream),
                "mono.vendor/imgui",
            ],
            root,
            env,
        )
        patch = root / "mono.vendor/patches/imgui/before-input-callback.patch"
        patch.parent.mkdir(parents=True)
        original_patch = (
            REPOSITORY / "mono.vendor/patches/imgui/before-input-callback.patch"
        )
        shutil.copy(original_patch, patch)
        run(["git", "add", "."], root, env)
        run(["git", "commit", "-qm", "fixture"], root, env)
        script = base / "run.cmake"
        script.write_text(
            f'set(CMAKE_SOURCE_DIR "{root.as_posix()}")\n'
            f'include("{(REPOSITORY / "mono.build/PreparePatchedImgui.cmake").as_posix()}")\n'
            'mono_prepare_patched_imgui(result)\nmessage(STATUS "prepared=${result}")\n'
        )
        command = ["cmake", "-P", str(script)]
        processes = [
            subprocess.Popen(
                command,
                cwd=root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            for _ in range(2)
        ]
        for process in processes:
            stdout, stderr = process.communicate()
            assert process.returncode == 0, (stdout, stderr)
        objects = root / ".cache/vendor/imgui/objects"
        entries = [path for path in objects.iterdir() if path.is_dir()]
        assert len(entries) == 1, entries
        assert "ImGuiInputTextFlags_CallbackBeforeInput" in (
            entries[0] / "imgui.h"
        ).read_text(encoding="utf-8")
        vendor = root / "mono.vendor/imgui"
        run(["git", "checkout", "--detach", "-q"], vendor, env)
        module = (REPOSITORY / "mono.build/PreparePatchedImgui.cmake").as_posix()
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(PatchedImguiProbe NONE)\n"
            f'include("{module}")\n'
            "mono_prepare_patched_imgui(result)\n"
            f'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "{patch.as_posix()}")\n'
            "add_custom_target(probe ALL COMMAND ${CMAKE_COMMAND} -E echo ready)\n"
        )
        build = root / "build"
        run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja"], root, env)
        build_command = ["cmake", "--build", str(build)]
        run(build_command, root, env)
        (root / "unrelated.txt").write_text("unrelated\n", encoding="utf-8")
        run(["git", "add", "unrelated.txt"], root, env)
        result = run(build_command, root, env)
        assert "Re-running CMake" not in result.stdout, result.stdout

        hook_env = env | {
            "GIT_DIR": str(root / ".git"),
            "GIT_PREFIX": "unrelated/",
            "GIT_SHALLOW_FILE": str(root / "missing-shallow-file"),
        }
        run(command, root, hook_env)
        with patch.open("a", encoding="utf-8") as stream:
            stream.write("\n# witness change\n")
        result = run(build_command, root, env)
        assert "Re-running CMake" in result.stdout, result.stdout
        assert len([path for path in objects.iterdir() if path.is_dir()]) == 2
        patch.write_text("not a patch\n", encoding="utf-8")
        assert run(command, root, env, check=False).returncode != 0
        shutil.copy(original_patch, patch)
        with (vendor / "imgui.h").open("a", encoding="utf-8") as stream:
            stream.write("\n// dirty\n")
        assert run(command, root, env, check=False).returncode != 0
        run(["git", "add", "imgui.h"], vendor, env)
        assert run(command, root, env, check=False).returncode != 0
        run(["git", "commit", "-qm", "different head"], vendor, env)
        assert run(command, root, env, check=False).returncode != 0
        run(["git", "add", "mono.vendor/imgui"], root, env)
        result = run(build_command, root, env)
        assert "Re-running CMake" in result.stdout, result.stdout
        assert len([path for path in objects.iterdir() if path.is_dir()]) == 3
        previous = {path for path in objects.iterdir() if path.is_dir()}
        with patch.open("a", encoding="utf-8") as stream:
            stream.write("\n# third key\n")
        run(command, root, env)
        entries = {path for path in objects.iterdir() if path.is_dir()}
        assert len(entries) == 4
        newest = (entries - previous).pop()
        (newest / ".complete").unlink()
        assert run(command, root, env, check=False).returncode != 0

        container = base / "unrelated repository"
        container.mkdir()
        run(["git", "init", "-q"], container, env)
        exported = container / "source export"
        source = exported / "mono.vendor/imgui"
        source.mkdir(parents=True)
        for name in ("imgui.h", "imgui_widgets.cpp"):
            shutil.copy(REPOSITORY / "mono.vendor/imgui" / name, source / name)
        exported_patch = (
            exported / "mono.vendor/patches/imgui/before-input-callback.patch"
        )
        exported_patch.parent.mkdir(parents=True)
        shutil.copy(original_patch, exported_patch)
        export_script = base / "export.cmake"
        export_script.write_text(
            f'set(CMAKE_SOURCE_DIR "{exported.as_posix()}")\n'
            f'include("{(REPOSITORY / "mono.build/PreparePatchedImgui.cmake").as_posix()}")\n'
            "mono_prepare_patched_imgui(result)\n"
        )
        export_command = ["cmake", "-P", str(export_script)]
        run(export_command, exported, env)
        exported_objects = exported / ".cache/vendor/imgui/objects"
        exported_entries = list(exported_objects.iterdir())
        assert len(exported_entries) == 1
        assert "ImGuiInputTextFlags_CallbackBeforeInput" in (
            exported_entries[0] / "imgui.h"
        ).read_text(encoding="utf-8")
        (exported / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(ExportProbe NONE)\n"
            f'include("{module}")\n'
            "mono_prepare_patched_imgui(result)\n"
            "add_custom_target(probe ALL COMMAND ${CMAKE_COMMAND} -E echo ready)\n"
        )
        export_build = exported / "build"
        run(
            ["cmake", "-S", str(exported), "-B", str(export_build), "-G", "Ninja"],
            exported,
            env,
        )
        run(["cmake", "--build", str(export_build)], exported, env)
        with (source / "imgui.h").open("a", encoding="utf-8") as stream:
            stream.write("\n// export changed\n")
        result = run(["cmake", "--build", str(export_build)], exported, env)
        assert "Re-running CMake" in result.stdout, result.stdout
        assert len(list(exported_objects.iterdir())) == 2
    print("patched ImGui cache checks passed")


if __name__ == "__main__":
    main()
