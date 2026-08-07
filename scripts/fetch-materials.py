#!/usr/bin/env python3
"""Downloads public-domain PBR materials into a tree `assetc` can bake.

**A script rather than a tool, and it stops at the staging tree.** What this
produces is *source art* — PNGs and a `.mat` beside them — which is exactly what
`assetc --input` takes. It does not write into the content store, does not bake
and does not publish, because each of those already has one implementation and a
downloader that grew a second would be the format-dialect mistake `AGENTS.md`
names. The three steps in order:

    scripts/fetch-materials.py --out .cache/materials
    assetc --input .cache/materials --output ~/Documents/atomic-game-engine/cdn/raw
    contentimport --publish --key HEX          # or the studio's Publish button

`just materials` is those first two with the paths filled in.

## The three sources

All three are CC0 — public domain, no attribution required — which is the only
reason this script exists in this repository at all. `THIRD_PARTY_NOTICES.md`
carries the entry anyway, because "no attribution required" is a licence term
and not a reason to leave the provenance of six gigabytes unrecorded.

- **ambientCG** — a JSON API, one zip per material.
- **Poly Haven** — a JSON API, one URL per map, no zip.
- **cgbookcase** — no API. The index page is server-rendered and lists every
  material, and each detail page carries the archive's name; the archive itself
  is at a CDN host the page's own script builds a URL for.

**Stdlib only, and deliberately.** This runs once in a while on a developer's
machine to fill a content store; adding a dependency file to the repository for
it would make every clone carry a package manifest for a script most people
never run.

## What lands, and under what name

One directory per source, flat inside it:

    materials/ambientcg/Bricks075A.mat
    materials/ambientcg/Bricks075A_Color.png
    materials/ambientcg/Bricks075A_Normal.png
    materials/ambientcg/Bricks075A_Roughness.png
    materials/ambientcg/Bricks075A_AO.png
    materials/ambientcg/Bricks075A_Height.png

**The map suffixes are normalised and the three sources disagree about all of
them** — `Color`/`BaseColor`/`Diffuse`, `NormalGL`/`nor_gl`/`Normal`. A `.mat`
naming whichever spelling its source happened to use would make the material
format's reference depend on where the pixels came from, which is a dialect in
the one place this engine has been careful not to grow one.

**Only `Color` is named by the `.mat`**, because only a colour map is sampled
today. The other four sit beside it as ordinary textures — published, fetchable,
and waiting for the pass that reads them. A `.mat` key that nothing read would be
half a feature somebody would reasonably assume worked; `ROADMAP.md` v0.11 is
where the other four get one.

## Resumable, because it is six gigabytes over somebody's home connection

Anything already on disk is skipped. A run interrupted halfway and started again
picks up where it stopped, and re-running after a `--count` bump costs only the
new materials.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import io
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

# A browser's, because two of the three refuse anything else. Not an attempt to
# look like something we are not: both sites serve these files to anybody, and a
# default Python agent is simply on a block list somewhere upstream.
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36"

# What the engine calls each map, whatever its source called it.
COLOR, NORMAL, ROUGHNESS, AO, HEIGHT = "Color", "Normal", "Roughness", "AO", "Height"

# The one map a `.mat` names today. See the module docstring.
SAMPLED_MAP = COLOR


def log(message: str) -> None:
    print(message, flush=True)


def fetch(url: str, referer: str | None = None, timeout: int = 300) -> bytes:
    headers = {"User-Agent": USER_AGENT}
    if referer:
        headers["Referer"] = referer
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def fetch_json(url: str, timeout: int = 120) -> object:
    return json.loads(fetch(url, timeout=timeout).decode("utf-8"))


def safe_stem(text: str) -> str:
    """A file name from an asset id, with nothing in it a path cares about."""
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("._-")
    return cleaned or "unnamed"


class Material:
    """One material's maps, before anything has been written."""

    def __init__(self, source: str, name: str) -> None:
        self.source = source
        self.name = safe_stem(name)
        self.maps: dict[str, bytes] = {}

    def write(self, root: Path) -> int:
        """Writes the maps and the `.mat`. Returns bytes written."""
        directory = root / "materials" / self.source
        directory.mkdir(parents=True, exist_ok=True)

        written = 0
        for role, pixels in self.maps.items():
            path = directory / f"{self.name}_{role}.png"
            path.write_bytes(pixels)
            written += len(pixels)

        # **Written last, so an interrupted run leaves no material claiming a
        # colour map that is not beside it.** `exists()` on the `.mat` is what
        # the skip check reads, and a half-written material that answered "done"
        # would be one nobody could tell from a finished one.
        mat = directory / f"{self.name}.mat"
        mat.write_text(
            "# atomic material — see engine/assets/Material.hpp\n"
            "# Paths are relative to this file; assetc rewrites them to baked names.\n"
            f"color = {self.name}_{SAMPLED_MAP}.png\n",
            encoding="utf-8",
        )
        return written

    def done(self, root: Path) -> bool:
        return (root / "materials" / self.source / f"{self.name}.mat").exists()


def unpack_zip(payload: bytes, roles: dict[str, str], material: Material) -> None:
    """Pulls the named members out of an archive under their engine roles.

    `roles` maps a lowercase substring of a member name to a role. First match
    wins, so the more specific patterns must come first in the dict — which is
    why `normalgl` precedes `normal` in every caller.
    """
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        for member in archive.namelist():
            if not member.lower().endswith(".png"):
                continue
            lowered = member.lower()
            for pattern, role in roles.items():
                if pattern in lowered and role not in material.maps:
                    material.maps[role] = archive.read(member)
                    break


# --------------------------------------------------------------------------
# ambientCG
# --------------------------------------------------------------------------

ACG_API = "https://ambientcg.com/api/v2/full_json"

# **`NormalGL` and not `NormalDX`**, and the choice is not arbitrary: OpenGL's
# convention has +Y pointing up in the texture, which is what this engine's
# shaders assume and what glTF specifies. A DirectX-convention map sampled as if
# it were GL lights every bump from the wrong side, which reads as a lighting
# bug rather than as a content one.
ACG_ROLES = {
    "_color": COLOR,
    "_normalgl": NORMAL,
    "_roughness": ROUGHNESS,
    "_ambientocclusion": AO,
    "_displacement": HEIGHT,
}


def ambientcg_list(count: int) -> list[str]:
    query = urllib.parse.urlencode(
        {"type": "Material", "sort": "Popular", "limit": count, "include": "downloadData"}
    )
    payload = fetch_json(f"{ACG_API}?{query}")
    assert isinstance(payload, dict)
    return [asset["assetId"] for asset in payload.get("foundAssets", [])]


def ambientcg_one(asset_id: str, resolution: str) -> Material:
    material = Material("ambientcg", asset_id)
    archive = f"{asset_id}_{resolution}-PNG.zip"
    payload = fetch(f"https://ambientcg.com/get?file={urllib.parse.quote(archive)}")
    unpack_zip(payload, ACG_ROLES, material)
    return material


# --------------------------------------------------------------------------
# Poly Haven
# --------------------------------------------------------------------------

PH_API = "https://api.polyhaven.com"

PH_ROLES = {
    "Diffuse": COLOR,
    "nor_gl": NORMAL,
    "Rough": ROUGHNESS,
    "AO": AO,
    "Displacement": HEIGHT,
}


def polyhaven_list(count: int) -> list[str]:
    payload = fetch_json(f"{PH_API}/assets?type=textures")
    assert isinstance(payload, dict)
    # The API returns a dict keyed by id, carrying a download count. Ordering by
    # it gives the same "common materials first" the other two sources give.
    ranked = sorted(payload.items(), key=lambda row: -row[1].get("download_count", 0))
    return [asset_id for asset_id, _ in ranked[:count]]


def polyhaven_one(asset_id: str, resolution: str) -> Material:
    material = Material("polyhaven", asset_id)
    files = fetch_json(f"{PH_API}/files/{urllib.parse.quote(asset_id)}")
    assert isinstance(files, dict)

    key = resolution.lower()
    for source_role, role in PH_ROLES.items():
        entry = files.get(source_role)
        if not isinstance(entry, dict):
            continue
        url = entry.get(key, {}).get("png", {}).get("url")
        if url:
            material.maps[role] = fetch(url)
    return material


# --------------------------------------------------------------------------
# cgbookcase
# --------------------------------------------------------------------------

CGB_SITE = "https://www.cgbookcase.com"

# Where the site's own thanks-page script sends a browser. Read out of
# `_app/immutable/nodes/*.js` rather than guessed; if it moves, this is the one
# line to change and the failure is a clean 404 rather than silence.
CGB_VOLUME = "https://cgbookcase-volume.b-cdn.net/t/"

CGB_ROLES = {
    "basecolor": COLOR,
    "normal": NORMAL,
    "roughness": ROUGHNESS,
    "_ao": AO,
    "height": HEIGHT,
}


def cgbookcase_list(count: int) -> list[str]:
    page = fetch(f"{CGB_SITE}/textures").decode("utf-8", "replace")
    slugs = re.findall(r'href="/textures/([a-z0-9][a-z0-9-]*)"', page)

    # The index also links its own filters and the thanks page; keep the order
    # the page gave, drop the duplicates and the two that are not materials.
    seen: set[str] = set()
    ordered: list[str] = []
    for slug in slugs:
        if slug in seen or slug in {"thanks"}:
            continue
        seen.add(slug)
        ordered.append(slug)
    return ordered[:count]


def cgbookcase_one(slug: str, resolution: str) -> Material:
    detail = fetch(f"{CGB_SITE}/textures/{slug}", referer=f"{CGB_SITE}/textures").decode("utf-8", "replace")

    # The download button is a link to a thanks page carrying the archive name
    # in `t` and the material's own name in `u`. Both are wanted: `u` is what
    # the maps inside are prefixed with, and it is not derivable from the slug —
    # `asphalt-01` is `Asphalt01`.
    match = re.search(r'href="/textures/thanks\?t=([^"&]+)&(?:amp;)?r=(\d)&(?:amp;)?u=([^"&]+)"', detail)
    if not match:
        raise RuntimeError("no download link on the detail page")

    archive, name = urllib.parse.unquote(match.group(1)), urllib.parse.unquote(match.group(3))

    # The site names archives by resolution — `Asphalt01_MR_4K.zip`. The link on
    # the page is whichever the button defaults to, so the wanted one is that
    # name with the resolution swapped.
    archive = re.sub(r"_(\d+K)\.zip$", f"_{resolution}.zip", archive)

    material = Material("cgbookcase", name)
    payload = fetch(CGB_VOLUME + urllib.parse.quote(archive), referer=f"{CGB_SITE}/")
    unpack_zip(payload, CGB_ROLES, material)
    return material


SOURCES = {
    "ambientcg": (ambientcg_list, ambientcg_one),
    "polyhaven": (polyhaven_list, polyhaven_one),
    "cgbookcase": (cgbookcase_list, cgbookcase_one),
}


def run_source(source: str, count: int, resolution: str, root: Path, workers: int) -> tuple[int, int, int]:
    lister, fetcher = SOURCES[source]

    log(f"[{source}] listing…")
    try:
        ids = lister(count)
    except Exception as failure:  # noqa: BLE001 — one dead source must not stop the others
        log(f"[{source}] could not list: {failure}")
        return 0, 0, 0

    log(f"[{source}] {len(ids)} material(s)")

    done = skipped = written = 0

    def one(asset_id: str) -> tuple[str, int, str]:
        placeholder = Material(source, asset_id)
        if placeholder.done(root):
            return asset_id, -1, ""
        try:
            material = fetcher(asset_id, resolution)
        except Exception as failure:  # noqa: BLE001
            return asset_id, 0, str(failure)

        # **A material with no colour map is not written at all.** The `.mat`
        # names one, so writing the rest would publish a material referencing an
        # asset nobody has — which draws as the default and gives no clue why.
        if SAMPLED_MAP not in material.maps:
            return asset_id, 0, "no colour map in the archive"
        return asset_id, material.write(root), ""

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for asset_id, bytes_written, failure in pool.map(one, ids):
            if bytes_written < 0:
                skipped += 1
            elif failure:
                log(f"[{source}] {asset_id}: {failure}")
            else:
                done += 1
                written += bytes_written
                log(f"[{source}] {asset_id} — {bytes_written / 1_000_000:.1f} MB")

    return done, skipped, written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--out",
        default=".cache/materials",
        help="Staging tree to write. This is assetc's --input, not the content store.",
    )
    parser.add_argument("--count", type=int, default=100, help="How many materials per source")
    parser.add_argument("--resolution", default="1K", choices=["1K", "2K", "4K"])
    parser.add_argument(
        "--sources",
        default=",".join(SOURCES),
        help="Comma-separated subset of " + ", ".join(SOURCES),
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=4,
        help="Concurrent downloads per source. Kept small on purpose: these are "
        "free archives on somebody else's bandwidth.",
    )
    options = parser.parse_args()

    root = Path(options.out).expanduser()
    root.mkdir(parents=True, exist_ok=True)

    chosen = [name.strip() for name in options.sources.split(",") if name.strip()]
    unknown = [name for name in chosen if name not in SOURCES]
    if unknown:
        log(f"unknown source(s): {', '.join(unknown)}")
        return 2

    total_done = total_skipped = total_bytes = 0
    for source in chosen:
        done, skipped, written = run_source(source, options.count, options.resolution, root, options.workers)
        total_done += done
        total_skipped += skipped
        total_bytes += written

    log(
        f"\n{total_done} material(s) written, {total_skipped} already present, "
        f"{total_bytes / 1_000_000_000:.2f} GB into {root}"
    )
    log("next: assetc --input " + str(root) + " --output ~/Documents/atomic-game-engine/cdn/raw")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        # Resumable by construction — say so rather than printing a traceback.
        log("\ninterrupted; re-run to carry on where this stopped")
        sys.exit(130)
