#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


PIN = "7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f"
CHAIN = "e641898384f7"
STAGES = dict(zip(
    ("vertex", "pixel", "geometry", "hull", "domain", "compute", "task", "mesh", "raytracing"),
    ("vs", "ps", "gs", "hs", "ds", "cs", "ts", "ms", "rt"),
))
SIDE_TABLES = ("c1_full.tsv", "cb_extent.tsv")
IMAGE = f"b-{CHAIN}.img"


def require(condition, problem):
    if not condition:
        raise ValueError(problem)


def set_shaders_key(control, code, skip=(0, 0)):
    """Hash a complete offline code area, as SetShaders does for a mapped stage."""
    require(len(control) >= 8, "short control header")
    nvn2 = struct.unpack_from("<II", control) == (0x98761234, 0xC9)
    code_offset = 0x44 if nvn2 else 0x704
    require(len(control) >= code_offset + 4, "short control length field")
    code_len = struct.unpack_from("<I", control, code_offset)[0]
    require(0 < code_len < (1 << 24) and len(code) == code_len, "invalid mapped code length")
    if nvn2:
        control_len, offset, length = 0x48, 0x30, 4
    else:
        own = struct.unpack_from("<I", control, 0x20)[0]
        control_len = own if 0x760 <= own < 0x880 else 0x880
        offset, length = skip
    require(len(control) >= control_len, "short control bytes in key")
    ctl = bytearray(control[:control_len])
    if length and 0 <= offset <= offset + length <= control_len:
        ctl[offset:offset + length] = bytes(length)
    h = hashlib.sha256(ctl)
    h.update(code)
    return h.hexdigest()[:16]


def shader_map(text):
    keys, records = {}, defaultdict(dict)
    for line in text.splitlines():
        if not line or line.startswith(("#", "package")):
            continue
        row = line.split("\t")
        require(len(row) >= 5, "malformed shader map row")
        name, index, stage, key = row[:4]
        require(re.fullmatch(r"[a-zA-Z0-9_-]+\.bin", name) and re.fullmatch(r"[0-9]+", index),
                "invalid shader package reference")
        require(re.fullmatch(r"[0-9a-f]{16}", key) and stage in STAGES, "invalid shader key or stage")
        require(keys.setdefault(key, stage) == stage, "shader key maps to conflicting stages")
        require(records[name].setdefault(int(index), key) == key, "conflicting shader package indices")
    require(keys, "empty shader map")
    return keys, records


def check_package(path, records, skip):
    require({p.name for p in path.glob("*.bin")} == records.keys(), "package file inventory differs from map")
    for name, expected in records.items():
        require(sorted(expected) == list(range(len(expected))), "noncontiguous shader package indices")
        with (path / name).open("rb") as f:
            for index in range(len(expected)):
                head = f.read(0x38)
                require(len(head) == 0x38 and head[:8] == b"BTCDBTCD", "invalid shader package record")
                ctl_size, code_size = struct.unpack_from("<II", head, 0x28)
                require(0 < code_size < 1 << 24 and 0 < ctl_size < 1 << 24, "invalid package record size")
                code, ctl = f.read(code_size), f.read(ctl_size)
                require(len(ctl) == ctl_size and len(code) == code_size, "truncated package record")
                require(head[8:16].hex() == expected[index] == set_shaders_key(ctl, code, skip),
                        "shader map, package and SetShaders keys differ")
            require(not f.read(1), "package contains records outside the shader map")


def reflection(path, stem, key, stage):
    obj = json.loads((path / (stem + ".json")).read_bytes())
    require(isinstance(obj, dict) and
            (obj.get("key"), obj.get("route"), obj.get("chain"), obj.get("dm_ctl"), obj.get("stage"))
            == (stem, "B", CHAIN, key, stage), "reflection key, route, chain or stage differs")
    require(isinstance(obj.get("bindings"), list), "missing reflection bindings")
    return obj


def metallib_entries(path):
    with path.open("rb") as f:
        head = f.read(0x58)
        require(len(head) == 0x58 and head[:4] == b"MTLB", "invalid metallib header")
        size, functions, _, public, _, _, _, code, code_len = struct.unpack_from("<9Q", head, 0x10)
        require(size == path.stat().st_size == code + code_len and 0x58 <= functions <= public - 4 <= code,
                "invalid metallib section bounds")
        require(public - functions <= 1 << 20, "oversized metallib function table")
        f.seek(functions)
        table = f.read(public - functions)
    require(len(table) == public - functions, "truncated metallib function table")
    count = struct.unpack_from("<I", table)[0]
    pos, names = 4, set()
    require(count and count <= (len(table) - pos) // 8, "invalid metallib function count")
    for _ in range(count):
        require(pos + 4 <= len(table), "truncated metallib function record")
        end = pos + struct.unpack_from("<I", table, pos)[0]
        require(pos + 8 <= end <= len(table), "invalid metallib function bounds")
        pos += 4
        name = None
        while pos + 4 <= end:
            tag = table[pos:pos + 4]
            pos += 4
            if tag == b"ENDT":
                break
            require(pos + 2 <= end, "truncated metallib tag")
            length = struct.unpack_from("<H", table, pos)[0]
            pos += 2
            require(pos + length <= end, "invalid metallib tag bounds")
            if tag == b"NAME":
                require(name is None, "duplicate metallib function name tag")
                name = table[pos:pos + length].rstrip(b"\0").decode("utf-8")
            pos += length
        else:
            raise ValueError("unterminated metallib function record")
        require(name and name not in names, "invalid or duplicate metallib function name")
        names.add(name)
        pos = end
    return names


def check_libraries(path, keys, build_rows, permit_dynamic, fetch_required, dual_required, mac_record):
    libraries, gaps = set(), Counter()
    require(isinstance(build_rows, list), "invalid shader build report")
    status = {row["key"]: row["st"] for row in build_rows}
    require(len(status) == len(build_rows), "duplicate keys in shader build report")
    names = {p.name for p in path.iterdir()}

    def check_lib(stem, entries):
        name = stem + ".metallib"
        require(set(entries) <= metallib_entries(path / name), "metallib is missing declared function names")
        libraries.add(name)

    def check_entry(obj):
        require(isinstance(obj.get("entry"), str) and obj["entry"], "missing reflection entry")

    for key, kind in keys.items():
        stage, stem = STAGES[kind], key + "_b"
        try:
            require(status.get(key) == "ok", "map key not built successfully")
            require(stage != "rt", "universal ray tracing is outside this preflight")
            obj = reflection(path, stem, key, stage)
            require(not obj.get("entries_lost"), "compiler dropped declared entry variants")
            require(not (isinstance(obj.get("tess"), dict) and obj["tess"].get("unsupported")),
                    "unsupported tessellation reflection")
            require(obj.get("chain_metal") == mac_record, "reflection does not name the pinned macOS tool options")
            if stage == "gs" and isinstance(obj.get("gs_slice"), dict):
                gs = obj["gs_slice"]
                require(type(gs.get("loc")) is int and gs.get("comp") in ("x", "y", "z", "w"),
                        "invalid geometry slice reflection")
                continue
            check_entry(obj)
            require(permit_dynamic or not any(
                str(b.get("msl_name")).endswith("_cb0_40000000")
                and re.fullmatch(r"array<.+>", str(b.get("msl_type")))
                for b in obj["bindings"]),
                    "dynamic texture slots disabled in profile")
            base_entries = {v for k, v in obj.items() if isinstance(v, str) and
                            (k == "entry" or k.startswith("entry_"))}
            if obj.get("entry_u_side"):
                base_entries.discard(obj["entry_u"])
            check_lib(stem, base_entries)
            if stage == "vs":
                for field, suffix in (("pull", "p"), ("vin", "v")):
                    if obj.get(field):
                        require(isinstance(obj[field], dict), "invalid vertex variant reflection")
                        entries = {v for k, v in obj[field].items() if isinstance(v, str) and
                                   (k == "entry" or k.startswith("entry_"))}
                        check_lib(stem + suffix, entries or {obj["entry"]})
            if stage == "ps":
                if obj.get("alpha") or obj.get("logic") or obj.get("entry_u_side"):
                    entries = {e for variant in (obj.get("alpha"), obj.get("logic")) if variant
                               for e in variant["entries"]}
                    if obj.get("entry_u_side"):
                        entries.add(obj["entry_u"])
                    check_lib(stem + "x", entries)
                if obj.get("fetch") or key in fetch_required:
                    require(obj.get("fetch"), "requested readable tile variant not built")
                    check_lib(stem + "f", {"main0_f"})
                require(key not in dual_required or obj.get("entry_d"), "requested dual-source entry not built")
            if stage == "gs":
                for suffix in ("g0", "g1", "g2", "g3", "g4", "gv", "l0", "l1", "l3"):
                    variant = stem + suffix
                    if suffix.startswith("l") and not any(variant + ext in names for ext in (".json", ".metallib")):
                        continue
                    meta = reflection(path, variant, key, "vs" if suffix == "gv" else stage)
                    check_entry(meta)
                    require(meta.get("chain_metal") == mac_record, "reflection does not name the pinned macOS tool options")
                    check_lib(variant, {v for k, v in meta.items() if isinstance(v, str) and
                                       (k == "entry" or k.startswith("entry_"))})
        except FileNotFoundError:
            gaps["required reflection or metallib missing"] += 1
        except (KeyError, TypeError, AttributeError):
            gaps["malformed reflection"] += 1
        except (ValueError, struct.error) as e:
            if isinstance(e, (UnicodeDecodeError, struct.error)):
                gaps["malformed metallib names or section table"] += 1
                continue
            gaps[str(e) if not isinstance(e, json.JSONDecodeError) else "malformed reflection JSON"] += 1
    require(not gaps, "; ".join(f"{reason}: {n}" for reason, n in sorted(gaps.items())))
    return len(libraries)


def read_kv(text):
    out = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        key, value = line.split("\t", 1)
        require(key not in out, "duplicate stamp field")
        out[key] = value
    return out


def file_hash(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def required_keys(path, profile, package, keys):
    wanted = {line.split()[0] for line in path.read_text().splitlines()
              if line.strip() and not line.lstrip().startswith("#")}
    require(wanted, "empty observed key list")
    require(all(re.fullmatch(r"[0-9a-f]{16}", k) for k in wanted), "invalid observed key")
    require(wanted <= keys.keys(), f"observed keys outside local map: {len(wanted - keys.keys())}")
    selected = set(wanted)
    for table in ("shader_pairs.tsv", "pso_pairs.tsv"):
        p = profile.table(table)
        if table == "pso_pairs.tsv" and not p.is_file():
            p = package.parent / table
        if table == "pso_pairs.tsv" and not p.is_file():
            continue
        for line in p.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                v, ps = line.split()
                if v in wanted and ps in keys:
                    selected.add(ps)
    return {k: keys[k] for k in selected}


def preflight(args):
    base = args.assets
    manifest = base / "coverage.tsv"
    manifest.unlink(missing_ok=True)
    commit = subprocess.check_output(["git", "-C", str(args.nxnative), "rev-parse", "HEAD"], text=True).strip()
    require(commit == PIN, "nxnative checkout is not the supported revision")
    sys.path[:0] = [str(args.nxnative / p) for p in ("dm/tools", "dm/games", "dm/gfx/tools")]
    import dmsh_chain
    import game
    import romfs

    require(not (base / f"b-{CHAIN}").exists(), "loose library overlay would bypass the sealed image")
    profile = game.load(str(args.profile))
    inputs = dmsh_chain.inputs(profile)
    map_file = base / "shader_map.tsv"
    require(map_file.read_bytes() == profile.table("shader_map.tsv").read_bytes(), "runtime map differs from build input")
    keys, records = shader_map(map_file.read_text())
    skip = profile.get("shader_key_skip", "0+0").split("+")
    require(len(skip) == 2, "malformed shader_key_skip")
    skip = tuple(int(x, 0) for x in skip)
    require(all(x >= 0 for x in skip), "malformed shader_key_skip")
    check_package(args.package, records, skip)
    selected = required_keys(args.required_keys, profile, args.package, keys) if args.required_keys else keys
    with args.main.open("rb") as f:
        header = f.read(0x60)
    require(len(header) == 0x60 and header[:4] == b"NSO0", "main is not an NSO")
    build = header[0x40:0x50].hex()
    require(any(b.get("main") == build and b.get("display") == args.version for b in profile.builds().values()),
            "main build ID is not registered for the requested display version")

    sealed = ["shader_map.tsv", IMAGE]
    image = romfs.Path(base / IMAGE)
    stamp_file = image / "inputs.tsv"
    expected_stamp = {"game": profile.id, "inputs": inputs}
    if args.required_keys:
        require(not stamp_file.is_file() or read_kv(stamp_file.read_text()) == expected_stamp,
                "shader image stamp differs from current profile inputs")
    else:
        require(read_kv(stamp_file.read_text()) == expected_stamp, "shader image stamp differs from current profile inputs")
        marker = f"# chain b-{CHAIN} inputs {inputs} "
        for name in SIDE_TABLES:
            with (base / name).open() as f:
                require(f.readline().startswith(marker), "runtime side table stamp differs from image")
        sealed.extend(SIDE_TABLES)

    def wanted(name):
        p = profile.table(name)
        return {line.split()[0] for line in p.read_text().splitlines()
                if line.strip() and not line.lstrip().startswith("#")} if p.is_file() else set()

    mac_record = " ".join(dmsh_chain.MAC_METAL) + "; xcrun -sdk macosx metallib"
    count = check_libraries(image, selected, json.loads((image / "_b_build.json").read_bytes()),
                            profile.get("tex_dyn_slots", "off") == "on",
                            wanted("bf_want.tsv"), wanted("dual_want.tsv"), mac_record)
    rows = {"format": "1", "chain": CHAIN, "game": profile.id, "program": f"{int(profile.get('program_id'), 16):016x}",
            "build": build, "inputs": inputs, "mode": "observed" if args.required_keys else "mapped",
            "keys": str(len(selected)), "libraries": str(count)}
    rows.update({name: file_hash(base / name) for name in sealed})
    temp = manifest.with_suffix(".tmp")
    temp.write_text("".join(f"{k}\t{v}\n" for k, v in rows.items())
                    + "".join(f"key\t{k}\n" for k in sorted(selected)))
    temp.replace(manifest)
    print(f"Verified {len(selected)}/{len(keys)} mapped keys and {count} library files ({rows['mode']} scope).")


def main():
    parser = argparse.ArgumentParser(description="Validate a local nxnative Route B shader image and write coverage.tsv.")
    for name in ("nxnative", "profile", "package", "main", "assets"):
        parser.add_argument(f"--{name}", required=True, type=lambda s: Path(s).resolve())
    parser.add_argument("--version", required=True)
    parser.add_argument("--required-keys", type=Path,
                        help="limit the gate to a local observed key list and available pixel dependencies")
    args = parser.parse_args()
    try:
        preflight(args)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as e:
        parser.exit(1, f"Metal shader preflight: {e}\n")


if __name__ == "__main__":
    main()
