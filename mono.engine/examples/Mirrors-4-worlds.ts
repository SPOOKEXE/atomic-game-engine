// Four worlds, four view producers, one composited frame - the TypeScript twin
// of `Mirrors-4-worlds.luau`.
//
// **The same scene, the same bindings, the same result.** The differences below
// are the languages' rather than the engine's, and they are the same three the
// single-world pair already carries:
//
//   - `game.GetService("RunService")` - a dot, because JavaScript has no
//     colon-call.
//   - `a.mul(b)` and `a.add(b)` - methods, because JavaScript has no operator
//     overloading.
//   - `for (const caster of casters)` rather than a generic `for ... in`.
//
// Why four worlds and how one file tells them apart is in the Luau file and is
// not repeated here. The short version: `--worlds N` runs the same script in
// every world, `game.JobId` is the world's name, and the trailing number is
// this scene's index. Four rather than two because two of anything is the count
// at which a placement bug still looks like correct behaviour.
//
//     just run --script assets/examples/Mirrors-4-worlds.ts --worlds 4

const RunService = game.GetService("RunService");

// --- which world this is ------------------------------------------------------

// The trailing number of the world's name, or zero when it has none. The first
// world is `client.world` rather than `client.world.0`, so a script that
// assumed a suffix would fail on exactly one of the four.
function worldIndex(): number {
	const trailing = /\.(\d+)$/.exec(game.JobId);
	return trailing !== null ? Number(trailing[1]) : 0;
}

const INDEX = worldIndex();

// --- what makes each world tell itself apart ----------------------------------

// Warm to cool, left to right: the compositor places world 0 leftmost, so
// reading the frame left to right should read this table top to bottom.
const PALETTES = [
	{ name: "Ember", wall: Color3.fromRGB(196, 84, 72), casters: 6 },
	{ name: "Amber", wall: Color3.fromRGB(214, 158, 74), casters: 9 },
	{ name: "Moss", wall: Color3.fromRGB(126, 178, 96), casters: 12 },
	{ name: "Slate", wall: Color3.fromRGB(96, 152, 196), casters: 15 },
];

// Wraps rather than clamps, so `--worlds 6` still builds six distinguishable
// rooms. Zero-based on both sides here, unlike the Luau file - which is the one
// place the two genuinely differ rather than merely reading differently.
const PALETTE = PALETTES[INDEX % PALETTES.length];

// --- the room -----------------------------------------------------------------

const PLATE = 40;
const PLATE_HALF = PLATE / 2;

const WALL_HEIGHT = 14;
const WALL_THICKNESS = 0.4;

// Outside the plate rather than on its edge, so the floor runs to the glass
// with no seam of background between them.
const WALL_OFFSET = PLATE_HALF + WALL_THICKNESS / 2;

const CASTER_SPREAD = 9;

// Every world uses the same eye, and the compositor is what moves the views
// apart - which is the property under test: a world does not know where its
// image will be drawn.
const EYE = Vector3.new(0, 6, 16);
const EYE_FOV = 60;

const floor = Instance.new("Part");
floor.Name = "Baseplate";
floor.Anchored = true;
floor.Size = Vector3.new(PLATE, 0.5, PLATE);
floor.Position = Vector3.new(0, -0.25, 0);
floor.Color = Color3.new(
	0.15 + PALETTE.wall.R * 0.15,
	0.15 + PALETTE.wall.G * 0.15,
	0.15 + PALETTE.wall.B * 0.15
);
floor.Parent = workspace;

// One mirrored wall rather than four: four walls each reflecting the other
// three is the single-world scene's subject and would cost sixteen surface
// targets across this one.
const pane = Instance.new("Part");
pane.Name = "Mirror";
pane.Anchored = true;
pane.Position = Vector3.new(0, WALL_HEIGHT / 2, -WALL_OFFSET);
pane.Size = Vector3.new(PLATE + WALL_THICKNESS * 2, WALL_HEIGHT, WALL_THICKNESS);

// The tint multiplies whatever the reflection carries, so this is what makes
// one world's mirror legibly not another's.
pane.Color = PALETTE.wall;

// Not a caster: a wall this size would rake the floor with its own shadow.
pane.CastShadow = false;
pane.Parent = workspace;

const reflection = Instance.new("SurfaceCamera");
reflection.Name = "Reflection";
reflection.SurfaceSize = Vector3.new(1024, 256);

// `Back` is +Z on an unrotated part, which is the face pointing at the origin
// for a wall on -Z. Nothing in this file rotates anything.
reflection.Face = Enum.NormalId.Back;
reflection.ImageTransparency = 0;
reflection.Parent = pane;

// --- the things it reflects ---------------------------------------------------

// Deterministic placement, salted with the world index so the four rooms are
// not the same room in four colours.
//
// **Written with `Math.imul` and `>>>`, which the Luau file spells with
// `bit32`.** The arithmetic is identical: JavaScript numbers are doubles, so a
// bare `*` on two 32-bit values loses the low bits to rounding, and `imul` is
// the multiply that does not.
function hashed(index: number, salt: number): number {
	let value = Math.imul(index, 0x9e3779b1) ^ Math.imul(salt + INDEX * 7919, 0x85ebca77);
	value = value ^ (value >>> 15);
	value = Math.imul(value, 0x2545f491) >>> 0;
	value = value ^ (value >>> 13);
	return (value % 100000) / 100000;
}

function range(index: number, salt: number, low: number, high: number): number {
	return low + hashed(index, salt) * (high - low);
}

type Caster = { part: Part; base: Vector3; phase: number; rate: number };
const casters: Caster[] = [];

for (let index = 0; index < PALETTE.casters; index++) {
	const part = Instance.new("Part");
	part.Name = "Caster";
	part.Anchored = true;

	const edge = range(index, 31, 0.8, 1.8);
	part.Size = Vector3.new(edge, edge, edge);

	// Pulled toward this world's palette rather than fully random, so a cube
	// belongs to a room visibly.
	part.Color = Color3.new(
		PALETTE.wall.R * 0.5 + range(index, 19, 0.0, 0.5),
		PALETTE.wall.G * 0.5 + range(index, 23, 0.0, 0.5),
		PALETTE.wall.B * 0.5 + range(index, 29, 0.0, 0.5)
	);

	const base = Vector3.new(
		range(index, 3, -CASTER_SPREAD, CASTER_SPREAD),
		range(index, 5, 1.5, 6),
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

// --- the eye ------------------------------------------------------------------

const view = Instance.new("Camera");
view.Name = "Viewer";
view.CFrame = CFrame.new(EYE.X, EYE.Y, EYE.Z);
view.FieldOfView = EYE_FOV;
view.Parent = workspace;
workspace.CurrentCamera = view;

// --- what moves ---------------------------------------------------------------

// Each world runs at its own rate, so the four views are visibly out of step -
// which is what proves they are four simulations rather than one image drawn
// four times.
const RATE = 0.7 + INDEX * 0.25;

RunService.Heartbeat.Connect((deltaTime: number) => {
	for (const caster of casters) {
		caster.phase += caster.rate * RATE * deltaTime;
		caster.part.Position = caster.base.add(Vector3.new(0, Math.sin(caster.phase) * 1.2, 0));
		caster.part.CFrame = caster.part.CFrame.mul(CFrame.Angles(0, caster.rate * RATE * deltaTime, 0));
	}
});

print(
	`mirrors[${INDEX}] '${PALETTE.name}': ${casters.length} casters, 1 mirrored wall, world '${game.JobId}'`
);
