#include "lossless_decoder.h"

#include "lossless_d3d11.h"
#include "lossless_metal.h"
#include "lossless_sdl.h"
#include "lossless_vulkan.h"
#include "streaming/session.h"

#include <QDateTime>
#include <SDL.h>

#include <cstdio>

namespace {

sds::lossless::hash_algo_e toCodecHashAlgo(StreamingPreferences::LosslessHashMode mode)
{
    switch (mode) {
    case StreamingPreferences::LHM_BLAKE3:
        return sds::lossless::hash_algo_e::blake3;
    case StreamingPreferences::LHM_OFF:
        return sds::lossless::hash_algo_e::none;
    case StreamingPreferences::LHM_CRC32C:
    default:
        return sds::lossless::hash_algo_e::crc32c;
    }
}

const char* hashAlgoLabel(sds::lossless::hash_algo_e algo)
{
    switch (algo) {
    case sds::lossless::hash_algo_e::blake3:
        return "BLAKE3";
    case sds::lossless::hash_algo_e::crc32c:
        return "CRC32C";
    case sds::lossless::hash_algo_e::none:
    default:
        return "OFF";
    }
}

} // namespace

LosslessVideoDecoder::LosslessVideoDecoder() = default;

LosslessVideoDecoder::~LosslessVideoDecoder()
{
    m_Stopping.store(true, std::memory_order_release);
    if (m_Client) {
        m_Client->stop();
    }
    if (m_DecodeThread) {
        SDL_WaitThread(m_DecodeThread, nullptr);
        m_DecodeThread = nullptr;
    }
    if (m_OverlayRegistered) {
        if (Session* s = Session::get()) {
            s->getOverlayManager().setOverlayRenderer(nullptr);
        }
        m_OverlayRegistered = false;
    }
    m_Client.reset();
    m_Renderer.reset();
}

bool LosslessVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    if (!params) {
        return false;
    }
    m_Params = *params;
    m_StreamWidth.store(params->width, std::memory_order_release);
    m_StreamHeight.store(params->height, std::memory_order_release);

    if (params->testOnly) {
        return true;
    }

    Session* session = Session::get();
    Overlay::OverlayManager* overlayMgr = session ? &session->getOverlayManager() : nullptr;

    const bool preferGpu = (params->losslessDecoderMode != StreamingPreferences::LDM_CPU);
    const bool allowCpuFallback = true;

#ifdef Q_OS_MACOS
    {
        auto metal = std::make_unique<LosslessMetalRenderer>();
        if (metal->initialize(params->window, params->width, params->height,
                              params->enableVsync, overlayMgr, preferGpu)) {
            m_Renderer = std::move(metal);
        }
    }
#elif defined(Q_OS_WIN32)
    {
        auto d3d = std::make_unique<LosslessD3D11Renderer>();
        if (d3d->initialize(params->window, params->width, params->height,
                            params->enableVsync, overlayMgr, preferGpu)) {
            m_Renderer = std::move(d3d);
        }
    }
#elif defined(Q_OS_LINUX)
    if (preferGpu) {
        auto vk = std::make_unique<LosslessVulkanRenderer>();
        if (vk->initialize(params->window, params->width, params->height,
                           params->enableVsync, overlayMgr, true)) {
            m_Renderer = std::move(vk);
        }
    }
#endif

    if (!m_Renderer && allowCpuFallback) {
        auto sdl = std::make_unique<LosslessSdlRenderer>();
        if (sdl->initialize(params->window, params->width, params->height,
                            params->enableVsync, overlayMgr, false)) {
            m_Renderer = std::move(sdl);
        }
    }

    if (!m_Renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LosslessVideoDecoder: failed to initialize any lossless renderer");
        return false;
    }

    if (overlayMgr) {
        overlayMgr->setOverlayRenderer(this);
        m_OverlayRegistered = true;
    }

    SdsLosslessClient::Config cfg;
    cfg.host = params->losslessHostAddress;
    cfg.httpsPort = params->losslessHttpsPort;
    cfg.serverCert = params->losslessServerCert;
    cfg.clientCert = params->losslessClientCert;
    cfg.clientKey = params->losslessClientKey;
    cfg.hashAlgo = toCodecHashAlgo(params->losslessHashMode);

    m_Client = std::make_unique<SdsLosslessClient>(cfg);
    m_Client->start();

    m_Stopping.store(false, std::memory_order_release);
    m_DecodeThread = SDL_CreateThread(LosslessVideoDecoder::decodeThreadProc, "LosslessDecode", this);
    if (!m_DecodeThread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LosslessVideoDecoder: failed to create decode thread: %s", SDL_GetError());
        m_Client->stop();
        m_Client.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "LosslessVideoDecoder initialized (%s, hash=%s)",
                m_Renderer->backendName(),
                hashAlgoLabel(cfg.hashAlgo));
    return true;
}

bool LosslessVideoDecoder::isHardwareAccelerated()
{
    return m_Renderer ? m_Renderer->isGpuComputeActive() : false;
}

bool LosslessVideoDecoder::isAlwaysFullScreen()
{
    return false;
}

bool LosslessVideoDecoder::isHdrSupported()
{
    return false;
}

int LosslessVideoDecoder::getDecoderCapabilities()
{
    return CAPABILITY_DIRECT_SUBMIT;
}

int LosslessVideoDecoder::getDecoderColorspace()
{
    return COLORSPACE_REC_709;
}

int LosslessVideoDecoder::getDecoderColorRange()
{
    return COLOR_RANGE_FULL;
}

QSize LosslessVideoDecoder::getDecoderMaxResolution()
{
    return QSize(sds::lossless::MAX_FRAME_DIM, sds::lossless::MAX_FRAME_DIM);
}

int LosslessVideoDecoder::submitDecodeUnit(PDECODE_UNIT)
{
    // LosslessVideoDecoder receives SDLL frames over SdsLosslessClient instead of RTP/FFmpeg.
    return DR_OK;
}

void LosslessVideoDecoder::renderFrameOnMainThread()
{
    if (!m_Renderer) {
        return;
    }
    m_Renderer->renderOnMainThread();
    m_RenderedFrames.fetch_add(1, std::memory_order_relaxed);
    updateDebugOverlayIfDue();
}

void LosslessVideoDecoder::setHdrMode(bool)
{
    // Bit-exact SDLL v1 lossless mode operates strictly in canonical SDR 8-bit RGB24.
}

bool LosslessVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    if (!m_Renderer) {
        return true;
    }
    return m_Renderer->notifyWindowChanged(info);
}

void LosslessVideoDecoder::notifyOverlayUpdated(Overlay::OverlayType type)
{
    if (m_Renderer) {
        m_Renderer->notifyOverlayUpdated(type);
    }
}

int LosslessVideoDecoder::decodeThreadProc(void* context)
{
    static_cast<LosslessVideoDecoder*>(context)->runDecodeLoop();
    return 0;
}

void LosslessVideoDecoder::runDecodeLoop()
{
    const bool useGpuCompute = (m_Params.losslessDecoderMode != StreamingPreferences::LDM_CPU);
    const bool verifyHash = (m_Params.losslessHashMode != StreamingPreferences::LHM_OFF);

    while (!m_Stopping.load(std::memory_order_acquire)) {
        SdsLosslessClient::FramePacket pkt;
        if (!m_Client || !m_Client->popFrame(pkt, 25)) {
            continue;
        }
        if (m_Stopping.load(std::memory_order_acquire)) {
            break;
        }

        m_ReceivedFrames.fetch_add(1, std::memory_order_relaxed);
        m_TotalBytes.fetch_add(static_cast<std::uint64_t>(pkt.bytes.size()), std::memory_order_relaxed);
        m_StreamWidth.store(pkt.header.width, std::memory_order_release);
        m_StreamHeight.store(pkt.header.height, std::memory_order_release);
        m_LastHostGpu.store((pkt.header.flags & sds::lossless::FLAG_GPU_ENCODED) != 0, std::memory_order_release);

        bool hashOk = true;
        std::uint64_t decodeUs = 0;
        std::uint64_t hashUs = 0;
        const bool ok = m_Renderer->presentPacket(
            reinterpret_cast<const std::uint8_t*>(pkt.bytes.constData()),
            static_cast<std::size_t>(pkt.bytes.size()),
            pkt.header,
            useGpuCompute,
            verifyHash,
            hashOk,
            decodeUs,
            hashUs);

        if (!ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "LosslessVideoDecoder: frame #%llu decode failed, requesting keyframe",
                        static_cast<unsigned long long>(pkt.header.frame_id));
            if (m_Client) {
                m_Client->requestKeyframe();
            }
            continue;
        }

        m_DecodedFrames.fetch_add(1, std::memory_order_relaxed);
        m_LastDecodeUs.store(decodeUs, std::memory_order_relaxed);
        m_LastHashUs.store(hashUs, std::memory_order_relaxed);

        if (verifyHash && pkt.header.hash_algo != sds::lossless::hash_algo_e::none) {
            m_HashVerifiedFrames.fetch_add(1, std::memory_order_relaxed);
            if (!hashOk) {
                const auto cnt = m_HashMismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "LosslessVideoDecoder: BIT-EXACT HASH MISMATCH #%llu on frame #%llu (%s), requesting keyframe!",
                            static_cast<unsigned long long>(cnt),
                            static_cast<unsigned long long>(pkt.header.frame_id),
                            hashAlgoLabel(pkt.header.hash_algo));
                if (m_Client) {
                    m_Client->requestKeyframe();
                }
            }
        }

        SDL_Event event {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

void LosslessVideoDecoder::updateDebugOverlayIfDue()
{
    Session* session = Session::get();
    if (!session || !session->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug)) {
        return;
    }
    const std::uint64_t nowMs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    if (m_LastOverlayMs == 0) {
        m_LastOverlayMs = nowMs;
        return;
    }
    const std::uint64_t elapsedMs = nowMs - m_LastOverlayMs;
    if (elapsedMs < 500) {
        return;
    }

    const std::uint64_t curBytes = m_TotalBytes.load(std::memory_order_relaxed);
    const std::uint64_t curDec = m_DecodedFrames.load(std::memory_order_relaxed);
    const std::uint64_t curRen = m_RenderedFrames.load(std::memory_order_relaxed);

    const double secs = static_cast<double>(elapsedMs) / 1000.0;
    const double mbps = (static_cast<double>(curBytes - m_LastOverlayBytes) * 8.0) / (secs * 1000000.0);
    const double decFps = static_cast<double>(curDec - m_LastOverlayDecoded) / secs;
    const double renFps = static_cast<double>(curRen - m_LastOverlayRendered) / secs;

    m_LastOverlayMs = nowMs;
    m_LastOverlayBytes = curBytes;
    m_LastOverlayDecoded = curDec;
    m_LastOverlayRendered = curRen;

    const auto hashAlgo = toCodecHashAlgo(m_Params.losslessHashMode);
    const auto mismatches = m_HashMismatches.load(std::memory_order_relaxed);
    const double decMs = static_cast<double>(m_LastDecodeUs.load(std::memory_order_relaxed)) / 1000.0;
    const double hashMs = static_cast<double>(m_LastHashUs.load(std::memory_order_relaxed)) / 1000.0;

    char buf[512] {};
    std::snprintf(buf, sizeof(buf),
                  "Stream: %dx%d | SDLL v1 Bit-Exact Lossless RGB24\n"
                  "Host: %s | Client: %s\n"
                  "Incoming/Render FPS: %.1f / %.1f | Bitrate: %.2f Mbps\n"
                  "Hash (%s): %s (mismatches: %llu)\n"
                  "Decode / Hash time: %.2f ms / %.2f ms",
                  m_StreamWidth.load(std::memory_order_relaxed),
                  m_StreamHeight.load(std::memory_order_relaxed),
                  m_LastHostGpu.load(std::memory_order_relaxed) ? "GPU-Compute" : "CPU-SIMD",
                  m_Renderer ? m_Renderer->backendName() : "none",
                  decFps,
                  renFps,
                  mbps,
                  hashAlgoLabel(hashAlgo),
                  (hashAlgo == sds::lossless::hash_algo_e::none) ? "DISABLED" : (mismatches == 0 ? "BIT-EXACT OK" : "RECOVERED/MISMATCH"),
                  static_cast<unsigned long long>(mismatches),
                  decMs,
                  hashMs);
    session->getOverlayManager().updateOverlayText(Overlay::OverlayDebug, buf);
}
