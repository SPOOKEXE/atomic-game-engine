"""Inspect five external PXCX projects without copying their bytes.

Usage: python3 scripts/tests/pxcx-external-acceptance.py DIRECTORY
"""

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def inspect(path: Path) -> dict[str, object]:
    raw = path.read_bytes()
    require(len(raw) >= 16 and raw[:4] == b"PXCX", "missing PXCX header")
    graph_offset = struct.unpack_from("<I", raw, 4)[0]
    at = 8
    thumbnail = b""
    if raw[at : at + 4] == b"THMB":
        length = struct.unpack_from("<I", raw, at + 4)[0]
        require(at + 8 + length <= len(raw), "truncated THMB block")
        thumbnail = zlib.decompress(raw[at + 8 : at + 8 + length]) if length else b""
        at += 8 + length

    require(raw[at : at + 4] == b"META", "missing META block")
    length = struct.unpack_from("<I", raw, at + 4)[0]
    require(graph_offset == at + 8 + length, "META and graph offsets disagree")
    require(graph_offset < len(raw) and length >= 5, "invalid graph or META span")
    metadata = raw[at + 8 : graph_offset]
    require(metadata[-1] == 0, "META text has no terminator")
    version = struct.unpack_from("<I", metadata)[0]
    version_text = metadata[4:-1].decode("utf-8")

    graph = zlib.decompress(raw[graph_offset:])
    require(graph.endswith(b"\0"), "graph JSON has no terminator")
    document = json.loads(graph[:-1])
    require(document.get("version") == version, "graph and META save versions disagree")
    require(document.get("versions") == version_text, "graph and META version strings disagree")
    nodes = document["nodes"]
    links = sum("from_node" in field for node in nodes for field in node["inputs"])
    return {
        "file": path.name,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "bytes": len(raw),
        "save_version": version,
        "version_string": version_text,
        "nodes": len(nodes),
        "links": links,
        "thumbnail_bytes": len(thumbnail),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="directory containing five external .pxc projects")
    paths = sorted(parser.parse_args().directory.glob("*.pxc"))
    require(len(paths) == 5, f"expected five PXC projects, found {len(paths)}")
    for path in paths:
        print(json.dumps(inspect(path), sort_keys=True))


if __name__ == "__main__":
    main()
