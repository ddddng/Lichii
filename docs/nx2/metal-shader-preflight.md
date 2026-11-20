# Local Metal shader preflight

The native Metal bridge uses nxnative at `7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f`, shader chain `e641898384f7`. The shader loader needs an owner-local profile and the original translated shaders for the selected update. It does not download libraries or translate guest shaders at runtime.

Keep the extracted game, local profile, split package, output directory, and build logs off the repository and off shared storage. The profile's generated `game_profile.h` is a local build input too. The profile changes NVN behavior, including the API, texture uploads, and dynamic texture handles, so the bridge test's synthetic header is not valid for a game build.

## Build and check on the Mac

Use the pinned checkout's matching translator and SPIRV-Cross binaries and the Metal toolchain recorded in its lock. In the commands below, `NX`, `PROFILE`, `PKG`, `MAIN`, and `ASSETS` are absolute local paths. `PROFILE` is a private copy of the matching profile and must retain its title-ID directory name. `MAIN` is the selected update's extracted `ExeFS/main`; `DISPLAY_VERSION` must match a `build` row in that profile. Set `PYTHON` to a Python 3.12 or later interpreter with the pinned splitter's dependencies.

If the exact split package and profile tables aren't already on disk, use the pin's splitter on the update's merged RomFS and corresponding main. Only supply base plus update separately when both game versions are real inputs to the desired scope.

```sh
DM_GAME="$PROFILE" "$PYTHON" "$NX/dm/gfx/tools/shader_split.py" \
  "$MERGED_ROMFS" "$MAIN" "$PKG" "$PROFILE/shader_map.tsv" \
  "$PROFILE/shader_facts.tsv" "$PROFILE/shader_pairs.tsv" "$PROFILE/pso_pairs.tsv"
```

For every key in that map, run the full build with no trailing keys. It writes the packed library and stamps the input tables. It can return success even with failed shaders, so check the output with the preflight before booting.

```sh
mkdir -p "$ASSETS"
DM_GAME="$PROFILE" DMSH_TARGETS=mac J=4 DMSH_PROCS=4 \
  "$PYTHON" "$NX/dm/gfx/tools/dmsh_build.py" "$PKG" "$ASSETS"
cp "$PROFILE"/{shader_map,c1_full,cb_extent}.tsv "$ASSETS/"
"$PYTHON" -I tools/nx2/metal_shader_preflight.py --nxnative "$NX" --profile "$PROFILE" \
  --package "$PKG" --main "$MAIN" --version "$DISPLAY_VERSION" --assets "$ASSETS"
```

On an existing exact build, skip the splitter and compiler. Use the build's existing `b-e641898384f7.img`; don't expand it. If the compile fell back to a loose directory, fix the packing error before trying to boot.

For a first boot limited to witnessed keys, use a fresh local output directory and pass those keys as trailing arguments to the build, then pass the same private TSV to the preflight. The preflight fails if any witnessed key is missing from the exact update's local map; it won't silently subtract it. nxnative compiles the paired pixel dependencies as well. Check the actual argument size against `getconf ARG_MAX` before passing a large key list.

```sh
keys=$(awk 'NF && $1 !~ /^#/ {print $1}' "$SEEN")
test -n "$keys" && DM_GAME="$PROFILE" DMSH_TARGETS=mac J=4 DMSH_PROCS=4 \
  "$PYTHON" "$NX/dm/gfx/tools/dmsh_build.py" "$PKG" "$ASSETS" $keys
cp "$PROFILE/shader_map.tsv" "$ASSETS/"
"$PYTHON" -I tools/nx2/metal_shader_preflight.py --nxnative "$NX" --profile "$PROFILE" \
  --package "$PKG" --main "$MAIN" --version "$DISPLAY_VERSION" --assets "$ASSETS" \
  --required-keys "$SEEN"
```

The observed scope does not use `c1_full.tsv` or `cb_extent.tsv`, even if present. A partial translation doesn't stamp those as a full build. Replay will use conservative buffer handling instead. The witness list plus its available pixel dependencies is the entire allowed runtime set; other libraries in the image cannot expand it.

Then launch the local native-capable Lichii build as usual, with a scratch user profile. For a first scoped run, disable pipeline archive and prewarm inputs so they're not another source of local artifacts:

```sh
EDEN_NX2_NVN_METAL=1 EDEN_NX2_NVN_METAL_ASSETS="$ASSETS" \
  DM_PSO_ARCHIVE=off DM_PSO_MANIFEST=off DM_PSO_PREBUILD=0 \
  "$LICHII" -g "$GAME"
```

## What the gate checks

- The offline check ties the main NSO's first 16 build-ID bytes to the requested profile version. It walks the package in map index order, bounds each record, and compares each package key against the map and the runtime `SetShaders` hash. NVN2 requires little-endian magic `0x98761234` and version `0xc9`. It hashes the first `0x48` control bytes with the four bytes at `0x30` replaced by zero, then the entire code area whose length is at `0x44`. Legacy NVN takes the control length at `0x20` in `[0x760, 0x880)` or else `0x880`, with any profile skip range zeroed, and the code length from `0x704`. The key is the first eight SHA-256 bytes in byte order, rendered as 16 lowercase hex digits. The offline check requires a complete code area, never a control-only match.
- It checks the Route B build result, reflection key, stage, chain, compiler options, declared function names in the metallib, and reflected side libraries. The geometry-slice reflection is an intentional exception to the base library requirement; ordinary geometry requires its topology/reader libraries. A failure blocks the selected scope. The tool rejects known unsupported cases such as universal RT and declared entry losses. It doesn't prove uncaptured shader generation, Metal specialization, all geometry combinations, or rendering parity.
- `coverage.tsv` binds the selected keys to the main build, profile, map, image bytes, and full-build side tables when applicable. Before guest ownership the loader compares the actual program ID and main key, checks Metal 4 and unified memory, verifies the receipt's hashes, and pins the image descriptor. It reads the map and side tables from checked snapshots and ignores loose-library overlays and alternative shader directories. Keep the image immutable until the process exits. Startup reads the whole image once to check its SHA-256; the backend loads individual libraries as it needs them.
- Once owned, an unmapped shader key or a validated library that Metal cannot use latches a failure. A host must stop the session and surface that failure, not attempt to resume the same guest on Vulkan. For a private missed-key diagnostic only, set `EDEN_NX2_NVN_METAL_DIAG_KEYS=1` and keep that log local.

An all-map pass says the local map's keys have the checked files. An observed pass says the same about just the witnessed scope and its dependencies. Neither says the title is complete or playable.

## Host integration and synthetic verification

Generate a copy of the pinned `nvn_mtl.m` using `python3 -I tools/nx2/metal_shader_overlay.py INPUT OUTPUT`. Run the same command with `--hle` on `hle_nvn.c` to require a registered code pool when resolving a deferred key. Compile the generated copies instead of the originals with the original NVN and Lichii `renderer_metal` directories in the include path. Invoke `lichii_nvn_metal_prepare_shaders` before handing guest ownership to NVN and monitor `lichii_nvn_metal_shader_failed` once the session is active.

```sh
LICHII_NXNATIVE_SOURCE_DIR="$NX" python3 -I tools/nx2/test_metal_shader_preflight.py -v
```

The test uses synthetic control and code areas and an image generated by the pinned packer. Its metallib records test the file parser; they aren't loadable GPU programs.
