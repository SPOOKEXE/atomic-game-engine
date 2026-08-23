#!/usr/bin/env bash
#
# The same script-built mesh, photographed by the client and by the editor, and
# the pixel counts that say both drew it.
#
#   scripts/demos/capture-editable-mesh.sh
#   OUT=/tmp/shots scripts/demos/capture-editable-mesh.sh
#
# **The editor drew nothing, and nothing is what a missing upload looks like.**
# `scene::EditableMesh` holds arrays a script wrote; `client::EditableMeshUploader`
# is what turns them into geometry a device can draw, and it is called once per
# frame by whichever program is drawing. `client::Client` called it and
# `studio::Editor` did not - so a `MeshPart` whose `MeshId` is
# `editable-mesh://N` resolved to a name nothing had registered and drew no
# triangles at all, in the viewport, in a Play run and in the studio's own
# capture, while the identical scene under `client --script` was correct.
#
# That is the one shape of bug a single capture cannot report: a picture of the
# editor alone is just a picture of an empty plate. So this runs both and
# compares, and the comparison is the check.
#
# The scene is written here rather than shipped because it has to reach the two
# programs by different doors: the client takes `--script`, and the editor takes
# a Rojo project. One source, two wrappers.
#
# Both processes are killed rather than waited for: there is a shutdown race in
# the GPU teardown that predates this - `NON-EUCLIDEAN.md` Appendix A - and both
# captures are written well before the exit that hangs.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

preset=${PRESET:-dev}
build="$root/.cache/build/$preset"
out=${OUT:-$build/captures}

cmake -S "$root" --preset "$preset" > /dev/null
cmake --build "$build" --target client
cmake --build "$build" --target studio

mkdir -p "$out/editable-mesh/src"

# A pyramid in a colour nothing else in either scene wears, so counting it is
# unambiguous, and a plain `Part` of the same size beside it. The control is
# what separates "the mesh did not upload" from "the camera is looking
# elsewhere": a run with neither is a bad viewpoint and a run with only the
# control is this bug.
cat > "$out/editable-mesh/src/Probe.server.luau" <<'PROBE'
-- **The editor's first world carries a full-screen interface example**, and a
-- `ScreenGui` is drawn over the viewport it belongs to - so a capture of the
-- world behind it is a capture of the interface. Cleared every frame rather
-- than once, because the example rebuilds its own.
local RunService = game:GetService("RunService")
RunService.Heartbeat:Connect(function()
	for _, service in { game:GetService("StarterGui"), game:GetService("Players") } do
		for _, child in service:GetDescendants() do
			if child:IsA("ScreenGui") then
				child:Destroy()
			end
		end
	end
end)

local HALF = 3.0
local APEX = 6.0

local mesh = Instance.new("EditableMesh")
local red = Color3.fromRGB(255, 40, 40)
assert(mesh:SetGeometry({
	{ Position = Vector3.new(-HALF, 0, -HALF), Color = red },
	{ Position = Vector3.new(HALF, 0, -HALF), Color = red },
	{ Position = Vector3.new(HALF, 0, HALF), Color = red },
	{ Position = Vector3.new(-HALF, 0, HALF), Color = red },
	{ Position = Vector3.new(0, APEX, 0), Color = red },
}, {
	0, 1, 2,
	0, 2, 3,
	1, 0, 4,
	2, 1, 4,
	3, 2, 4,
	0, 3, 4,
}), "bulk editable mesh transaction failed")

local built = Instance.new("MeshPart")
built.Name = "ProbePyramid"
built.Anchored = true
built.Size = Vector3.new(10, 10, 10)
built.Position = Vector3.new(0, 5, 0)
built.MeshId = mesh.ContentId
built.Parent = workspace

local control = Instance.new("Part")
control.Name = "ProbeControl"
control.Anchored = true
control.Size = Vector3.new(4, 10, 4)
control.Position = Vector3.new(-4, 5, 0)
control.Color = Color3.fromRGB(40, 255, 40)
control.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "ProbeViewer"
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(Vector3.new(18, 14, 18), Vector3.new(-2, 5, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

print("editable-mesh probe: " .. tostring(mesh.TriangleCount) .. " triangle(s) built")
PROBE

cat > "$out/editable-mesh/default.project.json" <<'PROJECT'
{
  "name": "EditableMeshProbe",
  "tree": {
    "$className": "DataModel",
    "ServerScriptService": {
      "$className": "ServerScriptService",
      "$path": "src"
    }
  }
}
PROJECT

# The client wants a whole scene rather than a script in a world, so it gets the
# probe with a floor and a camera wrapped round it.
{
	cat <<'HEAD'
local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(80, 1, 80)
floor.Position = Vector3.new(-2, -0.5, 0)
floor.Color = Color3.fromRGB(118, 120, 126)
floor.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "Viewer"
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(Vector3.new(18, 14, 18), Vector3.new(-2, 5, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
HEAD
	cat "$out/editable-mesh/src/Probe.server.luau"
} > "$out/editable-mesh/Probe.luau"

shot_client="$out/editable-mesh-client.bmp"
shot_studio="$out/editable-mesh-studio.bmp"
rm -f "$shot_client" "$shot_studio"

echo "capturing the client"
timeout --signal=KILL 120 "$build/client/client" \
	--script "$out/editable-mesh/Probe.luau" --frames 30 --capture "$shot_client" > /dev/null 2>&1 || true

# **`--run server`, so the viewport shows the authored world rather than a play
# client's replica**, and 60 frames because a Rojo sync, three worlds and the
# editor's own layout all settle before the picture is worth taking.
echo "capturing the editor"
timeout --signal=KILL 240 "$build/studio/studio" \
	--headless --frames 60 --run server \
	--rojo "$out/editable-mesh/default.project.json" \
	--capture-world Rings --capture "$shot_studio" \
	--width 1600 --height 900 --config-root "$out/editable-mesh/config" > /dev/null 2>&1 || true

for shot in "$shot_client" "$shot_studio"; do
	if [ ! -f "$shot" ]; then
		echo "no capture written to $shot - run the program by hand to see why" >&2
		exit 1
	fi
done

python3 "$here/editable-mesh-report.py" "$shot_client" "$shot_studio"

echo "stills in $out"
