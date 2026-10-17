#!/usr/bin/env python3
"""Run the engine and CDN stress benchmarks and write one markdown table.

    python3 scripts/engine-stress-report.py --build .cache/build/bench --samples 5

`just engine-stress-bench` builds the `bench` preset and runs this. Every suite
in CATALOG runs once through its own binary (`--suite ID --samples N`), and each
`bench` row it prints becomes one table row. Suites a binary declares but the
catalog does not name still run, under a module taken from the suite id, so a
new suite is never silently missing from the report.

A suite that fails, times out or is not built appears as a row saying so; the
report is still written and the exit code is 1.
"""

from __future__ import annotations

import argparse
import datetime
import os
import platform
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


@dataclass(slots=True)
class Entry:
    """One suite run: which module it stresses and what the workload pushes on."""

    suite: str
    module: str
    target: str
    env: dict[str, str] = field(default_factory=dict)
    gpu: bool = False


# Ordered like the 2026-09-22 audit: engine modules first, then CDN.
CATALOG: list[Entry] = [
    Entry("engine.core.bench.names", "core", "Name construction under the shared registry lock."),
    Entry("engine.core.bench.instrumentation", "core", "Metric writes and frame-graph spans: what measuring costs."),
    Entry("engine.core.bench.logging", "core", "Log statements with no listener."),
    Entry("engine.core.bench.serialisation", "core", "Byte layout at snapshot sizes."),
    Entry("engine.core.bench.values", "core", "Value types and the deterministic generator in bulk."),
    Entry("engine.parallel.bench.dispatch", "parallel", "Dispatch handshake floor for empty ranges and pinned ForWorkers tasks."),
    Entry("engine.parallel.bench.contention", "parallel", "Pool cost with real work, ForWorkers placement over 1M rows."),
    Entry("engine.parallel.bench.channel", "parallel", "Framed queue between worlds at message sizes."),
    Entry("engine.parallel.bench.process", "parallel", "Setup cost of a processed job context."),
    Entry("engine.ecs.bench.iteration", "ecs", "Steady-state query iteration per tick."),
    Entry("engine.ecs.bench.structure", "ecs", "Entity shape changes and archetype moves."),
    Entry("engine.world.bench.barrier", "world", "Serial and parallel world ticks, lane balance, bus routing."),
    Entry("engine.collision.bench.triangle-bvh", "collision", "Triangle soup BVH split and leaf policy."),
    Entry("engine.spatial.bench.hashgrid", "spatial", "Broad phase by cell size, hash grid build."),
    Entry("engine.spatial.bench.dynamicbvh", "spatial", "Dynamic BVH rebuild under churn and dense pair sort."),
    Entry("engine.scene.bench.ordering", "scene", "Scene ordering per view per frame."),
    Entry("engine.scene.bench.sunlight", "scene", "Authored lighting resolve per world and frame."),
    Entry("engine.scene.bench.editablemesh", "scene", "Steady collision refresh for presented Studio worlds."),
    Entry("engine.gui.bench.interface", "gui", "Layout per frame at interface tree sizes."),
    Entry("engine.assets.bench.textures", "assets", "Mip chains, same-size resize, texture read refusal."),
    Entry("engine.assets.bench.manifest", "assets", "Manifest lookups and joins."),
    Entry("engine.assets.bench.store", "assets", "Chunks to and from disk."),
    Entry("engine.assets.bench.content", "assets", "Content pipeline throughput without disk."),
    Entry("engine.assets.bench.mesh-decimate", "assets", "Automatic LOD build latency."),
    Entry("engine.physics.bench.stepping", "physics", "Whole ticks: stacks, spread, one dense pile, sleep, raycasts."),
    Entry("engine.physics.bench.pile-cells", "physics", "Dense 4,000-body pile per stage at 4m, 2m and 1m cells."),
    Entry("engine.physics.bench.broadphase", "physics", "Deterministic pair generation, cell sizes, static and kinematic motion."),
    Entry("engine.physics.bench.narrowphase", "physics", "Narrow phase per pair by shape combination."),
    Entry("engine.physics.bench.solver", "physics", "Dense contact solve, islands, topology churn."),
    Entry("engine.physics.bench.integrate", "physics", "Serial and parallel IntegrateMotion crossover."),
    Entry("engine.physics.bench.continuous", "physics", "Conservative advance for continuous collision."),
    Entry("engine.physics.bench.speculative-contacts", "physics", "Speculative contact generation and solve."),
    Entry("engine.effects.bench.particles", "effects", "Emitters at 100,000 scale, age and spawn dispatch."),
    Entry("engine.graph.bench.cull", "graph", "Culling cost and what it saves."),
    Entry("engine.graph.bench.submission", "graph", "Authored render graph to device frame."),
    Entry("engine.game.bench.documents", "game", "Save file read and write."),
    Entry("engine.bake.bench.robloxfile", "bake", "Roblox place bytes to engine tree."),
    Entry("engine.bakegraph.bench.pipeline-set", "bakegraph", "PipelineSet lookup against linear and binary controls."),
    Entry("engine.delivery.bench.cache", "delivery", "On-disk content cache lookups."),
    Entry("engine.delivery.bench.compression", "delivery", "Group compression cost and client saving."),
    Entry("engine.net.bench.framing", "net", "Per-packet framing at server packet rates."),
    Entry("engine.net.bench.reliability", "net", "Reliable channel upkeep and loss recovery."),
    Entry("engine.net.bench.crypto", "net", "Per-packet encryption and connection setup."),
    Entry("engine.net.bench.websocket", "net", "WebSocket framing."),
    Entry("engine.replication.bench.survey", "replication", "Per-tick survey before any client is visited."),
    Entry("engine.replication.bench.publish", "replication", "Authority::Publish per client and parallel crossover."),
    Entry("engine.replication.bench.priority-refinement", "replication", "Scored and refined multi-row publish under a byte budget."),
    Entry("engine.replication.bench.recovery-rows", "replication", "Recovery row re-offer with acknowledgements withheld."),
    Entry("engine.replication.bench.protocol", "replication", "Tick traffic encode and parse."),
    Entry("engine.replication.bench.interpolation", "replication", "Client smoothing per frame and per received tick."),
    Entry("engine.script.bench.source-mirror", "script", "Unchanged Luau source mirror walk."),
    Entry("engine.script.bench.surface", "scripthost", "Per-tick and per-call script surface cost."),
    Entry("engine.scriptjs.bench.property-access", "scriptjs", "QuickJS bound property read and write."),
    Entry("engine.scriptluau.bench.bindings", "scriptluau", "C++ to Luau boundary calls."),
    Entry("engine.scripthost.bench.input-dispatch", "scripthost", "Host input pump through both VM adapters."),
    Entry("engine.input.bench.translate", "input", "A busy frame of platform input: keys, mouse, axes, frame roll."),
    Entry("engine.audio.bench.mixing", "audio", "Mixer segments, command bursts, spatial voices."),
    Entry("engine.render.bench.world-presentation", "render", "Camera batches over shared world rows."),
    Entry("engine.render.bench.presentation", "render", "Publishing moving particles over static parts."),
    Entry("engine.render.bench.instances", "render", "Host work for device-resident instance rows."),
    Entry("engine.render.bench.meshes", "render", "Geometry admission and slot reclaim."),
    Entry("engine.render.bench.interface", "render", "UI triangles at display rate."),
    Entry("engine.render.bench.overlay", "render", "CPU overlay per frame."),
    Entry("engine.render.bench.graphrunner", "render", "Per-view backend dispatch table."),
    Entry("engine.render.bench.data-capture-hooks", "render", "Hook connection validation and backpressure, no device."),
    Entry("engine.render.bench.portal-exchange", "render", "Portal image exchange."),
    Entry("engine.render.bench.portal-ambient", "render", "Portal ambient codec."),
    Entry(
        "engine.render.bench.portal-ambient",
        "render",
        "Portal directional codec.",
        env={"MONO_PORTAL_CODEC_DIRECTIONAL": "1"},
    ),
    Entry("engine.render.bench.gpu-texture-atlas", "render", "GPU texture atlas uploads (device).", gpu=True),
    Entry("engine.render.bench.gpu-particle-field", "render", "GPU particle field (device).", gpu=True),
    Entry(
        "engine.render.bench.data-capture-gpu-profile",
        "render",
        "Data capture GPU profile (device).",
        env={"MONO_DATA_CAPTURE_PROFILE": "1"},
        gpu=True,
    ),
    Entry("engine.ui.bench.headless-interface", "ui", "Headless UI frame and geometry signature hash."),
    Entry("engine.control.bench.mcp-control", "control", "MCP tool discovery and dispatch."),
    Entry("engine.datastore.bench.sqlite-snapshot", "datastore", "SQLite snapshot replace and load."),
    Entry("engine.examples.bench.motion", "examples", "Example orbit and spin systems over Parts."),
    Entry("engine.msl.bench.translate", "msl", "SPIR-V to MSL translation on the CPU."),
    Entry("engine.imagegraph.bench.evaluation", "imagegraph", "Image graph plan evaluation."),
    Entry("engine.imagegraphio.bench.pxcx-import", "imagegraphio", "PXCX project import projection."),
    Entry("cdn.bench.grouping", "cdn", "Assembling assets into streamed groups."),
    Entry("cdn.bench.admission", "cdn", "Per-request grant admission by bundle scope."),
]

# Figures from the 2026-09-22 audit (Ryzen 9 9900X, `bench` preset), kept so a
# new run can be read against them. Several were taken with builds running.
REFERENCES: dict[tuple[str, str], str] = {
    ("engine.world.bench.barrier", "Tick · 2 worlds of 100k, parallel"): "14.95 us",
    ("engine.world.bench.barrier", "Tick · 2 worlds of 100k, serial"): "15.48 us",
    ("engine.world.bench.barrier", "Tick · 4 worlds of 100k, parallel"): "32.02 us",
    ("engine.world.bench.barrier", "Tick · 4 worlds of 100k, serial"): "31.15 us",
    ("engine.world.bench.barrier", "Tick · 200 quiet worlds, serial"): "103.12 us",
    ("engine.world.bench.barrier", "Tick · 200 suspended worlds, serial"): "10.29 us",
    ("engine.physics.bench.stepping", "Tick · 4000 bodies spread out, each touching only the floor"): "0.832 ms",
    ("engine.physics.bench.stepping", "Tick · 4000 bodies in one pile"): "39.19 ms",
    ("engine.physics.bench.broadphase", "Sync + pairs · 4000 colliders, 4m cells"): "321.03 us",
    ("engine.physics.bench.broadphase", "Pairs only · 4000 colliders, 4m cells"): "274.00 us",
    ("engine.physics.bench.solver", "Solve · stable topology, dense contacts"): "1.243 ms",
    ("engine.physics.bench.solver", "Solve · independent stacks with bridge churn"): "2.077 ms",
    ("engine.parallel.bench.dispatch", "For · dispatched, 128 empty ranges"): "39.66 us",
    ("engine.assets.bench.textures", "ResizeImage · 2048x2048 to the same size"): "43.41 ms, 0.39 ms after fix",
    ("engine.assets.bench.textures", "BuildMipChain · 512x512 RGBA8"): "1.204 ms",
    ("engine.assets.bench.textures", "BuildMipChain · 1024x1024 RGBA8"): "3.894 ms",
    ("engine.assets.bench.textures", "BuildMipChain · 2048x2048 RGBA8"): "24.058 ms",
    ("engine.audio.bench.mixing", "Render · 1 voice"): "2,225 ns, 1,973 ns after fix",
    ("engine.bakegraph.bench.pipeline-set", "PipelineSet::Find · 4096 pipelines"): "267 ns",
    ("engine.bakegraph.bench.pipeline-set", "control · linear Find · 4096 pipelines"): "385 ns",
    ("engine.replication.bench.priority-refinement", "Priority refinement · 2k entities × 4 rows · 32 clients"): "2.774 ms, 2.340 ms after fix",
    ("engine.replication.bench.recovery-rows", "Recovery rows · 512 outstanding rows · cap 128"): "446.9 us",
    ("engine.datastore.bench.sqlite-snapshot", "SQLite atomic snapshot replace, 1024 entries x 256 bytes"): "591.7 us",
    ("engine.datastore.bench.sqlite-snapshot", "SQLite snapshot load, 1024 entries x 256 bytes"): "207.7 us",
    ("engine.msl.bench.translate", "SPIRV-Cross translation of four-resource fragment"): "1,849 ns",
    ("engine.imagegraph.bench.evaluation", "256x256 three-octave simplex evaluation"): "59.79 ms",
    ("engine.imagegraphio.bench.pxcx-import", "project 256 opaque nodes and 257 links with source retention"): "1.14 ms",
    ("cdn.bench.grouping", "Assemble · 50k assets, nothing bound together"): "40.78 ms, 8.06 ms after fix",
    ("cdn.bench.admission", "Admits · 100k accepted requests, 1024 bundles in scope"): "28.21 us",
}

# Stress workloads that are not benchmark suites, listed under the table.
OUTSIDE = [
    ("`just preset=release stress-random-motion`", "200 randomly turning connected clients, 45 s; tick p50/p95/p99 and the server flame graph."),
    ("`just preset=release stress`", "200 connected clients with fixed headings."),
    ("`just preset=release stress-motion`", "`ReplicationStress.luau`, 20,000 moving parts."),
    ("`just preset=release integrated-stress`", "Server, CDN and loadtest fetch cohort together."),
    ("`just volume-light-stress 120`", "Light and fog volume selection, Vulkan volume fixture."),
    ("`just preset=release lighting-stress-scene`", "`LightingStress.luau` headless client, `gpu fog` span."),
]


@dataclass(slots=True)
class Row:
    module: str
    target: str
    suite: str
    name: str
    nanoseconds: int | None = None
    spread: int | None = None
    unit: str = ""
    samples: int = 0
    note: str = ""


def discover(build: Path) -> dict[str, Path]:
    """Map every suite id to the benchmark binary that declares it."""
    suites: dict[str, Path] = {}
    directory = build / "bench"
    if not directory.is_dir():
        return suites
    for binary in sorted(directory.iterdir()):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            continue
        try:
            listed = subprocess.run(
                [str(binary), "--mono-suites"], capture_output=True, text=True, timeout=60, check=False
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        for line in listed.stdout.splitlines():
            suite = line.split("\t", 1)[0].strip()
            if suite:
                suites[suite] = binary
    return suites


def run_suite(entry: Entry, binary: Path, samples: int, timeout: int) -> list[Row]:
    """Run one suite and parse its `bench` rows."""
    label = f"{entry.suite} {' '.join(f'{k}={v}' for k, v in entry.env.items())}".strip()
    print(f"  {label}", flush=True)

    failed = Row(entry.module, entry.target, entry.suite, "")
    try:
        result = subprocess.run(
            [str(binary), "--suite", entry.suite, "--samples", str(samples)],
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=binary.parent,
            env={**os.environ, **entry.env},
            check=False,
        )
    except subprocess.TimeoutExpired:
        failed.note = f"timed out after {timeout} s"
        return [failed]
    except OSError as error:
        failed.note = f"did not start: {error}"
        return [failed]

    rows: list[Row] = []
    for line in result.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != 8 or fields[0] != "bench" or fields[1] != entry.suite:
            continue
        rows.append(
            Row(
                entry.module,
                entry.target,
                entry.suite,
                fields[7],
                int(fields[2]),
                int(fields[3]),
                fields[6],
                int(fields[4]),
                REFERENCES.get((entry.suite, fields[7]), ""),
            )
        )

    if result.returncode != 0:
        failed.note = f"exit {result.returncode}"
        return rows + [failed]
    if not rows:
        failed.note = "emitted no rows"
        return [failed]
    return rows


def format_time(nanoseconds: int) -> str:
    """Nanoseconds in the unit that reads without counting zeroes."""
    if nanoseconds < 1_000:
        return f"{nanoseconds} ns"
    if nanoseconds < 1_000_000:
        return f"{nanoseconds / 1_000:.2f} us"
    if nanoseconds < 1_000_000_000:
        return f"{nanoseconds / 1_000_000:.2f} ms"
    return f"{nanoseconds / 1_000_000_000:.2f} s"


def cell(text: str) -> str:
    return text.replace("|", "\\|")


def host_line() -> str:
    """CPU model, thread count and memory, from /proc where available."""
    cpu = platform.processor() or platform.machine()
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    memory = ""
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal"):
                memory = f", {int(line.split()[1]) / 1024 / 1024:.0f} GiB RAM"
                break
    except OSError:
        pass
    return f"{cpu}, {os.cpu_count()} hardware threads{memory}, {platform.system()} {platform.release()}"


def commit_line() -> str:
    def git(*args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=False
        ).stdout.strip()

    dirty = " (dirty tree)" if git("status", "--porcelain") else ""
    return f"`{git('rev-parse', '--short', 'HEAD')}`{dirty}"


def render(rows: list[Row], samples: int, gpu: bool, build: Path) -> str:
    lines = [
        "# Engine stress test",
        "",
        "Generated by `just engine-stress-bench`. Do not edit by hand.",
        "",
        f"- Host: {host_line()}",
        f"- Commit: {commit_line()}",
        f"- Build: `{build.relative_to(ROOT) if build.is_relative_to(ROOT) else build}`, {samples} samples per row after 8 warm-up samples",
        f"- Date: {datetime.date.today().isoformat()}",
        f"- GPU suites: {'run' if gpu else 'skipped, pass `gpu=1`'}",
        "",
        "Min is the fastest sample per iteration. Spread is slowest minus fastest, as a share of min.",
        "Unit `call` is one call of the body; `item` is one of the items the body processes.",
        "A run with other work on the host is workload evidence, not a baseline.",
        "",
        "| Module | Stress target | Suite | Benchmark | Min | Spread | Unit | 2026-09-22 ref |",
        "|---|---|---|---|---:|---:|---|---|",
    ]

    shown: tuple[str, str] | None = None
    for row in rows:
        # The target repeats for every row of a suite; print it once per run.
        key = (row.suite, row.target)
        first = key != shown
        shown = key
        module = f"`{row.module}`" if first else ""
        target = cell(row.target) if first else ""
        suite = f"`{row.suite}`" if first else ""

        if row.nanoseconds is None:
            lines.append(f"| {module} | {target} | {suite} | **{cell(row.note)}** | | | | |")
            continue

        spread = f"{100 * row.spread / row.nanoseconds:.0f}%" if row.nanoseconds else "0%"
        lines.append(
            f"| {module} | {target} | {suite} | {cell(row.name)} | {format_time(row.nanoseconds)} "
            f"| {spread} | {row.unit} | {cell(row.note)} |"
        )

    lines += ["", "Stress workloads outside the benchmark preset:", ""]
    lines += [f"- {command}: {what}" for command, what in OUTSIDE]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, default=ROOT / ".cache/build/bench")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--out", type=Path, default=ROOT / "docs/ENGINE_STRESS_TEST.md")
    parser.add_argument("--timeout", type=int, default=900, help="seconds per suite")
    parser.add_argument("--gpu", action="store_true", help="also run device-owning suites")
    parser.add_argument("--filter", default="", help="only suites whose id contains this")
    arguments = parser.parse_args()

    build = arguments.build.resolve()
    found = discover(build)
    if not found:
        print(f"no benchmark binaries under {build}/bench; build the bench preset first", file=sys.stderr)
        return 1

    # Suites the catalog does not name still run, after the catalogued ones.
    entries = list(CATALOG)
    named = {entry.suite for entry in CATALOG}
    for suite in sorted(found):
        if suite not in named and suite.startswith(("engine.", "cdn.")):
            module = suite.split(".")[1] if suite.startswith("engine.") else "cdn"
            entries.append(Entry(suite, module, "No catalog entry in scripts/engine-stress-report.py."))

    rows: list[Row] = []
    for entry in entries:
        if arguments.filter not in entry.suite or (entry.gpu and not arguments.gpu):
            continue
        binary = found.get(entry.suite)
        if binary is None:
            rows.append(Row(entry.module, entry.target, entry.suite, "", note="not built"))
            continue
        rows.extend(run_suite(entry, binary, arguments.samples, arguments.timeout))

    arguments.out.write_text(render(rows, arguments.samples, arguments.gpu, build))
    broken = [row for row in rows if row.nanoseconds is None]
    print(f"\n{len(rows) - len(broken)} rows, {len(broken)} broken suites -> {arguments.out}")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
