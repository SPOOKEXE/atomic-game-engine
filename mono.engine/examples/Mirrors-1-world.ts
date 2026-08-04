// A room made of mirrors — the TypeScript twin of `Mirrors-1-world.luau`.
//
// **The same scene, the same bindings, the same result.** Two VMs over one
// binding surface, and the differences below are the languages' rather than the
// engine's:
//
//   - `game.GetService("RunService")` — a dot, because JavaScript has no
//     colon-call.
//   - `a.mul(b)` and `a.add(b)` — methods, because JavaScript has no operator
//     overloading. The transform being composed is identical.
//   - `for (const wall of WALLS)` rather than a generic `for ... in`.
//
// How a mirror works here is in the Luau file and is not repeated: a
// `SurfaceCamera` parented to a part renders into a texture and the part shows
// it, and the image is a frame stale because that is what breaks the dependency
// cycle between a mirror and what it reflects.
//
// **Four walls, four surfaces, and each one shows the other three**, which is
// also in the Luau file and is the fact most worth carrying across. Two things
// make it work: `scene::AimSurfaceCameras` gives every camera a texture of its
// own, and the surface pass excludes only the surface it is rendering *for*
// rather than every mirror. What a mirror shows of another mirror is one frame old,
// because each is being rendered for the others and no order would let any of
// them be ready first.
//
// They shared index 0 until v0.8, so all four wrote into one target and three
// walls projected the fourth's image across themselves.

const RunService = game.GetService("RunService");

// The baseplate, and therefore the room. Everything is placed off these three
// numbers so the enclosure cannot drift out of agreement with the floor it
// stands on.
const PLATE = 60;
const PLATE_HALF = PLATE / 2;

const WALL_HEIGHT = 20;
const WALL_THICKNESS = 0.4;

// Outside the plate, not on its edge: the wall's inward face lands exactly on
// the baseplate boundary and the floor runs to the glass with no seam.
const WALL_OFFSET = PLATE_HALF + WALL_THICKNESS / 2;

// How far from the middle the cubes are allowed to wander. Well inside the
// walls — a caster clipping through a mirror reads as a broken reflection
// rather than as a cube in the wrong place.
const CASTER_SPREAD = 12;

// A position only: the reflection is worked out from wherever this camera is,
// every frame.
//
// **Behind the cubes rather than among them**, which is why this is further out
// than `CASTER_SPREAD`: a caster is placed anywhere in that square, so an eye
// inside it eventually has one spawn in its face and fill the screen.
const EYE = Vector3.new(0, 7, 18);
const EYE_FOV = 60;

// A shadow needs something to fall on, and a scene of floating cubes has
// nothing.
const floor = Instance.new("Part");
floor.Name = "Baseplate";
floor.Anchored = true;
floor.Size = Vector3.new(PLATE, 0.5, PLATE);
floor.Position = Vector3.new(0, -0.25, 0);
floor.Color = Color3.fromRGB(120, 120, 125);
floor.Parent = workspace;

// **Axis-aligned, so there is no rotation anywhere in this file.** `Face` is
// resolved in the part's local space and then rotated into the world, so an
// unrotated wall with the right face already has the right inward normal:
// `Back` is +Z, `Front` is -Z, `Right` is +X, `Left` is -X, and each of the four
// below is the one that points at the origin.
//
// The X walls run the plate's exact depth and the Z walls overrun by a
// thickness at each end, so the corners meet flush instead of overlapping into
// a z-fighting post.
const WALLS: {
	name: string;
	position: Vector3;
	size: Vector3;
	face: Enum_NormalId;
}[] = [
	{
		name: "MirrorNorth",
		position: Vector3.new(0, WALL_HEIGHT / 2, -WALL_OFFSET),
		size: Vector3.new(PLATE + WALL_THICKNESS * 2, WALL_HEIGHT, WALL_THICKNESS),
		face: Enum.NormalId.Back,
	},
	{
		name: "MirrorEast",
		position: Vector3.new(WALL_OFFSET, WALL_HEIGHT / 2, 0),
		size: Vector3.new(WALL_THICKNESS, WALL_HEIGHT, PLATE),
		face: Enum.NormalId.Left,
	},
	{
		name: "MirrorWest",
		position: Vector3.new(-WALL_OFFSET, WALL_HEIGHT / 2, 0),
		size: Vector3.new(WALL_THICKNESS, WALL_HEIGHT, PLATE),
		face: Enum.NormalId.Right,
	},
	{
		name: "MirrorSouth",
		position: Vector3.new(0, WALL_HEIGHT / 2, WALL_OFFSET),
		size: Vector3.new(PLATE + WALL_THICKNESS * 2, WALL_HEIGHT, WALL_THICKNESS),
		face: Enum.NormalId.Front,
	},
];

// Wide rather than square, at the same texel count: a wall is four times as
// wide as it is tall and a square target spends half its texels on sky above
// it. All four declare the same size because all four walls are the same shape,
// not because they share a target — each index owns its own pair.
const SURFACE_SIZE = Vector3.new(2048, 512);

for (const wall of WALLS) {
	const pane = Instance.new("Part");
	pane.Name = wall.name;
	pane.Anchored = true;
	pane.Position = wall.position;
	pane.Size = wall.size;

	// White, because the tint multiplies whatever the reflection carries.
	pane.Color = Color3.new(1, 1, 1);

	// **Not a caster.** A twenty-metre wall on every side would rake the floor
	// with its own shadow and bury the cube shadows this scene exists to show.
	pane.CastShadow = false;

	// Nothing here says which texture this pane shows: a pane is a mirror
	// because a `SurfaceCamera` is parented to it, and `scene::AimSurfaceCameras`
	// hands out the slots and writes both ends of the pairing itself.
	pane.Parent = workspace;

	const reflection = Instance.new("SurfaceCamera");
	reflection.Name = "Reflection";
	reflection.SurfaceSize = SURFACE_SIZE;
	reflection.Face = wall.face;

	// A texture of its own, and nothing here asks for one. Every camera used to
	// share slot 0, so three walls projected the fourth's image across
	// themselves; the slots are now handed out in entity order by the engine.

	// Wide enough to still cover the pane when the viewer walks up to it: the
	// reflected camera stands as far behind the glass as the eye is in front,
	// so approaching the wall shortens that distance and a narrow lens would
	// start cropping the pane's edges.
	reflection.FieldOfView = 70;

	// The image's own opacity, which is not the pane's: at 0 the reflection is
	// solid whatever the pane's transparency is.
	reflection.ImageTransparency = 0;
	reflection.Parent = pane;
}

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

	// **Symmetric in X and Z, which the single-pane version was not.** It spread
	// the cubes across a band in front of one mirror; four walls want a cluster
	// in the middle that every one of them has something to reflect.
	const base = Vector3.new(
		range(index, 3, -CASTER_SPREAD, CASTER_SPREAD),
		range(index, 5, 1.5, 8),
		range(index, 7, -CASTER_SPREAD, CASTER_SPREAD)
	);
	part.Position = base;
	part.Parent = workspace;

	casters.push({
		part,
		base,
		phase: range(index, 11, 0, 6.283185),
		rate: range(index, 13, 0.4, 1.3),
	});
}

// The camera the scene is watched through. An identity rotation looks down -Z,
// so this faces the north wall — the one built first and therefore the one that
// reflects.
const view = Instance.new("Camera");
view.Name = "Viewer";
view.CFrame = CFrame.new(EYE.X, EYE.Y, EYE.Z);
view.FieldOfView = EYE_FOV;
view.Parent = workspace;
workspace.CurrentCamera = view;

RunService.Heartbeat.Connect((deltaTime: number) => {
	for (const caster of casters) {
		caster.phase += caster.rate * deltaTime;
		caster.part.Position = caster.base.add(Vector3.new(0, Math.sin(caster.phase) * 1.5, 0));
		caster.part.CFrame = caster.part.CFrame.mul(CFrame.Angles(0, caster.rate * deltaTime, 0));
	}
});

print(`mirrors: ${casters.length} casters, ${WALLS.length} mirrored walls, each on a surface of its own`);
