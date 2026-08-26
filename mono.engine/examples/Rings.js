// Rings of orbiting, spinning parts - the JavaScript twin of `Rings.luau`.
//
// **The same scene, the same bindings, the same result.** Two VMs over one
// binding surface: `game.GetService`, `Instance.new`, `Vector3`, `Color3`,
// `CFrame`, `RunService.Heartbeat`. Neither language is transpiled into the
// other and neither is the real one.
//
// Two things read differently, and both are the language rather than the
// engine:
//
//   - `game.GetService("RunService")` - a dot, because JavaScript has no
//     colon-call. Luau writes `game:GetService`.
//   - `a.mul(b)` - a method, because JavaScript has no operator overloading.
//     Luau writes `a * b`. The transform being composed is identical.
//
// Neither language is made to pretend it is the other. What is shared is the
// class table, the property surface and the conversions underneath them.

const RunService = game.GetService("RunService");

const COUNT = 512;
const PER_RING = 64;
const INNER_RADIUS = 3;
const OUTER_RADIUS = 12;

const rings = Math.max(1, Math.ceil(COUNT / PER_RING));
const ringStep = rings > 1 ? (OUTER_RADIUS - INNER_RADIUS) / (rings - 1) : 0;

// Deterministic in the index and a salt. Not `Math.random`: a shared generator
// makes the scene depend on the order things were created in.
function hashed(index, salt) {
	let value = (index * 0x9e3779b1) ^ (salt * 0x85ebca77);
	value = value ^ (value >>> 15);
	value = (value * 0x2545f491) >>> 0;
	value = value ^ (value >>> 13);
	return (value % 100000) / 100000;
}

function range(index, salt, low, high) {
	return low + hashed(index, salt) * (high - low);
}

// Orbit state lives in the script, because how a part moves is the game's
// business. The engine holds the part.
//
// **No part on the record.** The orbiters and `parts` below are the same list
// in the same order, and `BulkMoveTo` wants that list as a list - a handle here
// as well would be a second copy to keep in step with it.
const orbiters = [];

// The two lists `BulkMoveTo` takes. The parts never change, so that half is
// filled below once; the placements are overwritten in place every beat.
const parts = [];
const placements = [];

for (let index = 0; index < COUNT; index++) {
	const ring = Math.floor(index / PER_RING);
	const withinRing = index % PER_RING;

	const part = Instance.new("Part");
	part.Name = "Orbiter";

	// The whole edge, because `Size` is a full extent. The engine stores half
	// of one and the conversion halves it.
	const edge = range(index, 31, 0.6, 1.4);
	part.Size = Vector3.new(edge, edge, edge);
	part.Color = Color3.fromRGB(
		range(index, 19, 40, 230),
		range(index, 23, 50, 205),
		range(index, 29, 90, 240)
	);

	// Nothing here is simulated by physics; the script places every part every
	// beat. Anchoring is a real archetype move rather than a flag.
	part.Anchored = true;
	part.Parent = workspace;

	parts.push(part);
	placements.push(part.CFrame);

	orbiters.push({
		radius: INNER_RADIUS + ring * ringStep,
		height: range(index, 7, -5, 5),
		// Outer rings turn more slowly, which reads as depth without any depth
		// cue in the shading.
		speed: 0.45 / (1 + ring * 0.35),
		angle: (withinRing / PER_RING) * Math.PI * 2,
		spin: CFrame.new(),
		spinX: range(index, 11, -1.2, 1.2),
		spinY: range(index, 13, -1.2, 1.2),
		spinZ: range(index, 17, -1.2, 1.2),
	});
}

// **One `BulkMoveTo` at the end rather than a write inside the loop**, matching
// the Luau twin. A per-part `CFrame =` crosses the language boundary once per
// orbiter, and the boundary is most of what a move costs - `scene::BulkMoveTo`
// carries the measurement.
RunService.Heartbeat.Connect((deltaTime) => {
	for (let at = 0; at < orbiters.length; at++) {
		const orbiter = orbiters[at];
		orbiter.angle += orbiter.speed * deltaTime;

		// The part's own turn, accumulated by composition - the same thing
		// `spin *= CFrame.Angles(...)` does in Luau.
		orbiter.spin = orbiter.spin.mul(
			CFrame.Angles(
				orbiter.spinX * deltaTime,
				orbiter.spinY * deltaTime,
				orbiter.spinZ * deltaTime
			)
		);

		// Turn about the origin, step out along the ring, then apply the spin.
		placements[at] = CFrame.Angles(0, orbiter.angle, 0)
			.mul(CFrame.new(orbiter.radius, orbiter.height, 0))
			.mul(orbiter.spin);
	}

	workspace.BulkMoveTo(parts, placements);
});

print(`rings: ${COUNT} parts across ${rings} ring(s), animated from script`);
