// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "gow3_copy.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// gow3: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies) {
    if (copies.size() < 8 || Gow3Toggle::Disabled(Gow3Toggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    scheduler.EndRendering();
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    if (runtime.IsBufferAccessed(src, src_min, src_size) ||
        runtime.IsBufferAccessed(dst, dst_min, dst_size, true)) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.Record([src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        if (Vulkan::Scheduler::FullBarriers()) { // gow3: GOW3_FULL_BARRIERS=1
            Vulkan::Scheduler::RecordFullBarrier(cmdbuf);
        }
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
        if (Vulkan::Scheduler::FullBarriers()) {
            Vulkan::Scheduler::RecordFullBarrier(cmdbuf);
        }
    });
    runtime.AccessBuffer(src, src_min, src_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(dst, dst_min, dst_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
    return true;
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // gow3: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // GOW3_COPY_MERGE_KB overrides it.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("GOW3_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        VideoCore::written_binding_source = {"hle copy", info.pgm_hash};
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        if (!MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        }
        batch_start = batch_end;
    }

    return true;
}

// gow3: Killzone Shadow Fall's streaming pool defragmenter, a plain copy: thread
// (WorkgroupId.x * 64 + LocalInvocationId.x) loads 16 bytes of buffer 0 and stores them to buffer 1,
// 87-200 MiB per dispatch. On the GPU every copy made both ranges resident in the buffer arena,
// whose blocks are never released: 1.7 GiB of VRAM after a while, then out of device memory. When
// neither range holds GPU-written data the copy is done in guest memory instead, like a DMA copy
// (Rasterizer::CopyBuffer). GOW3_KZ_COPY_HLE=0 keeps the dispatch.
static constexpr u64 KZ_STREAM_COPY_HASH = 0xd1c4ffa0877dbef0;

// memmove split across the copy threads and this one (Gow3Copy::ParallelFor). The GPU command
// thread waits for the whole copy: one memmove of 160 MiB took 15 ms on an i9-14900HX, 36 ms when
// the data moves up (a backward copy), against 7-13 ms on 5 threads. Overlapping moves go in waves
// as wide as the overlap distance, in memmove's direction: no wave reads bytes it writes. Short
// copies and short distances stay one memmove. GOW3_KZ_COPY_PARALLEL=0: always one memmove.
// In a 36-minute session the copies averaged about 3 MiB (104,255 copies, 309 GB, none
// overlapping), so most stay one memmove; the split serves the large ones.
static void ParallelMove(u8* dst, const u8* src, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_KZ_COPY_PARALLEL");
        return !(env && env[0] == '0');
    }();
    constexpr u64 Chunk = 8_MB;
    const u64 distance = dst > src ? u64(dst - src) : u64(src - dst);
    if (!enabled || !Gow3Copy::Enabled() || size < 2 * Chunk || distance < Chunk) {
        std::memmove(dst, src, size);
        return;
    }
    // Chunks of at most Chunk <= distance bytes: each memcpy's own ranges never overlap.
    const auto copy_range = [&](u64 at, u64 length) {
        const u64 parts = (length + Chunk - 1) / Chunk;
        Gow3Copy::ParallelFor(parts, [&](std::size_t i) {
            const u64 offset = at + u64(i) * Chunk;
            std::memcpy(dst + offset, src + offset, std::min(Chunk, at + length - offset));
        });
    };
    if (distance >= size) {
        copy_range(0, size);
    } else if (dst < src) {
        for (u64 at = 0; at < size; at += distance) {
            copy_range(at, std::min(distance, size - at));
        }
    } else {
        for (u64 end = size; end > 0;) {
            const u64 wave = std::min(distance, end);
            end -= wave;
            copy_range(end, wave);
        }
    }
}

static bool ExecuteStreamCopyHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_KZ_COPY_HLE");
        return !(env && env[0] == '0');
    }();
    if (!enabled || info.buffers.size() != 2 || cs_program.dim_y != 1 || cs_program.dim_z != 1 ||
        cs_program.num_thread_x.full != 64 || cs_program.num_thread_y.full != 1 ||
        cs_program.num_thread_z.full != 1) {
        return false;
    }
    const auto src = info.buffers[0].GetSharp(info);
    const auto dst = info.buffers[1].GetSharp(info);
    if (src.GetStride() != 16 || dst.GetStride() != 16 || src.base_address == 0 ||
        dst.base_address == 0) {
        return false;
    }
    const u64 size = std::min({u64(cs_program.dim_x) * 64 * 16, u64(src.GetSize()), u64(dst.GetSize())});
    if (size == 0) {
        return true;
    }
    auto& buffer_cache = rasterizer.GetBufferCache();
    auto& texture_cache = rasterizer.GetTextureCache();
    if (buffer_cache.IsRegionGpuModified(src.base_address, size) ||
        buffer_cache.IsRegionGpuModified(dst.base_address, size) ||
        texture_cache.HasGpuModifiedImage(src.base_address, size) ||
        texture_cache.HasGpuModifiedImage(dst.base_address, size)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 10) {
            std::printf("GPU: stream copy %#llx -> %#llx (%llu KiB) runs on the GPU: GPU-written "
                        "data in range\n",
                        (unsigned long long)src.base_address, (unsigned long long)dst.base_address,
                        (unsigned long long)(size >> 10));
        }
        return false;
    }
    // The caches learn about the write first (its pages are then unprotected: no write faults).
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    rasterizer.InvalidateMemory(dst.base_address, size);
    const auto t1 = Clock::now();
    ParallelMove(reinterpret_cast<u8*>(dst.base_address),
                 reinterpret_cast<const u8*>(src.base_address), size);
    const auto t2 = Clock::now();
    // Every 10 s with copies: how long the GPU command thread spent on them (GPU thread only).
    static u64 copies = 0, bytes = 0, overlapping = 0;
    static double invalidate_ms = 0, copy_ms = 0, worst_ms = 0;
    static auto last = t2;
    const double inv = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double cpy = std::chrono::duration<double, std::milli>(t2 - t1).count();
    ++copies;
    bytes += size;
    overlapping += (dst.base_address > src.base_address ? dst.base_address - src.base_address
                                                         : src.base_address - dst.base_address) < size;
    invalidate_ms += inv;
    copy_ms += cpy;
    worst_ms = std::max(worst_ms, inv + cpy);
    if (t2 - last >= std::chrono::seconds(10)) {
        std::printf("GPU: stream copies in the last %.0f s: %llu (%llu overlapping), %llu MiB; GPU "
                    "command thread %.1f ms (invalidate %.1f, copy %.1f), worst %.1f ms\n",
                    std::chrono::duration<double>(t2 - last).count(), (unsigned long long)copies,
                    (unsigned long long)overlapping, (unsigned long long)(bytes >> 20),
                    invalidate_ms + copy_ms, invalidate_ms, copy_ms, worst_ms);
        std::fflush(stdout);
        copies = bytes = overlapping = 0;
        invalidate_ms = copy_ms = worst_ms = 0;
        last = t2;
    }
    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    case KZ_STREAM_COPY_HASH:
        return ExecuteStreamCopyHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
