#pragma once

#include "lossless_renderer.h"
#include "lossless_sdl.h"

#ifdef Q_OS_LINUX

class LosslessVulkanRenderer final : public ILosslessRenderer
{
public:
    LosslessVulkanRenderer() = default;
    ~LosslessVulkanRenderer() override;

    bool initialize(SDL_Window* window,
                    int width,
                    int height,
                    bool enableVsync,
                    Overlay::OverlayManager* overlayManager,
                    bool preferGpuCompute) override;

    bool presentPacket(const std::uint8_t* packet,
                       std::size_t len,
                       const sds::lossless::frame_header_t& hdr,
                       bool useGpuCompute,
                       bool verifyHash,
                       bool& outHashOk,
                       std::uint64_t& outDecodeUs,
                       std::uint64_t& outHashUs) override;

    void renderOnMainThread() override;
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;
    void notifyOverlayUpdated(Overlay::OverlayType type) override;

    const char* backendName() const override
    {
        return m_UsingGpuCompute ? "Vulkan-GPU-Compute" : "Vulkan-CPU-SIMD";
    }
    bool isGpuComputeActive() const override { return m_UsingGpuCompute; }
    bool supportsGpuCompute() const override { return m_GpuReady; }

private:
    struct Impl;
    Impl* m_Impl = nullptr;
    LosslessSdlRenderer m_Presenter;
    bool m_GpuReady = false;
    bool m_UsingGpuCompute = false;
};

#endif
