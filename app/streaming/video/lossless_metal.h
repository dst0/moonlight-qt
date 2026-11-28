#pragma once

#include "lossless_renderer.h"

#include <QMutex>

#include <vector>

class LosslessMetalRenderer final : public ILosslessRenderer
{
public:
    LosslessMetalRenderer() = default;
    ~LosslessMetalRenderer() override;

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
        return m_UsingGpuCompute ? "Metal-GPU-Compute" : "Metal-CPU-SIMD";
    }
    bool isGpuComputeActive() const override { return m_UsingGpuCompute; }
    bool supportsGpuCompute() const override { return m_GpuPipelineReady; }

    // Headless Metal GPU decode helper for automated bit-exact cross-parity unit testing
    static bool runHeadlessGpuDecodeSelfTest(const std::vector<const std::vector<std::uint8_t>*>& packets,
                                             std::vector<std::uint8_t>& outFinalBgra,
                                             int& outWidth,
                                             int& outHeight,
                                             std::vector<std::uint32_t>& outCrc32cPerFrame);

private:
    struct Impl;
    Impl* m_Impl = nullptr;

    SDL_Window* m_Window = nullptr;
    Overlay::OverlayManager* m_OverlayManager = nullptr;
    int m_StreamWidth = 0;
    int m_StreamHeight = 0;
    bool m_EnableVsync = true;
    bool m_GpuPipelineReady = false;
    bool m_UsingGpuCompute = false;

    sds::lossless::cpu_decoder_t m_CpuDecoder;
    QMutex m_FrameMutex;
    bool m_OverlayDirty[Overlay::OverlayMax] {};
};
