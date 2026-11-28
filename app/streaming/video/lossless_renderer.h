#pragma once

#include "lossless_codec.h"
#include "streaming/video/decoder.h"
#include "streaming/video/overlaymanager.h"

#include "SDL_compat.h"

#include <cstddef>
#include <cstdint>

class ILosslessRenderer
{
public:
    virtual ~ILosslessRenderer() = default;

    virtual bool initialize(SDL_Window* window,
                            int width,
                            int height,
                            bool enableVsync,
                            Overlay::OverlayManager* overlayManager,
                            bool preferGpuCompute) = 0;

    virtual bool presentPacket(const std::uint8_t* packet,
                               std::size_t len,
                               const sds::lossless::frame_header_t& hdr,
                               bool useGpuCompute,
                               bool verifyHash,
                               bool& outHashOk,
                               std::uint64_t& outDecodeUs,
                               std::uint64_t& outHashUs) = 0;

    virtual void renderOnMainThread() = 0;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) = 0;
    virtual void notifyOverlayUpdated(Overlay::OverlayType type) = 0;

    virtual const char* backendName() const = 0;
    virtual bool isGpuComputeActive() const = 0;
    virtual bool supportsGpuCompute() const = 0;
};
