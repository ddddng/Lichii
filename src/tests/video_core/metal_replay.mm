// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <unistd.h>

#include "common/host_memory.h"
#include "video_core/renderer_metal/guest_buffer.h"
#include "video_core/renderer_metal/guest_pool_backing.h"

namespace {

constexpr size_t Extent = 64;
constexpr size_t RowBytes = Extent * 4;
constexpr size_t ViewSize = 16 * 1024;

constexpr char Shader[] = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 vs(uint i [[vertex_id]], const device float2* points [[buffer(0)]]) {
    return float4(points[i], 0.0, 1.0);
}
fragment float4 fs() {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

void Check(bool success, const char* step) {
    if (!success) {
        std::fprintf(stderr, "Metal replay test failed: %s\n", step);
        std::exit(1);
    }
}

std::shared_ptr<void> Map(const std::shared_ptr<Common::HostMemory>& memory, size_t length,
                          std::span<const Common::HostMemory::BackingRun> runs) {
    auto* view = memory->MapBackingView(length, runs);
    Check(view != nullptr, "map backing view");
    return {view, [memory, length](void* address) {
                memory->UnmapBackingView(static_cast<u8*>(address), length);
            }};
}

API_AVAILABLE(macos(26.0))
void Draw(id<MTLDevice> device, id<MTL4CommandQueue> queue, id<MTLBuffer> vertices,
          uint64_t vertex_offset) {
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(Shader) options:nil error:&error];
    Check(library != nil, "compile fixture MSL");
    auto* pipeline_desc = [MTLRenderPipelineDescriptor new];
    pipeline_desc.vertexFunction = [library newFunctionWithName:@"vs"];
    pipeline_desc.fragmentFunction = [library newFunctionWithName:@"fs"];
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    Check(pipeline != nil, "create render pipeline");

    auto* target_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                          width:Extent
                                                                         height:Extent
                                                                      mipmapped:NO];
    target_desc.storageMode = MTLStorageModePrivate;
    target_desc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:target_desc];
    id<MTLBuffer> readback =
        [device newBufferWithLength:Extent * RowBytes options:MTLResourceStorageModeShared];

    auto* args_desc = [MTL4ArgumentTableDescriptor new];
    args_desc.maxBufferBindCount = 1;
    id<MTL4ArgumentTable> args = [device newArgumentTableWithDescriptor:args_desc error:&error];
    id<MTLResidencySet> residency =
        [device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
    id<MTL4CommandBuffer> cmd = [device newCommandBuffer];
    Check(target && readback && args && residency && allocator && cmd, "allocate frame resources");

    const id<MTLAllocation> allocations[] = {vertices, target, readback};
    [residency addAllocations:allocations count:3];
    [residency commit];
    [cmd beginCommandBufferWithAllocator:allocator];
    [cmd useResidencySet:residency];
    [args setAddress:vertices.gpuAddress + vertex_offset atIndex:0];

    auto* pass = [MTL4RenderPassDescriptor new];
    auto* color = pass.colorAttachments[0];
    color.texture = target;
    color.loadAction = MTLLoadActionClear;
    color.clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
    color.storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> render = [cmd renderCommandEncoderWithDescriptor:pass];
    Check(render != nil, "create render encoder");
    [render setRenderPipelineState:pipeline];
    [render setArgumentTable:args atStages:MTLRenderStageVertex];
    [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render endEncoding];

    id<MTL4ComputeCommandEncoder> copy = [cmd computeCommandEncoder];
    Check(copy != nil, "create readback encoder");
    [copy barrierAfterQueueStages:MTLStageFragment
                     beforeStages:MTLStageBlit
                visibilityOptions:MTL4VisibilityOptionDevice];
    [copy copyFromTexture:target
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(Extent, Extent, 1)
                       toBuffer:readback
              destinationOffset:0
         destinationBytesPerRow:RowBytes
       destinationBytesPerImage:0];
    [copy endEncoding];
    [cmd endCommandBuffer];

    auto done = dispatch_semaphore_create(0);
    __block NSError* gpu_error = nil;
    __block double gpu_ms = 0.0;
    auto* options = [MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
      gpu_error = feedback.error;
      gpu_ms = (feedback.GPUEndTime - feedback.GPUStartTime) * 1000.0;
      dispatch_semaphore_signal(done);
    }];
    const id<MTL4CommandBuffer> commands[] = {cmd};
    [queue commit:commands count:1 options:options];
    Check(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0,
          "wait for render/readback feedback");
    Check(gpu_error == nil, "execute render and readback");
    Check(gpu_ms > 0.0, "read native GPU timing");

    const auto* pixels = static_cast<const std::array<u8, 4>*>(readback.contents);
    Check(pixels[0] == std::array<u8, 4>{0, 0, 255, 255}, "read blue clear");
    Check(pixels[(Extent / 2) * Extent + Extent / 2] == std::array<u8, 4>{255, 0, 0, 255},
          "read red triangle drawn from guest-backed vertices");
    std::printf("Native Metal 4 render + readback: %.3f GPU ms\n", gpu_ms);
}

API_AVAILABLE(macos(26.0))
void DrawGuestPool(id<MTLDevice> device, id<MTL4CommandQueue> queue) {
    constexpr uint64_t GuestPage = 4 * 1024;
    auto backing = std::make_shared<Common::HostMemory>(4 * ViewSize, 8 * ViewSize);
    auto* const arena = backing->VirtualBasePointer();
    Check(arena != nullptr, "create pool fastmem arena");
    struct Fixture {
        std::shared_ptr<Common::HostMemory> backing;
        size_t host_offset{ViewSize};
        size_t coverage{ViewSize};
        size_t calls{};
        size_t opened{};
        size_t closed{};
    } fixture{backing};
    const lichii_metal_pool_mapper mapper{&fixture, [](void* context, uint64_t address,
                                                      uint64_t length,
                                                      lichii_metal_pool_lease* out) -> int {
        auto& f = *static_cast<Fixture*>(context);
        ++f.calls;
        Check(address == ViewSize && length == ViewSize, "request the enclosing 16 KiB view");
        f.opened += length / GuestPage;
        const Common::HostMemory::BackingRun run{0, f.host_offset, f.coverage};
        if (!Metal::MapGuestPoolBacking(f.backing, length, std::span{&run, 1}, out)) {
            f.closed += length / GuestPage;
            return 0;
        }
        out->pin = &f;
        out->unpin = [](void* pin, int close) {
            Check(close == 1, "close pins on drain");
            static_cast<Fixture*>(pin)->closed += ViewSize / GuestPage;
        };
        return 1;
    }};
    uint64_t offset = UINT64_MAX;
    lichii_metal_set_pool_mapper(arena, 3 * ViewSize, &mapper);
    auto* const guest_pool = arena + ViewSize + GuestPage;
    Check(!lichii_metal_new_guest_pool_buffer(device, guest_pool, UINT64_MAX, &offset) &&
              offset == UINT64_MAX && fixture.calls == 0,
          "reject a range beyond the arena before mapping");
    fixture.coverage = GuestPage;
    Check(!lichii_metal_new_guest_pool_buffer(device, guest_pool, GuestPage, &offset),
          "reject incomplete host-page backing");
    fixture.coverage = ViewSize;
    fixture.host_offset += GuestPage;
    Check(!lichii_metal_new_guest_pool_buffer(device, guest_pool, GuestPage, &offset),
          "reject incongruent backing on the 16 KiB host");
    fixture.host_offset = ViewSize;
    Check(fixture.opened == fixture.closed, "close pins of refused imports at once");
    const size_t refused = fixture.closed;
    const std::weak_ptr<Common::HostMemory> lifetime = backing;
    @autoreleasepool {
        id<MTLBuffer> vertices =
            lichii_metal_new_guest_pool_buffer(device, guest_pool, GuestPage, &offset);
        Check(vertices && offset == GuestPage && vertices.length == ViewSize,
              "import a 4 KiB guest pool with its Metal buffer offset");
        Check(fixture.opened - refused == ViewSize / GuestPage,
              "pin every 4 KiB guest page of the 16 KiB envelope");
        constexpr std::array<float, 6> points{-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
        auto* const bytes = backing->BackingBasePointer() + ViewSize + GuestPage;
        std::memcpy(bytes, points.data(), sizeof(points));
        Check(std::memcmp(static_cast<u8*>(vertices.contents) + offset, points.data(),
                          sizeof(points)) == 0,
              "Metal sees writes to the original backing");
        lichii_metal_set_pool_mapper(nullptr, 0, nullptr);
        Check(!lichii_metal_new_guest_pool_buffer(device, guest_pool, GuestPage, &offset),
              "no fastmem pointer fallback after mapper teardown");
        backing.reset();
        fixture.backing.reset();
        Check(!lifetime.expired(), "the Metal buffer owns the backing after mapper teardown");
        Draw(device, queue, vertices, offset);
        vertices = nil;
    }
    Check(lifetime.expired(), "release the backing after the finished Metal buffer is released");
    Check(fixture.closed == refused, "keep the pins after Metal releases the buffer");
    lichii_metal_drain_pool_pins(1);
    Check(fixture.closed == fixture.opened, "close every pin on drain");
    lichii_metal_drain_pool_pins(1);
    Check(fixture.closed == fixture.opened, "close each pin once");
}

} // namespace

int main() {
    @autoreleasepool {
        if (@available(macOS 26.0, *)) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device.hasUnifiedMemory || ![device supportsFamily:MTLGPUFamilyMetal4]) {
                return 77;
            }
            id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
            Check(queue != nil && getpagesize() == ViewSize, "create Metal 4 queue on 16 KiB host");
            auto backing = std::make_shared<Common::HostMemory>(4 * ViewSize, 8 * ViewSize);
            const std::array runs{
                Common::HostMemory::BackingRun{.view_offset = 0, .host_offset = ViewSize,
                                               .length = ViewSize},
                Common::HostMemory::BackingRun{.view_offset = 2 * ViewSize, .host_offset = 0,
                                               .length = ViewSize},
            };
            auto fragmented = Map(backing, 3 * ViewSize, runs);
            Check(Metal::ImportGuestBuffer(device, fragmented, 3 * ViewSize) == nil,
                  "reject an import crossing a mapping boundary");
            const auto one_run = std::span{runs}.first(1);
            auto mapping = Map(backing, ViewSize, one_run);
            const std::shared_ptr<void> unowned{std::shared_ptr<void>{}, mapping.get()};
            Check(Metal::ImportGuestBuffer(device, unowned, ViewSize) == nil,
                  "reject a pointer without a lifetime owner");
            DrawGuestPool(device, queue);
            return 0;
        }
        return 77;
    }
}
