#pragma once

#include "lossless_renderer.h"

#include <QMutex>

#include <cstdint>
#include <vector>

class LosslessSdlRenderer final : public ILosslessRenderer
{
public:
    LosslessSdlRenderer() = default;
    ~LosslessSdlRenderer() override;

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

    const char* backendName() const override { return "SDL2-CPU-SIMD"; }
    bool isGpuComputeActive() const override { return false; }
    bool supportsGpuCompute() const override { return false; }

    // Shared software presentation entry point used by Vulkan compute fallback too
    bool presentRawBgra(const std::uint8_t* bgra,
                        int stride,
                        int width,
                        int height,
                        const sds::lossless::frame_header_t& hdr,
                        bool verifyHash,
                        bool& outHashOk,
                        std::uint64_t& outHashUs);

private:
    void recreateTextureIfNeeded(int width, int height);
    void updateOverlayTexture(Overlay::OverlayType type);

    SDL_Window* m_Window = nullptr;
    SDL_Renderer* m_Renderer = nullptr;
    SDL_Texture* m_Texture = nullptr;
    int m_StreamWidth = 0;
    int m_StreamHeight = 0;
    int m_TexWidth = 0;
    int m_TexHeight = 0;

    Overlay::OverlayManager* m_OverlayManager = nullptr;
    SDL_Texture* m_OverlayTextures[Overlay::OverlayMax] {};
    SDL_Rect m_OverlayRects[Overlay::OverlayMax] {};
    bool m_OverlayDirty[Overlay::OverlayMax] {};

    sds::lossless::cpu_decoder_t m_Decoder;

    QMutex m_FrameMutex;
    std::vector<std::uint8_t> m_StagingBgra;
    int m_StagingWidth = 0;
    int m_StagingHeight = 0;
    int m_StagingStride = 0;
    bool m_HasNewFrame = false;
    bool m_HasPresented = false;
};
