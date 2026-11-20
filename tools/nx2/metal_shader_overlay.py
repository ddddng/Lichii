#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
from pathlib import Path


def overlay(source):
    def replace(old, new, count=1):
        nonlocal source
        if source.count(old) != count:
            raise ValueError(f"unexpected nxnative Metal shader hook: {old.splitlines()[0]}")
        source = source.replace(old, new)

    replace("static _Atomic(romfs_img *) g_msl_img;",
            'static _Atomic(romfs_img *) g_msl_img;\n#include "nvn_metal_shaders.inc"')
    replace("NSData *d = [NSData dataWithContentsOfFile:path];\n    if (d) return d;",
            "NSData *d = lichii_shader_path(path) ? nil : [NSData dataWithContentsOfFile:path];\n    if (d) return d;")
    replace("NSString *path = e ? @(e) : exe_rel([NSString stringWithFormat:",
            'NSString *path = lichii_shader_root ? [lichii_shader_root stringByAppendingPathComponent:@"shader_map.tsv"]\n'
            "        : e ? @(e) : exe_rel([NSString stringWithFormat:")
    replace("NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];\n"
            '    if (!txt) MDIE("cannot read the shader map',
            "NSString *txt = lichii_shader_map ?: [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];\n"
            '    if (!txt) MDIE("cannot read the shader map')
    replace("if (c.count > 3) g_shmap[c[3]] = c[2];",
            "if (c.count > 3 && (!lichii_shader_keys || [lichii_shader_keys containsObject:c[3]])) g_shmap[c[3]] = c[2];")
    replace('g_msl_dir = [NSString stringWithFormat:@"%@/b-%s",',
            'g_msl_dir = lichii_shader_root ? [lichii_shader_root stringByAppendingPathComponent:@"b-" @DMSH_CHAIN]\n'
            '        : [NSString stringWithFormat:@"%@/b-%s",')
    replace("NSString *ct = cp ? [NSString stringWithContentsOfFile:cp",
            "NSString *ct = lichii_shader_root ? lichii_c1_full : cp ? [NSString stringWithContentsOfFile:cp")
    replace("NSString *xt = [NSString stringWithContentsOfFile:xp",
            "NSString *xt = lichii_shader_root ? lichii_cb_extent : [NSString stringWithContentsOfFile:xp")
    replace("static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {",
            "static id<MTLLibrary> lib_from_file_impl(NSString *path, NSError **err) {")
    replace("    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;",
            "    int fd = lichii_shader_path(path) ? -1 : open(path.fileSystemRepresentation, O_RDONLY); struct stat st;")
    replace("// NVN depth mode (nvnDeviceSetDepthMode):",
            """static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    id<MTLLibrary> lib = lib_from_file_impl(path, err);
    romfs_img *img;
    if (!lib && lichii_shader_path(path) && msl_image_file(path, &img))
        lichii_shader_failure("Metal rejected a library in the sealed image");
    return lib;
}
// NVN depth mode (nvnDeviceSetDepthMode):""")
    replace("return !access(path.fileSystemRepresentation, F_OK) || msl_image_file(path, &m) != NULL;",
            "return (!lichii_shader_path(path) && !access(path.fileSystemRepresentation, F_OK)) || msl_image_file(path, &m) != NULL;")
    replace("    if (o->rf.nb) {\n        o->ae",
            """    if (lichii_shader_keys && (!o->fn || (j[@"entry_geometry_capture"] && !o->geo_fn) ||
        (j[@"entry_tf"] && !o->tf_fn) || (j[@"entry_discard"] && !o->discard_fn) ||
        (j[@"entry_d"] && !o->fn_d) || (o->mesh_notask && !o->mesh_obj) ||
        (j[@"entry_u"] && !o->u_name))) {
        lichii_shader_failure("a library lacks a declared shader entry");
        return nil;
    }
    if (o->rf.nb) {
        o->ae""")
    replace('if (o->arg_len != (NSUInteger)(8 * (max_aid + 1)) || (o->arg_len & 7) || max_aid >= NVN_REFL_MAXARG)\n'
            '            MDIE("shader %s has an unexpected argument buffer layout: encodedLength=%lu maxAid=%d", o->key, (unsigned long)o->arg_len, max_aid);',
            '''if (o->arg_len != (NSUInteger)(8 * (max_aid + 1)) || (o->arg_len & 7) || max_aid >= NVN_REFL_MAXARG) {
            if (lichii_shader_keys) { lichii_shader_failure("unexpected shader argument buffer layout"); return nil; }
            MDIE("shader %s has an unexpected argument buffer layout: encodedLength=%lu maxAid=%d", o->key, (unsigned long)o->arg_len, max_aid);
        }''')
    replace('if (!prog_key(p, i, md)) MDIE("program stage %u has no shader key", i);\n'
            '        NSString *k =',
            '''if (!prog_key(p, i, md)) {
            if (lichii_shader_keys) {
                lichii_shader_failure("a guest program stage has no shader key");
                break;
            }
            MDIE("program stage %u has no shader key", i);
        }
        NSString *k =''', count=2)
    replace("        if (!stage) {   // Not in the package:",
            """        if (lichii_shader_keys && !stage) {
            lichii_shader_uncovered(k);
            return nil;
        }
        if (!stage) {   // Not in the package:""")
    replace("        if (sh) o->sh[st] = sh; else MLOG(",
            """        if (!sh && lichii_shader_keys) {
            lichii_shader_failure("validated guest shader failed to load");
            return nil;
        }
        if (sh) o->sh[st] = sh; else MLOG(""")
    replace("    prog_fold_gs(o);   // a slice-writing geometry shader:",
            """    if (lichii_nvn_metal_shader_failed()) return nil;
    prog_fold_gs(o);   // a slice-writing geometry shader:""")
    replace('if (!prog_key(pg, i, md)) MDIE("program stage %u has no shader key", i);',
            '''if (!prog_key(pg, i, md)) {
                if (lichii_shader_keys) continue;
                MDIE("program stage %u has no shader key", i);
            }''')
    replace("    if (!g_dev || !prefetch_on()) return;",
            "    if (!g_dev || (!lichii_shader_keys && !prefetch_on())) return;")
    replace("    shmap_read_once();   // the map only: the prewarm starts on the submit thread (shmap_load)\n"
            "    for (unsigned i = 0; i < p->nstage; i++) {",
            """    shmap_read_once();   // the map only: the prewarm starts on the submit thread (shmap_load)
    for (unsigned i = 0; i < p->nstage; i++) {
        if (lichii_shader_keys) {
            const NVNshaderData *sd = &p->sd[i];
            if (!sd->control || !sd->data) continue;
            uint32_t control_len, code_len;
            nvn_ctl_lens(sd->control, &control_len, &code_len);
            if (!code_len || code_len >= (1u << 24) || !mtl_addr_known(sd->data, code_len)) continue;
        }""")
    replace("        if (st < 0) continue;   // not in the package: a built-in kernel (prog_get)",
            """        if (lichii_shader_keys && st < 0) {
            lichii_shader_uncovered(k);
            return;
        }
        if (st < 0 || !prefetch_on()) continue;   // not in the package: a built-in kernel (prog_get)""")
    replace("    image_prepare(pr, 0x1f);\n    draw_gs_check(pr);",
            "    if (!pr || lichii_nvn_metal_shader_failed()) return;\n    image_prepare(pr, 0x1f);\n    draw_gs_check(pr);")
    replace("    NvShader *cs = prog_get(S.cprog)->sh[5];",
            "    NvProg *cs_program = prog_get(S.cprog);\n    if (!cs_program || lichii_nvn_metal_shader_failed()) return;\n"
            "    NvShader *cs = cs_program->sh[5];")
    replace("    image_prepare(prog_get(S.cprog), 0x20);", "    image_prepare(cs_program, 0x20);")
    replace("    for (;;) {\n        op_hdr h; memcpy(&h, p, sizeof h);\n        g_exec_cur = p;",
            """    for (;;) {
        if (lichii_shader_keys && lichii_nvn_metal_shader_failed()) {
            g_exec_cur = NULL;
            return;
        }
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;""")
    return source


def main():
    parser = argparse.ArgumentParser(description="Apply Lichii's shader hooks to a generated copy of pinned nvn_mtl.m.")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        output = overlay(args.source.read_text())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    except (OSError, ValueError) as e:
        parser.exit(1, f"Metal shader overlay: {e}\n")


if __name__ == "__main__":
    main()
