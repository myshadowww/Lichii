// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production native-only exposure helpers with actual GPU arithmetic.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include "nr_exposure_smoke_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/vulkan_common/vulkan_device.h"
namespace {
using namespace Vulkan;
struct Parameters {
    std::array<float, 4> scene, changed;
};
constexpr std::array BINDINGS{
    VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
};
constexpr std::array TEMPLATES{
    VkDescriptorUpdateTemplateEntry{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0,
                                    sizeof(DescriptorUpdateEntry)},
    VkDescriptorUpdateTemplateEntry{7, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                    sizeof(DescriptorUpdateEntry), sizeof(DescriptorUpdateEntry)},
};
constexpr std::array PUSH{VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)}};
class Probe final : public ComputePass {
public:
    Probe(const Device& device, Scheduler& scheduler, DescriptorPool& pool)
        : ComputePass(device, scheduler, pool, BINDINGS, TEMPLATES,
                      DescriptorBankInfo{.uniform_buffers = 1, .storage_buffers = 1, .score = 2},
                      PUSH, NR_EXPOSURE_SMOKE_SPV) {}
    void Record(Scheduler& scheduler, const void* descriptors, Parameters params) {
        scheduler.Record([this, descriptors, params](vk::CommandBuffer cmd) {
            const auto set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmd.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, params);
            cmd.Dispatch(4, 1, 1);
        });
    }
};
} // namespace
void NrExposureSmoke(const Vulkan::Device& device) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    DescriptorPool pool{device, scheduler};
    ComputePassDescriptorQueue queue{device,
                                     ComputePassDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
    Probe probe{device, scheduler, pool};
    auto uniform = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           .size = 16,
                                           .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                          MemoryUsage::Upload);
    auto result = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          .size = 64,
                                          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE},
                                         MemoryUsage::Download);
    struct Case {
        std::array<float, 4> exposure;
        Parameters params;
    };
    const std::array cases{
        Case{{.25f, .5f, .125f, 0}, {{0, 0, 0, 1}, {0, 0, 0, 1}}},
        Case{{.25f, .5f, .125f, 0}, {{.0625f, .0625f, .0625f, 1}, {.1875f, .1875f, .1875f, 1}}},
        Case{{.25f, .5f, .125f, 0}, {{1, 1, 1, 1}, {.046875f, .046875f, .046875f, 1}}},
        Case{{.25f, .5f, .125f, 0}, {{.125f, .125f, .125f, 1}, {.09375f, .09375f, .09375f, 1}}},
        Case{{.2992350161f, .1294686794f, .15f, 0},
             {{2.09375f, 1.140625f, .4375f, 1}, {.1f, .04f, .01f, 1}}},
        Case{{.25f, 0, .15f, 0}, {{2, 1, .5f, 1}, {.5f, .25f, .125f, 1}}},
        Case{{1, 0, 0, 0}, {{4, 2, 1, 1}, {.5f, .25f, .125f, 1}}},
    };
    size_t crossings{};
    for (const auto& test : cases) {
        std::memcpy(uniform.Mapped().data(), test.exposure.data(), 16);
        uniform.Flush();
        scheduler.Record([](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                                    VK_ACCESS_HOST_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT});
        });
        queue.Acquire(scheduler, 2);
        queue.AddBuffer(*result, 0, 64);
        queue.AddBuffer(*uniform, 0, 16);
        probe.Record(scheduler, queue.UpdateData(), test.params);
        scheduler.Record([](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_SHADER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        result.Invalidate();
        const std::array<double, 3> weights{.2126, .7152, .0722};
        const auto transform = [&](const auto& rgb, bool inverse) {
            double y{};
            for (size_t c = 0; c < 3; ++c)
                y += rgb[c] * weights[c];
            const double base = test.exposure[0], extra = test.exposure[1], cap = test.exposure[2];
            // Solve the native scalar luminance equation independently in double precision.
            const double next = inverse ? (y <= (base + extra) * cap ? y / (base + extra)
                                                                     : (y - extra * cap) / base)
                                        : base * y + extra * std::min(y, cap);
            std::array<double, 3> out{};
            for (size_t c = 0; c < 3; ++c)
                out[c] = y > 0 ? rgb[c] * next / y : 0;
            return out;
        };
        const auto forward = transform(test.params.scene, false),
                   inverse = transform(test.params.changed, true);
        const std::array expected{forward, transform(forward, true), inverse,
                                  transform(inverse, false)};
        for (size_t i = 0; i < 4; ++i)
            for (size_t c = 0; c < 3; ++c) {
                float actual;
                std::memcpy(&actual, result.Mapped().data() + (i * 4 + c) * 4, 4);
                if (!std::isfinite(actual) || std::abs(actual - expected[i][c]) >
                                                  2e-6 * std::max(1., std::abs(expected[i][c])))
                    throw std::runtime_error("NR native exposure GPU forward/inverse mismatch");
            }
        double original_y{}, changed_y{};
        for (size_t c = 0; c < 3; ++c) {
            original_y += test.params.scene[c] * weights[c];
            changed_y += test.params.changed[c] * weights[c];
        }
        if ((original_y < test.exposure[2]) !=
            (changed_y < (test.exposure[0] + test.exposure[1]) * test.exposure[2]))
            ++crossings;
    }
    if (crossings < 2)
        throw std::runtime_error("NR exposure smoke did not cross both inverse branches");
    std::printf("NR native exposure smoke: %zu cases; black, both knee crossings, chroma and "
                "constant identity passed\n",
                cases.size());
}
