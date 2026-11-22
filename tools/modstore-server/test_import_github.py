import hashlib
import json
from io import BytesIO
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

import import_github as gh


TITLE = "0100123456789000"
SECOND = "010012345678A000"
SWITCH_TWO = "0400123456789000"
SHORT = "A1B2C3D4E5F60123"
PATCH = f"@nsobid-{SHORT.lower()}\r\n".encode() + b"@enabled\n00000000 00000000\n# INERT_PATCH_BODY"
CREDIT_PATH = f"titles/{TITLE}/Example Game.txt"


def inert_zip(files):
    out = BytesIO()
    with zipfile.ZipFile(out, "w") as archive:
        for path, contents in files.items():
            archive.writestr(path, contents)
    return out.getvalue()


class SnapshotFixture:
    def __init__(self, repository, files):
        self.source = gh.Source(repository, "a" * 40, repository.split("/")[0])
        self.files = files
        self.blobs_read = []

    def size(self, path):
        return len(self.files[path])

    def data(self, path, limit=gh.MAX_METADATA_BYTES):
        if len(self.files[path]) > limit:
            raise AssertionError(f"read past limit: {path}")
        self.blobs_read.append(path)
        return self.files[path]

    def text(self, path):
        return self.data(path).decode()


class ParseTests(unittest.TestCase):
    def test_nso_is_exact_and_not_a_main_cheat_prefix(self):
        for length in (16, 32, 40, 64):
            identifier = "1a" * (length // 2)
            with self.subTest(length=length):
                self.assertEqual(
                    gh.pchtxt_build(f"  @NSOBID-{identifier} # note\r\n@STOP\r".encode()),
                    identifier.upper().ljust(64, "0"),
                )
        for data in (
            b"// @nsobid-" + SHORT.encode(), b"@stop\n" + PATCH,
            b"@nsobid-" + b"0" * 32, b"@nsobid-" + b"1" * 38, b"@nsobid-" + b"1" * 65,
            b"@nonsense" + SHORT.encode(), b"\xef\xbb\xbf" + PATCH,
            PATCH + b"\n" + PATCH, b"@nsobid-not-a-build",
        ):
            with self.subTest(data=data):
                self.assertIsNone(gh.pchtxt_build(data))

    def test_kenji_coalesces_versions_without_blending_variants(self):
        root = f"Example Game/{TITLE}"
        snap = SnapshotFixture("Example/kenji", {
            f"{root}/60 FPS/exefs/1.0.0.pchtxt": PATCH,
            f"{root}/60 FPS/exefs/1.0.1.pchtxt": PATCH,
            f"{root}/Higher Resolution.zip": b"not an archive",
            f"{root}/Higher Resolution/exefs/1.0.0.pchtxt": PATCH,
            f"Different Game/{SWITCH_TWO}/60 FPS/exefs/1.0.0.pchtxt": PATCH,
        })
        omissions = gh.Counter()
        rows = list(gh.kenji(snap, omissions))
        self.assertEqual([(r.name, r.kind, r.builds) for r in rows], [
            ("60 FPS", "fps", {SHORT.ljust(64, "0"): "unknown"}),
            ("Higher Resolution", "graphics", {SHORT.ljust(64, "0"): "unknown"}),
        ])
        self.assertEqual(omissions, {"unresolved title or edition": 1, "unreadable ZIPs": 1,
                                     "ZIPs without resolved variants": 1})
        self.assertEqual(len(snap.blobs_read), 4)
        self.assertIsNone(rows[0].payloads[SHORT.ljust(64, "0")])
        self.assertIsNotNone(rows[1].payloads[SHORT.ljust(64, "0")])

    def test_fl4sh_variant_builds_stay_with_their_own_zip_paths(self):
        path = f"Example Game [US][{TITLE.lower()}][mods].zip"
        snap = SnapshotFixture("Example/fl4sh", {
            path: inert_zip({
                "[60FPS v1.0.0]/exefs/1.0.0.pchtxt": PATCH,
                "[60FPS v1.0.1]/exefs/v2.pchtxt": PATCH,
                "[60FPS v1.0.1][Optional]/cheats/" + SHORT + ".txt": b"INERT_CHEAT_BODY",
                "[21.9 Ultrawide v1.0.1]/romfs/example.bin": b"INERT_ROMFS_BODY",
                "[False]/exefs/../different.pchtxt": PATCH,
                "[False]/exefs/nested/different.pchtxt": PATCH,
                "../[False]/exefs/another.pchtxt": PATCH,
                "[False Switch 2]/exefs/valid.pchtxt": PATCH,
            }),
            f"Example Game Switch 2 [{SECOND}][mods].zip": b"not an archive",
            "Wrong application [0100123456789800][mods].zip": b"not an archive",
            "Pokémon Scarlet and Violet [01008F6008C5E000][mods].zip": b"not an archive",
        })
        omissions = gh.Counter()
        rows = list(gh.fl4sh(snap, omissions))
        self.assertEqual([(r.title, r.game, r.name, r.builds) for r in rows], [
            (TITLE, "Example Game", "60 FPS", {SHORT.ljust(64, "0"): "unknown"}),
            (TITLE, "Example Game", "60 FPS Optional", {SHORT: "main"}),
            (TITLE, "Example Game", "21.9 Ultrawide", {}),
        ])
        self.assertEqual(omissions, {"unresolved title or edition": 2,
                                     "update ID or bundled title ambiguity": 1,
                                     "unsafe or duplicate ZIP members": 2})
        self.assertEqual(snap.blobs_read, [path])

    def test_zip_one_pinned_leaf_per_build_and_no_required_sidecars(self):
        path = f"Example Game [{TITLE}][mods].zip"
        member = "[60 FPS v1.0.1]/exefs/1.0.1.pchtxt"
        archive = inert_zip({
            member: PATCH,
            "[60 FPS Required sidecars v1.0.1]/exefs/1.0.1.pchtxt": PATCH,
            "[60 FPS Required sidecars v1.0.1]/cheats/" + SHORT + ".txt": b"[Enable FPS]\n00000000",
            "[60 FPS Required sidecars v1.0.1]/romfs/dummy.bin": b"INERT_ROMFS_BODY",
            "[Resolution cheat]/cheats/" + SHORT + ".txt": b"[ 720p ]\n00000000",
        })
        rows = {r.name: r for r in gh.fl4sh(SnapshotFixture("Example/fl4sh", {path: archive}), gh.Counter())}
        pchtxt = SHORT.ljust(64, "0")
        self.assertEqual(rows["60 FPS"].payloads[pchtxt].record(), {
            "path": path, "member": member, "size_bytes": len(archive),
            "sha256": hashlib.sha256(archive).hexdigest(), "kind": "pchtxt",
        })
        self.assertEqual(rows["60 FPS Required sidecars"].payloads, {pchtxt: None, SHORT: None})
        self.assertEqual(rows["Resolution cheat"].payloads[SHORT].record()["tag"], " 720p ")

    def test_zip_limits_keep_only_a_browse_pack(self):
        path = f"Example Game [{TITLE}][mods].zip"
        snap = SnapshotFixture("Example/fl4sh", {path: b""})
        size = snap.size
        snap.size = lambda _: gh.MAX_ZIP_BYTES + 1
        omissions = gh.Counter()
        rows = list(gh.fl4sh(snap, omissions))
        self.assertEqual([(r.name, r.builds) for r in rows], [("Emulator mod pack", {})])
        self.assertEqual(snap.blobs_read, [])
        self.assertEqual(omissions, {"ZIPs above inspection limit": 1, "ZIPs without resolved variants": 1})
        snap.size = size
        snap.files[path] = inert_zip({str(i): b"" for i in range(gh.MAX_ZIP_MEMBERS + 1)})
        self.assertEqual(gh.zip_variants(snap, path, gh.Counter()), {})

    def test_cheat_game_index_requires_same_title_and_documented_layout(self):
        table = (
            f"[Example Game](titles/{TITLE}/Example%20Game.txt) | [{TITLE}](titles/{TITLE}) |\n"
            f"[Ignored Game](titles/{TITLE}/Example%20Game.txt) | [{SECOND}](titles/{SECOND}) |\n"
            f"[Wrong ID](titles/{TITLE}/Example%20Game.txt) | [{SECOND}](titles/{TITLE}) |\n"
        )
        snap = SnapshotFixture("Example/chansey", {
            "GAMES.md": table.encode(),
            CREDIT_PATH: b"By ExampleAuthor, @Another\nDo not put these notes in the manifest.",
            f"titles/{TITLE}/cheats/{SHORT}.txt": b"[60 FPS]\n00000000\n[Master FPS Code]\n",
            f"titles/{TITLE}/cheats/{SHORT.lower()}.txt": b"[60fps]\n00000000\n[Default 30 FPS]\n",
            f"titles/{TITLE}/cheats/AABBCCDDEEFF1234.txt": b"[Disable Dynamic Resolution]\n00000000",
            f"titles/{TITLE}/cheats/{SHORT * 4}.txt": b"not a runtime cheat filename",
            f"titles/{SECOND}/cheats/{SHORT}.txt": b"orphan",
        })
        omissions = gh.Counter()
        rows = list(gh.chansey(snap, omissions))
        self.assertEqual([(r.title, r.game, r.name, r.author, r.builds) for r in rows], [
            (TITLE, "Example Game", "60 FPS", "ExampleAuthor, Another", {SHORT: "main"}),
            (TITLE, "Example Game", "Disable Dynamic Resolution", "ExampleAuthor, Another",
             {"AABBCCDDEEFF1234": "main"}),
        ])
        self.assertEqual(omissions, {"unusable cheat filename": 1, "unresolved title or edition": 1,
                                    "cheat directories absent from game index": 1})
        self.assertEqual(snap.blobs_read[:2], ["GAMES.md", CREDIT_PATH])
        self.assertEqual(len(snap.blobs_read), 5)
        self.assertIsNone(rows[0].payloads[SHORT])
        self.assertEqual(rows[1].payloads["AABBCCDDEEFF1234"].tag, "Disable Dynamic Resolution")

    def test_cheat_download_requires_a_unique_valid_section_without_master(self):
        for data in (
            b"{Bootstrap}\n00000000\n[60 FPS]\n00000000",
            b"[60 FPS]\n00000000\n[60 FPS]\n00000000",
            b"[60 FPS]\nnot opcodes",
            b"[60 FPS] / note\n00000000\n[60 FPS]\n00000000",
            b"[60 FPS]\n",
        ):
            with self.subTest(data=data):
                self.assertFalse(gh.cheat_sections(data)["60 FPS"])
        self.assertEqual(gh.cheat_sections(b"[60FPS]\r\n00000000 00000000\n[30 FPS]\n00000000"),
                         {"60FPS": True, "30 FPS": True})
        for path in ("../file", "x/./y", "x//y", "space/file?query", "C:/file", "x#fragment", "x%20/y", "x\\y",
                     "x/" + "é" * 256):
            self.assertFalse(gh.safe_path(path))

    def test_stevens_nearest_documented_title_variant_and_credit(self):
        root = f"Example Game/US [{TITLE}]"
        bad = f"Conflicting Game/[{SECOND}]"
        snap = SnapshotFixture("Example/stevens", {
            f"{root}/readme.md": (
                f"**Game Title**: Example: The Game\n**TitleID: {TITLE}**\n"
                "**Credits**: Author1 (60fps Mod), Author2 (Uncap FPS Mod)\n"
                "Do not install these mods using third-party tools."
            ).encode(),
            f"{root}/1.0.0/FPS/60fps/60.pchtxt": PATCH,
            f"{root}/1.0.1/FPS/60FPS/60.pchtxt": PATCH,
            f"{root}/Uncap FPS/uncap.zip": inert_zip({"[Uncap FPS v1.0.0]/exefs/1.0.0.pchtxt": PATCH}),
            f"{root}/60FPS/IPS Files (Only for Switch users)/60.zip": b"not an archive",
            f"{bad}/readme.md": f"**TitleID**: {TITLE}".encode(),
            f"{bad}/60fps/60.pchtxt": PATCH,
            "No Mapping/FPS/60fps.zip": b"not an archive",
        })
        omitted = gh.Counter()
        rows = list(gh.stevens(snap, omitted))
        self.assertEqual([(r.name, r.game, r.author, r.builds) for r in rows], [
            ("60 FPS", "Example: The Game", "Author1", {SHORT.ljust(64, "0"): "unknown"}),
            ("Uncap FPS", "Example: The Game", "Author2", {SHORT.ljust(64, "0"): "unknown"}),
        ])
        self.assertEqual(omitted, {"hardware-only or unverified IPS layout": 1,
                                   "conflicting README title ID": 1, "unresolved title or edition": 1,
                                   "assets restricted by third-party tool terms": 3})
        self.assertTrue(all(not any(row.payloads.values()) for row in rows))


class ManifestTests(unittest.TestCase):
    def test_batch_directory_preserves_whole_games_within_request_limit(self):
        with tempfile.TemporaryDirectory() as scratch:
            games = [{"title_id": key, "name": "é" * 32, "mods": []} for key in ("A", "B", "C")]
            directory = Path(scratch) / "batches"
            gh.write_batches({"games": games}, directory, 120)
            chunks = [path.read_bytes() for path in sorted(directory.glob("*.json"))]
            self.assertEqual(len(chunks), 3)
            self.assertTrue(all(len(chunk) <= 120 for chunk in chunks))
            self.assertEqual([game for chunk in chunks for game in json.loads(chunk)["games"]], games)
            with self.assertRaises(gh.ImportFailure):
                gh.write_batches({"games": games}, Path(scratch) / "too-large", 80)

    def test_reupload_prohibition_does_not_block_original_direct_links(self):
        self.assertIsNone(gh.PROHIBITION.search(
            "You don't have permission to reupload my mods to external sites. JUST USE THE ORIGINAL LINKS"
        ))
        self.assertIsNotNone(gh.PROHIBITION.search("Do not download these patches using other tools."))

    def test_reads_only_the_reviewed_git_tree_and_emits_only_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = "Example/reviewed"
            root = Path(directory)
            clone = root / "reviewed"
            clone.mkdir()
            def git(*args, input=None):
                return subprocess.run(["git", "-C", clone, "-c", "core.hooksPath=/dev/null", *args],
                                      input=input, check=True, capture_output=True, text=True).stdout.strip()

            def write_fixture_commit():
                tree = git("write-tree")
                person = "Anonymous <> 1792408455 +0200"
                commit = git("hash-object", "-t", "commit", "-w", "--stdin",
                             input=f"tree {tree}\nauthor {person}\ncommitter {person}\n\nTest data\n")
                git("update-ref", "HEAD", commit)
                return gh.Source(repo, commit, "Example")

            git("init")
            git("remote", "add", "origin", f"https://github.com/{repo}.git")
            patch = clone / f"Example Game/{TITLE}/60 FPS/exefs/1.0.0.pchtxt"
            patch.parent.mkdir(parents=True)
            patch.write_bytes(PATCH)
            (patch.parents[2] / "Opaque pack.zip").write_bytes(b"INERT_ARCHIVE_BODY")
            symlink = patch.with_name("2.0.0.pchtxt")
            symlink.symlink_to("/does-not-exist")
            git("add", ".")
            old = write_fixture_commit()
            manifest, stats = gh.build_manifest(root, (old,), (gh.kenji,))
            patch.write_bytes(PATCH.replace(SHORT.lower().encode(), b"ba" * 16))
            git("add", ".")
            new = write_fixture_commit()
            patch.write_bytes(b"UNCOMMITTED_PATCH_BODY")
            self.assertEqual((manifest, stats), gh.build_manifest(root, (old,), (gh.kenji,)))
            newer, _ = gh.build_manifest(root, (new,), (gh.kenji,))
            mods = manifest["games"][0]["mods"]
            self.assertEqual([m["id"] for m in mods], [m["id"] for m in newer["games"][0]["mods"]])
            self.assertEqual(len(mods), 2)
            self.assertEqual(stats[repo]["without_builds"], 1)
            first, second = mods
            self.assertRegex(first["id"], r"^gh-[a-z0-9-]{1,59}$")
            self.assertEqual(first, {
                "id": first["id"], "name": "60 FPS", "summary": "60 FPS for Example Game.",
                "description": "Check the source for supported versions, credits, and notes.",
                "type": "fps", "author": "Example", "license": "NOASSERTION",
                "repository": repo, "commit": old.commit, "updated_at": "2026-10-19T11:14:15Z",
                "source_url": f"https://github.com/{repo}/tree/{old.commit}",
                "builds": [{"id": SHORT.ljust(64, "0"), "target": "unknown", "download": {
                    "path": patch.relative_to(clone).as_posix(), "kind": "pchtxt",
                    "size_bytes": len(PATCH), "sha256": hashlib.sha256(PATCH).hexdigest(),
                }}],
            })
            self.assertEqual(second["builds"], [])
            self.assertNotRegex(json.dumps(manifest), r"INERT_|UNCOMMITTED_|/raw/|/archive/")
            with self.assertRaisesRegex(gh.ImportFailure, "full lowercase"):
                gh.Snapshot(root, gh.Source(repo, "HEAD", "Example"))
            with self.assertRaises(gh.ImportFailure):
                gh.Snapshot(root, gh.Source(repo, "b" * 40, "Example"))
            git("remote", "set-url", "origin", "https://example.invalid/wrong")
            with self.assertRaisesRegex(gh.ImportFailure, "expected origin"):
                gh.Snapshot(root, old)


if __name__ == "__main__":
    unittest.main()
