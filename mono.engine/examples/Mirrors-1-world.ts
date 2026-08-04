// A mirror, and what it takes to make one — the TypeScript twin of
// `Mirrors-1-world.luau`.
//
// **The same scene, the same bindings, the same result.** Two VMs over one
// binding surface, and the differences below are the languages' rather than the
// engine's:
//
//   - `game.GetService("RunService")` — a dot, because JavaScript has no
//     colon-call.
//   - `a.mul(b)` and `a.add(b)` — methods, because JavaScript has no operator
//     overloading. The transform being composed is identical.
//
// How a mirror works here is in the Luau file and is not repeated: a
// `SurfaceCamera` parented to a part renders into a texture and the part shows
// it, and the image is a frame stale because that is what breaks the dependency
// cycle between a mirror and what it reflects.

const RunService = game.GetService("RunService");

const MIRROR_WIDTH = 16;
const MIRROR_HEIGHT = 9;
const MIRROR_Z = -6;

const EYE = Vector3.new(0, 5, 22);

const mirror = Instance.new("Part");
mirror.Name = "Mirror";
mirror.Anchored = true;
mirror.Size = Vector3.new(MIRROR_WIDTH, MIRROR_HEIGHT, 0.4);
mirror.Position = Vector3.new(0, MIRROR_HEIGHT / 2, MIRROR_Z);
mirror.Color = Color3.new(1, 1, 1);

// No `Surface` line: parenting the camera below is what tells this part which
// texture to show.
mirror.Parent = workspace;

const frameThickness = 0.5;
const bars: [ReturnType<typeof Vector3.new>, ReturnType<typeof Vector3.new>][] = [
	[
		Vector3.new(0, MIRROR_HEIGHT + frameThickness / 2, MIRROR_Z),
		Vector3.new(MIRROR_WIDTH + 1, frameThickness, 0.6),
	],
	[Vector3.new(0, -frameThickness / 2, MIRROR_Z), Vector3.new(MIRROR_WIDTH + 1, frameThickness, 0.6)],
	[
		Vector3.new(-(MIRROR_WIDTH + frameThickness) / 2, MIRROR_HEIGHT / 2, MIRROR_Z),
		Vector3.new(frameThickness, MIRROR_HEIGHT + 1, 0.6),
	],
	[
		Vector3.new((MIRROR_WIDTH + frameThickness) / 2, MIRROR_HEIGHT / 2, MIRROR_Z),
		Vector3.new(frameThickness, MIRROR_HEIGHT + 1, 0.6),
	],
];

for (const [position, size] of bars) {
	const bar = Instance.new("Part");
	bar.Name = "Frame";
	bar.Anchored = true;
	bar.Position = position;
	bar.Size = size;
	bar.Color = Color3.fromRGB(60, 50, 40);
	bar.Parent = workspace;
}

// A shadow needs something to fall on, and a scene of floating cubes has
// nothing.
const floor = Instance.new("Part");
floor.Name = "Floor";
floor.Anchored = true;
floor.Size = Vector3.new(60, 0.5, 60);
floor.Position = Vector3.new(0, -0.25, 0);
floor.Color = Color3.fromRGB(120, 120, 125);
floor.Parent = workspace;

// Deterministic in the index and a salt. Not `Math.random`: a shared generator
// makes the scene depend on the order things were created in.
function hashed(index: number, salt: number): number {
	let value = (index * 0x9e3779b1) ^ (salt * 0x85ebca77);
	value = value ^ (value >>> 15);
	value = (value * 0x2545f491) >>> 0;
	value = value ^ (value >>> 13);
	return (value % 100000) / 100000;
}

function range(index: number, salt: number, low: number, high: number): number {
	return low + hashed(index, salt) * (high - low);
}

const casters = [];

for (let index = 0; index < 24; index++) {
	const part = Instance.new("Part");
	part.Name = "Caster";
	part.Anchored = true;

	const edge = range(index, 31, 0.8, 2.0);
	part.Size = Vector3.new(edge, edge, edge);
	part.Color = Color3.fromRGB(
		range(index, 19, 60, 240),
		range(index, 23, 70, 210),
		range(index, 29, 90, 250)
	);

	const base = Vector3.new(range(index, 3, -12, 12), range(index, 5, 1.5, 7), range(index, 7, -2, 9));
	part.Position = base;
	part.Parent = workspace;

	casters.push({
		part,
		base,
		phase: range(index, 11, 0, 6.283185),
		rate: range(index, 13, 0.4, 1.3),
	});
}

// The surface camera, parented to the pane. The engine mirrors it through the
// face every frame — see `scene/SurfaceCameras.hpp` — so the vector maths that
// used to be here is gone, and the reflection now follows a viewer that moves.
const reflection = Instance.new("SurfaceCamera");
reflection.Name = "Reflection";
reflection.SurfaceSize = Vector3.new(1024, 1024);
reflection.Face = "Front";
reflection.FieldOfView = 70;

// The image's own opacity, which is not the pane's: at 0 the reflection is
// solid whatever the pane's transparency is.
reflection.ImageTransparency = 0;
reflection.Parent = mirror;

const view = Instance.new("Camera");
view.Name = "Viewer";
view.CFrame = CFrame.new(EYE.X, EYE.Y, EYE.Z);
view.FieldOfView = 70;
view.Parent = workspace;
workspace.CurrentCamera = view;

RunService.Heartbeat.Connect((deltaTime: number) => {
	for (const caster of casters) {
		caster.phase += caster.rate * deltaTime;
		caster.part.Position = caster.base.add(Vector3.new(0, Math.sin(caster.phase) * 1.5, 0));
		caster.part.CFrame = caster.part.CFrame.mul(CFrame.Angles(0, caster.rate * deltaTime, 0));
	}
});

print(`mirrors: ${casters.length} casters, one surface camera at 1024x1024`);
