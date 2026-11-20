// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nvn_metal_shaders.h"

static NSString *lichii_shader_root, *lichii_shader_map, *lichii_c1_full, *lichii_cb_extent;
static NSSet<NSString *> *lichii_shader_keys;
static _Atomic bool lichii_shader_failed;

bool lichii_nvn_metal_shader_failed(void) {
    return atomic_load_explicit(&lichii_shader_failed, memory_order_acquire);
}

static void lichii_shader_failure(const char *reason) {
    if (lichii_shader_keys && !atomic_exchange_explicit(&lichii_shader_failed, true, memory_order_acq_rel))
        MLOG("[RED] Lichii native Metal shader failure: %s; stop the guest session", reason);
}

static void lichii_shader_uncovered(NSString *key) {
    const char *diagnostic = getenv("EDEN_NX2_NVN_METAL_DIAG_KEYS");
    if (diagnostic && !strcmp(diagnostic, "1") && !lichii_nvn_metal_shader_failed())
        MLOG("[RED] local-only unvalidated NVN shader key: %s", key.UTF8String);
    lichii_shader_failure("guest shader key is outside validated local coverage");
}

static bool lichii_shader_path(NSString *path) {
    return lichii_shader_root && [path hasPrefix:[g_msl_dir stringByAppendingString:@"/"]];
}

static NSString *lichii_digest_hex(const uint8_t *digest) {
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return @(hex);
}

static NSString *lichii_sealed_text(NSString *path, NSString *digest) {
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data || data.length > UINT32_MAX)
        return nil;
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    return [lichii_digest_hex(hash) isEqualToString:digest]
        ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
}

static bool lichii_sealed_image(const romfs_img *image, NSString *digest) {
    if (!image || image->damaged)
        return false;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    uint8_t buf[64 * 1024], hash[CC_SHA256_DIGEST_LENGTH];
    off_t offset = 0;
    for (;;) {
        ssize_t bytes = pread(image->fd, buf, sizeof(buf), offset);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0)
            return false;
        if (!bytes)
            break;
        CC_SHA256_Update(&context, buf, (CC_LONG)bytes);
        offset += bytes;
    }
    CC_SHA256_Final(hash, &context);
    return [lichii_digest_hex(hash) isEqualToString:digest];
}

static bool lichii_shader_hex(NSString *text, NSUInteger length) {
    return text.length == length &&
        [text rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location
            == NSNotFound;
}

static bool lichii_shader_preflight_error(char *error, size_t size, const char *reason) {
    if (error && size)
        snprintf(error, size, "%s", reason);
    return false;
}

bool lichii_nvn_metal_prepare_shaders(const char *assets_dir, uint64_t program_id,
                                     const char *main_build_id, char *error, size_t error_size) {
    @autoreleasepool {
        if (g_shmap || lichii_shader_keys || !assets_dir || assets_dir[0] != '/' || !main_build_id)
            return lichii_shader_preflight_error(error, error_size, "provide an absolute shader asset path before native GPU startup");
        NSString *root = @(assets_dir);
        NSString *build = [@(main_build_id) lowercaseString];
        if (!root || program_id != DM_GAME_PROGRAM_ID || !lichii_shader_hex(build, 32))
            return lichii_shader_preflight_error(error, error_size, "guest build does not match the compiled native profile");

        NSString *cover = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"coverage.tsv"]
                                                   encoding:NSUTF8StringEncoding error:nil];
        if (!cover)
            return lichii_shader_preflight_error(error, error_size, "local shader coverage.tsv is missing");
        NSMutableDictionary<NSString *, NSString *> *fields = [NSMutableDictionary dictionary];
        NSMutableSet<NSString *> *keys = [NSMutableSet set];
        for (NSString *line in [cover componentsSeparatedByString:@"\n"]) {
            if (!line.length)
                continue;
            NSArray<NSString *> *cols = [line componentsSeparatedByString:@"\t"];
            if (cols.count != 2 || !cols[0].length || !cols[1].length)
                return lichii_shader_preflight_error(error, error_size, "invalid local shader coverage record");
            if ([cols[0] isEqualToString:@"key"]) {
                if (!lichii_shader_hex(cols[1], 16) || [keys containsObject:cols[1]])
                    return lichii_shader_preflight_error(error, error_size, "invalid or duplicate key in shader coverage");
                [keys addObject:cols[1]];
            } else {
                if (fields[cols[0]])
                    return lichii_shader_preflight_error(error, error_size, "duplicate field in shader coverage");
                fields[cols[0]] = cols[1];
            }
        }
        NSString *mode = fields[@"mode"];
        bool observed = [mode isEqualToString:@"observed"];
        if (![fields[@"format"] isEqualToString:@"1"] ||
            ![fields[@"chain"] isEqualToString:@DMSH_CHAIN] ||
            ![fields[@"game"] isEqualToString:@(DM_GAME_ID)] ||
            ![fields[@"program"] isEqualToString:[NSString stringWithFormat:@"%016llx", (unsigned long long)program_id]] ||
            ![fields[@"build"] isEqualToString:build] ||
            (!observed && ![mode isEqualToString:@"mapped"]) ||
            !keys.count || ![fields[@"keys"] isEqualToString:[NSString stringWithFormat:@"%lu", (unsigned long)keys.count]])
            return lichii_shader_preflight_error(error, error_size, "coverage scope, build identity or translation chain differs");

        NSString *map = lichii_sealed_text([root stringByAppendingPathComponent:@"shader_map.tsv"], fields[@"shader_map.tsv"]);
        NSString *c1 = nil, *extents = nil;
        if (!observed) {
            c1 = lichii_sealed_text([root stringByAppendingPathComponent:@"c1_full.tsv"], fields[@"c1_full.tsv"]);
            extents = lichii_sealed_text([root stringByAppendingPathComponent:@"cb_extent.tsv"], fields[@"cb_extent.tsv"]);
        }
        if (!map || (!observed && (!c1 || !extents)))
            return lichii_shader_preflight_error(error, error_size, "shader map or side tables changed after preflight");

        NSString *leaf = @"b-" @DMSH_CHAIN @".img";
        romfs_img *image = romfs_image_open([root stringByAppendingPathComponent:leaf].fileSystemRepresentation);
        if (!lichii_sealed_image(image, fields[leaf])) {
            if (image)
                romfs_image_close(image);
            return lichii_shader_preflight_error(error, error_size, "shader image is missing, invalid or changed after preflight");
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device.hasUnifiedMemory || ![device supportsFamily:MTLGPUFamilyMetal4]) {
            romfs_image_close(image);
            return lichii_shader_preflight_error(error, error_size, "native NVN needs unified memory and a Metal 4 GPU");
        }

        lichii_shader_map = map;
        lichii_c1_full = c1;
        lichii_cb_extent = extents;
        lichii_shader_keys = [keys copy];
        lichii_shader_root = root;
        atomic_store_explicit(&g_msl_img, image, memory_order_release);
        MLOG("Lichii native Metal shader coverage: %s scope, %lu keys", mode.UTF8String, (unsigned long)keys.count);
        return true;
    }
}
