#include "lossless_sdl.h"

#include "lossless_hash.h"
#include "streaming/session.h"
#include "streaming/streamutils.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#include <SDL_syswm.h>

#include <algorithm>
#include <cstring>

LosslessSdlRenderer::~LosslessSdlRenderer()
{
    for (int i = 0; i < Overlay::OverlayMax; ++i) {
        if (m_OverlayTextures[i]) {
            SDL_DestroyTexture(m_OverlayTextures[i]);
            m_OverlayTextures[i] = nullptr;
        }
    }
    if (m_Texture) {
        SDL_DestroyTexture(m_Texture);
        m_Texture = nullptr;
    }
    if (m_Renderer) {
        SDL_DestroyRenderer(m_Renderer);
        m_Renderer = nullptr;
    }
}

bool LosslessSdlRenderer::initialize(SDL_Window* window,
                                     int width,
                                     int height,
                                     bool enableVsync,
                                     Overlay::OverlayManager* overlayManager,
                                     bool)
{
    m_Window = window;
    m_StreamWidth = width;
    m_StreamHeight = height;
    m_OverlayManager = overlayManager;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    Uint32 rendererFlags = SDL_RENDERER_ACCELERATED;
    if (SDL_GetWindowWMInfo(window, &info)) {
        switch (info.subsystem) {
        case SDL_SYSWM_WINDOWS:
            if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN && enableVsync) {
                rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
            }
            break;
        case SDL_SYSWM_WAYLAND:
            break;
        default:
            if (enableVsync) {
                rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
            }
            break;
        }
    } else if (enableVsync) {
        rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
    }

#ifdef Q_OS_WIN32
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1", SDL_HINT_OVERRIDE);
#endif

    m_Renderer = SDL_CreateRenderer(window, -1, rendererFlags);
    if (!m_Renderer) {
        m_Renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!m_Renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LosslessSdlRenderer: SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    Session* session = Session::get();
    if (session != nullptr) {
        session->flushWindowEvents();
    } else {
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_WINDOWEVENT);
    }

    SDL_SetRenderDrawColor(m_Renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(m_Renderer);
    SDL_RenderPresent(m_Renderer);
    return true;
}

bool LosslessSdlRenderer::presentPacket(const std::uint8_t* packet,
                                        std::size_t len,
                                        const sds::lossless::frame_header_t& hdr,
                                        bool,
                                        bool verifyHash,
                                        bool& outHashOk,
                                        std::uint64_t& outDecodeUs,
                                        std::uint64_t& outHashUs)
{
    QElapsedTimer timer;
    timer.start();

    sds::lossless::decode_stats_t stats {};
    if (!m_Decoder.decode_frame(packet, len, verifyHash, nullptr, &stats)) {
        outHashOk = false;
        outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
        outHashUs = 0;
        return false;
    }

    const std::uint64_t totalUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
    outDecodeUs = totalUs;
    outHashUs = stats.hash_us;
    outHashOk = (!verifyHash || hdr.hash_algo == sds::lossless::hash_algo_e::none) ? true : stats.hash_matched;

    const int w = m_Decoder.width();
    const int h = m_Decoder.height();
    const int stride = m_Decoder.stride();
    const std::size_t bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);

    QMutexLocker lk(&m_FrameMutex);
    m_StagingBgra.resize(bytes);
    std::memcpy(m_StagingBgra.data(), m_Decoder.current_bgra(), bytes);
    m_StagingWidth = w;
    m_StagingHeight = h;
    m_StagingStride = stride;
    m_HasNewFrame = true;
    return true;
}

bool LosslessSdlRenderer::presentRawBgra(const std::uint8_t* bgra,
                                         int stride,
                                         int width,
                                         int height,
                                         const sds::lossless::frame_header_t& hdr,
                                         bool verifyHash,
                                         bool& outHashOk,
                                         std::uint64_t& outHashUs)
{
    QElapsedTimer htimer;
    htimer.start();
    if (verifyHash && hdr.hash_algo != sds::lossless::hash_algo_e::none) {
        const auto computed = sds::lossless::compute_frame_hash_bgra(hdr.hash_algo, bgra, stride, width, height);
        outHashOk = (computed == hdr.frame_hash);
    } else {
        outHashOk = true;
    }
    outHashUs = static_cast<std::uint64_t>(htimer.nsecsElapsed() / 1000);

    const std::size_t bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    QMutexLocker lk(&m_FrameMutex);
    m_StagingBgra.resize(bytes);
    std::memcpy(m_StagingBgra.data(), bgra, bytes);
    m_StagingWidth = width;
    m_StagingHeight = height;
    m_StagingStride = stride;
    m_HasNewFrame = true;
    return true;
}

void LosslessSdlRenderer::recreateTextureIfNeeded(int width, int height)
{
    if (m_Texture && m_TexWidth == width && m_TexHeight == height) {
        return;
    }
    if (m_Texture) {
        SDL_DestroyTexture(m_Texture);
        m_Texture = nullptr;
    }
    m_Texture = SDL_CreateTexture(m_Renderer,
                                  SDL_PIXELFORMAT_BGRA32,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  width,
                                  height);
    m_TexWidth = width;
    m_TexHeight = height;
}

void LosslessSdlRenderer::updateOverlayTexture(Overlay::OverlayType type)
{
    if (!m_OverlayManager || !m_Renderer) {
        return;
    }
    if (m_OverlayTextures[type]) {
        SDL_DestroyTexture(m_OverlayTextures[type]);
        m_OverlayTextures[type] = nullptr;
    }
    if (!m_OverlayManager->isOverlayEnabled(type)) {
        return;
    }
    SDL_Surface* surface = m_OverlayManager->getUpdatedOverlaySurface(type);
    if (!surface) {
        return;
    }
    m_OverlayTextures[type] = SDL_CreateTextureFromSurface(m_Renderer, surface);
    SDL_Rect& r = m_OverlayRects[type];
    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(m_Renderer, &outW, &outH);
    if (type == Overlay::OverlayStatusUpdate) {
        r.x = outW - surface->w - 10;
        r.y = outH - surface->h - 10;
    } else {
        r.x = 10;
        r.y = 10;
    }
    r.w = surface->w;
    r.h = surface->h;
    SDL_FreeSurface(surface);
}

void LosslessSdlRenderer::renderOnMainThread()
{
    if (!m_Renderer) {
        return;
    }

    int w = 0, h = 0, stride = 0;
    bool upload = false;
    {
        QMutexLocker lk(&m_FrameMutex);
        if (m_HasNewFrame && !m_StagingBgra.empty() && m_StagingWidth > 0 && m_StagingHeight > 0) {
            recreateTextureIfNeeded(m_StagingWidth, m_StagingHeight);
            if (m_Texture) {
                SDL_UpdateTexture(m_Texture, nullptr, m_StagingBgra.data(), m_StagingStride);
                m_HasPresented = true;
            }
            w = m_StagingWidth;
            h = m_StagingHeight;
            stride = m_StagingStride;
            m_HasNewFrame = false;
            upload = true;
        }
    }
    (void)stride;
    (void)upload;

    SDL_SetRenderDrawColor(m_Renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(m_Renderer);

    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(m_Renderer, &outW, &outH);

    if (m_Texture && m_HasPresented) {
        const int vw = (w > 0) ? w : m_TexWidth;
        const int vh = (h > 0) ? h : m_TexHeight;
        SDL_Rect src {0, 0, vw, vh};
        SDL_Rect dst {0, 0, outW, outH};
        if (vw > 0 && vh > 0 && outW > 0 && outH > 0) {
            StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
        }
        SDL_SetTextureScaleMode(m_Texture,
                                (dst.w == vw && dst.h == vh) ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        SDL_RenderCopy(m_Renderer, m_Texture, nullptr, &dst);
    }

    for (int i = 0; i < Overlay::OverlayMax; ++i) {
        const auto type = static_cast<Overlay::OverlayType>(i);
        if (m_OverlayDirty[i]) {
            m_OverlayDirty[i] = false;
            updateOverlayTexture(type);
        }
        if (m_OverlayManager && m_OverlayManager->isOverlayEnabled(type) && m_OverlayTextures[i]) {
            const int ow = m_OverlayRects[i].w;
            const int oh = m_OverlayRects[i].h;
            SDL_Rect r = (type == Overlay::OverlayStatusUpdate)
                ? SDL_Rect { outW - ow - 10, outH - oh - 10, ow, oh }
                : SDL_Rect { 10, 10, ow, oh };
            SDL_RenderCopy(m_Renderer, m_OverlayTextures[i], nullptr, &r);
        }
    }

    SDL_RenderPresent(m_Renderer);
}

bool LosslessSdlRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    auto unhandled = info->stateChangeFlags;
    unhandled &= ~WINDOW_STATE_CHANGE_SIZE;
    unhandled &= ~WINDOW_STATE_CHANGE_DISPLAY;
    return unhandled == 0;
}

void LosslessSdlRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    m_OverlayDirty[type] = true;
}
