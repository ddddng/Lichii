#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
TOOL = HERE / "metal_shader_preflight.py"
spec = importlib.util.spec_from_file_location("metal_shader_preflight", TOOL)
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
PINNED = os.environ.get("LICHII_NXNATIVE_SOURCE_DIR")


def control(length, code, nvn2):
    data = bytearray(length)
    struct.pack_into("<II", data, 0, 0x98761234, 0xC9 if nvn2 else 1)
    struct.pack_into("<I", data, 0x30 if nvn2 else 0x20, length)
    struct.pack_into("<I", data, 0x44 if nvn2 else 0x704, len(code))
    return data


def metallib(*names):
    def record(name):
        name = name.encode() + b"\0"
        body = b"NAME" + struct.pack("<H", len(name)) + name + b"ENDT"
        return struct.pack("<I", len(body) + 4) + body

    entries = struct.pack("<I", len(names)) + b"".join(record(n) for n in names)
    head = bytearray(0x58)
    head[:4] = b"MTLB"
    end = len(head) + len(entries)
    struct.pack_into("<9Q", head, 0x10, end + 1, len(head), len(entries), end,
                     0, end, 0, end, 1)
    return bytes(head) + entries + b"x"


class ShaderPreflight(unittest.TestCase):
    def test_set_shaders_key(self):
        code = b"offline synthetic program"
        nvn2 = control(0xd8, code, True)
        nvn2[0x20:0x28] = b"identity"
        want = hashlib.sha256(nvn2[:0x30] + bytes(4) + nvn2[0x34:0x48] + code).hexdigest()[:16]
        self.assertEqual(check.set_shaders_key(nvn2, code), want)
        nvn2[0x30:0x34] = bytes(4)
        nvn2[0x48] = 1
        self.assertEqual(check.set_shaders_key(nvn2, code), want)
        self.assertNotEqual(check.set_shaders_key(nvn2, bytes(len(code))), want)
        with self.assertRaisesRegex(ValueError, "code length"):
            check.set_shaders_key(nvn2, code[:-1])

        nvn = control(0x850, code, False)
        nvn[0x778:0x77c] = b"skip"
        want = hashlib.sha256(nvn[:0x778] + bytes(4) + nvn[0x77c:] + code).hexdigest()[:16]
        self.assertEqual(check.set_shaders_key(nvn + b"not hashed", code, (0x778, 4)), want)
        self.assertNotEqual(check.set_shaders_key(nvn, code), want)

    def test_conflicting_map_stage(self):
        with self.assertRaisesRegex(ValueError, "conflicting stages"):
            check.shader_map("test.bin\t0\tvertex\t0123456789abcdef\tx\n"
                             "test.bin\t1\tpixel\t0123456789abcdef\ty\n")

    @unittest.skipUnless(PINNED, "set LICHII_NXNATIVE_SOURCE_DIR to test the pinned packed image")
    def test_sealed_observed_and_mapped(self):
        source = Path(PINNED).resolve()
        sys.path[:0] = [str(source / p) for p in ("dm/tools", "dm/games", "dm/gfx/tools")]
        import dmsh_chain
        import game
        import shader_lib

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile = root / "0000000000000001"
            profile.mkdir()
            package = root / "pkg"
            package.mkdir()
            main = root / "main"
            header = bytearray(0x60)
            header[:4], header[0x40:0x50] = b"NSO0", bytes(range(16))
            main.write_bytes(header)
            build = header[0x40:0x50].hex()
            (profile / "game.tsv").write_text(f"title_id\t{profile.name}\nprogram_id\t{profile.name}\n"
                                              f"build\t1\t0.0.1\tmain={build}\n")
            cmd = [sys.executable, "-I", str(TOOL), "--nxnative", str(source), "--profile", str(profile),
                   "--main", str(main), "--assets", str(root), "--package", str(package), "--version", "0.0.1"]
            assets, rows, blob = {}, [], bytearray()
            keys = []
            for index, (stage, short) in enumerate((("vertex", "vs"), ("pixel", "ps"), ("compute", "cs"))):
                code = f"synthetic {stage}".encode()
                ctl = control(0xd8, code, True)
                key = check.set_shaders_key(ctl, code)
                keys.append(key)
                rec = bytearray(0x38)
                rec[:16] = b"BTCDBTCD" + bytes.fromhex(key)
                struct.pack_into("<II", rec, 0x28, len(ctl), len(code))
                blob += rec + code + ctl
                rows.append(f"local.bin\t{index}\t{stage}\t{key}\tsynthetic\n")
                obj = {"key": f"{key}_b", "route": "B", "chain": check.CHAIN, "dm_ctl": key,
                       "stage": short, "bindings": [], "entry": "main0",
                       "chain_metal": " ".join(dmsh_chain.MAC_METAL) + "; xcrun -sdk macosx metallib"}
                if short == "vs":
                    obj["pull"] = {"entry": "main0_p"}
                    assets[f"{key}_bp.metallib"] = metallib("main0_p")
                assets[f"{key}_b.json"] = json.dumps(obj).encode()
                assets[f"{key}_b.metallib"] = metallib("main0")
            (package / "local.bin").write_bytes(blob)
            for target in (root, profile):
                (target / "shader_map.tsv").write_text("".join(rows))
            (profile / "shader_pairs.tsv").write_text(f"{keys[0]}\t{keys[1]}\n")
            seen = root / "seen.tsv"
            seen.write_text(f"{keys[0]}\n")
            inputs = dmsh_chain.inputs(game.load(str(profile)))
            assets["_b_build.json"] = json.dumps([{"key": k, "st": "ok"} for k in keys]).encode()
            image = root / check.IMAGE
            shader_lib.write(assets, str(image))
            observed = [*cmd, "--required-keys", str(seen)]
            self.assertIn("2/3 mapped keys and 3 library files", subprocess.check_output(observed, text=True))
            seal = root / "coverage.tsv"
            manifest = seal.read_text()
            self.assertIn("mode\tobserved", manifest)
            self.assertNotIn(f"key\t{keys[2]}", manifest)

            assets[f"{keys[0]}_bp.metallib"] = metallib("wrong_entry")
            shader_lib.write(assets, str(image))
            bad = subprocess.run(observed, capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1)
            self.assertIn("metallib is missing declared function names", bad.stderr)
            self.assertFalse(seal.exists())

            assets[f"{keys[0]}_bp.metallib"] = metallib("main0_p")
            assets["inputs.tsv"] = f"game\t{profile.name}\ninputs\t{inputs}\n".encode()
            shader_lib.write(assets, str(image))
            for table in check.SIDE_TABLES:
                (root / table).write_text(f"# chain b-{check.CHAIN} inputs {inputs} synthetic\n")
            self.assertIn("3/3 mapped keys and 4 library files", subprocess.check_output(cmd, text=True))
            self.assertIn("mode\tmapped", seal.read_text())
            self.assertEqual(check.read_kv(seal.read_text().split("key\t", 1)[0])[check.IMAGE],
                             hashlib.sha256(image.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
