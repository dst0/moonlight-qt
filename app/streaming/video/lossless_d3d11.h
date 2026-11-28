#pragma once

#include "lossless_renderer.h"

#ifdef Q_OS_WIN32

#include <QMutex>

class LosslessD3D11Renderer final : public ILosslessRenderer
{
public:
    LosslessD3D11Renderer() = default;
    ~LosslessD3D11Renderer() override;

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
        return m_UsingGpuCompute ? "D3D11-GPU-Compute" : "D3D11-CPU-SIMD";
    }
    bool isGpuComputeActive() const override { return m_UsingGpuCompute; }
    bool supportsGpuCompute() const override { return m_GpuReady; }

private:
    struct Impl;
    Impl* m_Impl = nullptr;

    SDL_Window* m_Window = nullptr;
    Overlay::OverlayManager* m_OverlayManager = nullptr;
    bool m_GpuReady = false;
    bool m_UsingGpuCompute = false;
    sds::lossless::cpu_decoder_t m_CpuDecoder;
    QMutex m_FrameMutex;
    bool m_OverlayDirty[Overlay::OverlayMax] {};
};

#endif
