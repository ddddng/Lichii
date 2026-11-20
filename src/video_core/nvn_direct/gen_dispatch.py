#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes gen/nvn_dispatch.inc: per NVN function the call of its front-end implementation from a
guest call's registers (AArch64: integer arguments in x0-x7, float ones in v0-v7), with the guest's
addresses turned into host pointers (fastmem arena + address) on the way in and back on the way out.

Signatures: Banana Native's prototypes (bananative/dm/gfx/nvn/nvn_protos.h, MIT) and the NVN_NOPROTO
ones of hle_nvn.c and nvn_window.c, which win (nvn_protos.h has a few wrong). Run after updating bananative/:
    python3 gen_dispatch.py
With --native (or an external --nvn-dir), generate from the pinned nxnative implementation
declarations and slot names. Prototypes, empty stubs and unbridged implementations are not entries.
Arguments past x7 / v7 come from the guest's stack (s[], one 8-byte slot each, in order).
It fails on what the dispatch cannot pass: structs returned through x8,
structs passed by value other than the separate handles, pointers to structs holding addresses
it has no rule for."""
import os
import re
import sys
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
NVN = os.path.join(HERE, 'bananative', 'dm', 'gfx', 'nvn')
OUT = os.path.join(HERE, 'gen', 'nvn_dispatch.inc')

FLOATS = {'float', 'double'}
# Scalars that are guest addresses: buffer / texture addresses (GPU address == CPU address for NVN2
# pools) and command handles (where a recording starts in command memory).
ADDRESSES = {'NVNbufferAddress', 'NVNtextureAddress', 'NVNcommandHandle'}
# By-value structs of one 64-bit word, passed and returned in an x register.
WORD_STRUCTS = {'NVNseparateTextureHandle', 'NVNseparateSamplerHandle'}
# Arrays whose elements hold guest addresses, by element type: the helper that makes a host copy.
ARRAYS = {
    'NVNbufferRange': 'xl_buffer_ranges',
    'NVNcommandHandle': 'xl_handles',
    'NVNshaderData': 'xl_shader_data',
    'shader_data_ext': 'xl_shader_data_ext',
    'NVNmappingRequest': 'xl_mapping_requests',
}
# Pointers to structs that hold no guest address (the struct's bytes are read as they are).
PLAIN_STRUCTS = {'NVNcopyRegion', 'NVNdrawTextureRegion', 'NVNpackagedTextureLayout', 'NVNrectangle',
                 'NVNtextureSparseTileLayout', 'NVNqueueErrorInfo', 'NVNcounterData',
                 'NVNsubroutineLinkageMapPtr', 'NVNseparateTextureHandle', 'NVNseparateSamplerHandle'}
# Calls Lichii's hook answers itself (name lookups).
HOOK_OWN = {'nvnBootstrapLoader', 'nvnDeviceGetProcAddress'}

# Setters that keep a guest function pointer the front end never calls: Lichii's hook runs the
# command buffer's memory callback in the guest (nvn_cmd_memory_ask).
NATIVE_GUEST_CODE = {'nvnCommandBufferSetMemoryCallback'}

NATIVE_BLOCKED = {
    # This setter persists a host-callable function pointer in an in-place NVN object.
    'nvnDeviceInstallDebugCallback',
    # Inferred signature and an embedded GPU address, not a descriptor index.
    'nvnDeviceGetUniformBufferHandle',
    # The available guest and native declarations disagree about the argument count.
    'nvnCommandBufferSetComputeWorkloadPriority',
    'nvnDlssNetworkBuilderSetTextureIDs', 'nvnDlssNetworkBuilderGetOptimalSettings',
    # RT needs compound guest descriptors and addresses that live through submit.
    'nvnCommandBufferDispatchRays', 'nvnCommandBufferDispatchRaysIndirect',
    'nvnProgramGetShaderGroupHandle', 'nvnProgramBuilderSetRayTracingModules',
    'nvnDeviceCalculateAccelerationStructureMemoryRequirements',
    'nvnCommandBufferBuildAccelerationStructures', 'nvnCommandBufferCopyAccelerationStructure',
    'nvnCommandBufferCompactAccelerationStructure', 'nvnCommandBufferBindAccelerationStructure',
    'nvnCommandBufferBindAccelerationStructures',
}

NATIVE_ARRAY_COUNTS = {
    'nvnWindowBuilderSetTextures': 1,
    'nvnCommandBufferSetRenderTargets': 1,
    'nvnMemoryPoolMapVirtual': 1,
    'nvnQueueSubmitCommands': 1,
    'nvnCommandBufferCallCommands': 1,
    'nvnCommandBufferCopyCommands': 1,
    'nvnProgramSetShaders': 1,
    'nvnProgramSetShadersExt': 1,
    'nvnCommandBufferBindUniformBuffers': 3,
    'nvnCommandBufferBindStorageBuffers': 3,
    'nvnCommandBufferBindVertexBuffers': 2,
    'nvnCommandBufferBindTransformFeedbackBuffers': 2,
}

NATIVE_ARRAY_LIMITS = {
    'nvnWindowBuilderSetTextures': 4,
    'nvnCommandBufferSetRenderTargets': 8,
    'nvnProgramSetShaders': 6,
    'nvnProgramSetShadersExt': 6,
}

NATIVE_PRIVATE_POINTERS = {'shader_data_ext', 'dlss_builder_t', 'dlss_net_t', 'dlss_params_t'}

# Native: the bytes behind pointer arguments that are not a struct of nvn.h or nxnative, by
# (function, argument index). Native Metal on 16 KiB host pages checks that they are reachable as a
# whole (nvn_guest_address.h). A void* followed by a size_t is sized by it, except pool storage
# (nvnMemoryPoolInitialize checks it).
NATIVE_POINTER_SIZES = {
    ('nvnCommandBufferUpdateUniformBuffer', 5): 'x[4]',
    ('nvnProgramInitializeFromBuilder', 1): 'sizeof(nvn_native_program_builder)',
}
NATIVE_UNSIZED_STORAGE = {'nvnMemoryPoolBuilderSetStorage'}

NATIVE_SCALARS = {
    'void', 'char', 'float', 'double', 'int', 'int32_t', 'uint32_t', 'int64_t', 'uint64_t',
    'size_t', 'ptrdiff_t', 'NVNboolean', 'NVNdebugDomainId', 'NVNstorageClass',
    'NVNtextureHandle', 'NVNimageHandle', 'NVNnativeWindow',
} | ADDRESSES


def native_protos(nvn):
    src = open(os.path.join(nvn, 'hle_nvn.c')).read()
    for fragment in re.findall(r'^#include "(nvn_\w+\.inc)"', src, re.M):
        src += '\n' + open(os.path.join(nvn, fragment)).read()
    names = open(os.path.join(nvn, 'names.txt')).read().split()
    slots = set(names)
    count = open(os.path.join(nvn, 'nvn_count.h')).read()
    if len(slots) != len(names) or not re.search(rf'^#define NVN_NFUNC {len(names)}$', count, re.M):
        sys.exit('native slot names do not match NVN_NFUNC')
    out = {}
    for m in re.finditer(r'^\s*NVN(?:_NOPROTO)?\((\w+),\s*([^,]+),\s*([^)]*)\)\s*\{', src, re.M):
        name, ret, params = m.groups()
        if name not in slots or name in out:
            sys.exit(f'{name}: native implementation outside the slot table or defined twice')
        args = [a.strip() for a in params.split(',') if a.strip() not in ('', 'void')]
        if any(not re.search(r'\b\w+$', a) for a in args):
            sys.exit(f'{name}: unsupported native parameter declaration')
        out[name] = (ret.strip(), [re.sub(r'\s*\*', '*', re.sub(r'\w+$', '', a)).strip()
                                   for a in args])
    if missing := NATIVE_BLOCKED - out.keys():
        sys.exit(f'changed native ABI exclusions: {", ".join(sorted(missing))}')
    return {name: sig for name, sig in out.items() if name not in NATIVE_BLOCKED}


def protos(nvn, include_lichii_overrides):
    out = {}
    for line in open(os.path.join(nvn, 'nvn_protos.h')):
        m = re.match(r'typedef (.+?) \(\*NVNT_(\w+)\)\((.*)\);', line)
        if m:
            args = [a.strip() for a in m.group(3).split(',')] if m.group(3).strip() else []
            out[m.group(2)] = (m.group(1).strip(), [a for a in args if a != 'void'])
    src = open(os.path.join(nvn, 'hle_nvn.c')).read()
    if include_lichii_overrides:
        src += open(os.path.join(HERE, 'nvn_window.c')).read()
    for m in re.finditer(r'^NVN_NOPROTO\((\w+),\s*([^,]+),\s*([^)]*)\)', src, re.M):
        args = []
        for a in m.group(3).split(','):
            a = a.strip()
            if not a or a == 'void':
                continue
            if a == '...':
                sys.exit(f'{m.group(1)}: variadic')
            args.append(re.sub(r'\s*\*', '*', re.sub(r'\w+$', '', a)).strip())
        out[m.group(1)] = (m.group(2).strip(), args)
    return out


def struct_types(nvn):
    """{name: True for an opaque object (only reserved bytes), False for a data struct}"""
    text = open(os.path.join(nvn, 'include', 'nvn.h')).read()
    out = {}
    for body, name in re.findall(r'typedef struct\s*\w*\s*\{([^}]*)\}\s*(\w+);', text, re.S):
        fields = [f.strip() for f in body.split(';') if f.strip()]
        out[name] = all(re.search(r'\b(reserved|dummy)\[', f) for f in fields)
    return out


def base(t):
    return re.sub(r'\bconst\b', '', t).replace('*', '').strip()


def is_pointer(t):
    return '*' in t or 'PFN' in t or base(t) == 'NVNnativeWindow'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nvn-dir', default=NVN)
    parser.add_argument('--output', default=OUT)
    parser.add_argument('--only', nargs='+')
    parser.add_argument('--native', action='store_true')
    options = parser.parse_args()
    native = options.native or os.path.realpath(options.nvn_dir) != NVN
    structs = struct_types(options.nvn_dir)
    if native:
        if os.path.realpath(options.output) == OUT:
            sys.exit('native output must not overwrite the legacy dispatcher; specify --output')
        signatures = native_protos(options.nvn_dir)
        header = open(os.path.join(options.nvn_dir, 'include', 'nvn.h')).read()
        scalars = NATIVE_SCALARS | set(re.findall(r'typedef enum\s*\w*\s*\{[^}]*\}\s*(\w+)\s*;', header))
    else:
        signatures = protos(options.nvn_dir, True)
    if options.only:
        selected = set(options.only)
        if missing := selected - (signatures.keys() - HOOK_OWN):
            label = 'not in the native NVN allowlist' if native else 'missing NVN prototypes'
            sys.exit(f'{label}: {", ".join(sorted(missing))}')
        signatures = {name: signatures[name] for name in selected}
    source = "nxnative's NVN implementations" if native else "Banana Native's NVN prototypes"
    lines = [f'// Generated by gen_dispatch.py from {source} (MIT). Do not edit.',
             '// D(name, flags, body): flags NVN_DISPATCH_*; body calls fn with x[] / v[] / the stack s[] and returns x0 (v0 through *v0).']
    if native:
        lines.append('#include "nvn_native_dispatch_abi.h"')
    start = len(lines)
    for name, (ret, args) in sorted(signatures.items()):
        if name in HOOK_OWN:
            continue
        if native:
            for t in (ret, *args):
                b = base(t)
                if (('PFN' in t and name not in NATIVE_GUEST_CODE)
                        or ('PFN' not in t and b not in scalars | structs.keys())) and not (
                    '*' in t and b in NATIVE_PRIVATE_POINTERS
                ):
                    sys.exit(f'{name}: unaudited native type {t}')
        regs = []
        ints = floats = stack = 0
        for a in args:
            if a in FLOATS:
                regs.append(f'v[{floats}]' if floats < 8 else f's[{stack}]')
                stack += floats >= 8
                floats += 1
            else:
                regs.append(f'x[{ints}]' if ints < 8 else f's[{stack}]')
                stack += ints >= 8
                ints += 1
        ints = floats = stack = 0
        exprs = []
        last_int = None
        scratch = 0
        flags = []
        for n, a in enumerate(args):
            b = base(a)
            stars = a.count('*')
            if a in FLOATS:
                if floats < 8:
                    exprs.append(f'F_{a}(v[{floats}])')
                else:
                    exprs.append(f'F_{a}(s[{stack}])')
                    stack += 1
                floats += 1
                continue
            if ints < 8:
                i = f'x[{ints}]'
            else:
                i = f's[{stack}]'
                stack += 1
            ints += 1
            if 'PFN' in a:
                expr = f'({a})(uintptr_t){i}'  # guest code: kept, never called
            elif stars == 0 and b in WORD_STRUCTS:
                expr = f'({a}){{{i}}}'
            elif stars == 0 and b in ADDRESSES:
                expr = f'({a})host_addr({i})'
            elif stars == 0 and b in structs:
                sys.exit(f'{name}: struct {b} by value')
            elif stars == 0 and b != 'NVNnativeWindow':
                expr = f'({a}){i}'
                if b in ('int', 'int32_t', 'uint32_t', 'size_t', 'uint64_t', 'int64_t', 'ptrdiff_t'):
                    last_int = i
            elif native and name == 'nvnProgramBuilderSetShaders' and b == 'NVNshaderData':
                expr = f'({a})host_ptr({i})'
            elif b in ARRAYS and stars == 1 or (stars == 2 and b != 'char'):
                if native and last_int != f'x[{NATIVE_ARRAY_COUNTS.get(name)}]':
                    sys.exit(f'{name}: unaudited native array/count pair')
                if last_int is None:
                    sys.exit(f'{name}: array argument {i} without a count before it')
                helper = ARRAYS.get(b, 'xl_pointers') if stars == 1 else 'xl_pointers'
                expr = f'({a}){helper}({i}, (int){last_int}, {scratch})'
                if native and helper == 'xl_pointers' and b in structs:
                    expr = f'({a})xl_pointers_n({i}, (int){last_int}, {scratch}, sizeof({b}))'
                scratch += 1
            else:
                if b in structs and not structs[b] and b not in PLAIN_STRUCTS:
                    sys.exit(f'{name}: pointer to {b}, a struct with no rule for the addresses it may hold')
                expr = f'({a})host_ptr({i})'
                if native:
                    size = NATIVE_POINTER_SIZES.get((name, n))
                    if size is None and stars == 1 and (b in structs or b in NATIVE_PRIVATE_POINTERS):
                        size = f'sizeof({b})'
                    if (size is None and b == 'void' and n + 1 < len(args) and args[n + 1] == 'size_t'
                            and name not in NATIVE_UNSIZED_STORAGE):
                        size = regs[n + 1]
                    if size is None and b == 'void' and name.startswith('nvnProgramBuilder') and n == 0:
                        size = 'sizeof(nvn_native_program_builder)'
                    if size is not None:
                        expr = f'({a})host_ptr_n({i}, {size})'
            exprs.append(expr)
        if scratch > 2:
            sys.exit(f'{name}: more than two translated arrays')
        rb = base(ret)
        if ret not in FLOATS and '*' not in ret and rb in structs and rb not in WORD_STRUCTS:
            sys.exit(f'{name}: returns struct {rb} (x8)')
        sig = f'{ret} (*)({", ".join(args) if args else "void"})'
        # Native: the arguments are translated first, and the call does not run when one failed.
        prologue = ''
        if native and args:
            prologue = ' '.join(f'{a} a{n}_ = {e};' for n, (a, e) in enumerate(zip(args, exprs)))
            prologue += ' if (XL_FAILED()) return 0; '
            exprs = [f'a{n}_' for n in range(len(args))]
        call = f'(({sig})fn)({", ".join(exprs)})'
        if native and name == 'nvnProgramInitializeFromBuilder':
            call = f'nvn_native_program_initialize(({sig})fn, {", ".join(exprs)})'
        if ret == 'void':
            body = f'{call}; return 0;'
        elif ret in FLOATS:
            flags.append('NVN_DISPATCH_FLOAT_RETURN')
            body = f'{ret} r_ = {call}; *v0 = R_{ret}(r_); return 0;'
        elif rb in WORD_STRUCTS:
            body = f'{ret} r_ = {call}; uint64_t o_; memcpy(&o_, &r_, 8); return o_;'
        elif is_pointer(ret):
            body = f'return guest_addr((uint64_t)(uintptr_t){call});'
        elif rb in ADDRESSES:
            body = f'return guest_addr((uint64_t){call});'
        else:
            body = f'return (uint64_t){call};'
        body = prologue + body
        if floats:
            flags.append('NVN_DISPATCH_FLOAT_ARGS')
        if native and stack:
            flags.append(f'({stack} << NVN_DISPATCH_STACK_SHIFT)')
        if native and name in NATIVE_ARRAY_LIMITS:
            limit = NATIVE_ARRAY_LIMITS[name]
            guard = f'(unsigned)(int)x[1] > {limit}'
            if name != 'nvnCommandBufferSetRenderTargets':
                guard += ' || ((int)x[1] && !x[2])'
            body = f'if ({guard}) return 0; {body}'
        lines.append(f'D({name}, {" | ".join(flags) if flags else "0"}, {{ {body} }})')
    os.makedirs(os.path.dirname(os.path.abspath(options.output)), exist_ok=True)
    open(options.output, 'w').write('\n'.join(lines) + '\n')
    print(f'{len(lines) - start} functions -> {options.output}')


main()
