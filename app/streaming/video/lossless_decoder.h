#pragma once

#include "decoder.h"
#include "lossless_client.h"
#include "lossless_codec.h"
#include "lossless_renderer.h"
#include "streaming/video/overlaymanager.h"

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <memory>

class LosslessVideoDecoder final : public IVideoDecoder, public Overlay::IOverlayRenderer
{
public:
    LosslessVideoDecoder();
    ~LosslessVideoDecoder() override;

    bool initialize(PDECODER_PARAMETERS params) override;
    bool isHardwareAccelerated() override;
    bool isAlwaysFullScreen() override;
    bool isHdrSupported() override;
    int getDecoderCapabilities() override;
    int getDecoderColorspace() override;
    int getDecoderColorRange() override;
    QSize getDecoderMaxResolution() override;
    int submitDecodeUnit(PDECODE_UNIT du) override;
    void renderFrameOnMainThread() override;
    void setHdrMode(bool enabled) override;
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;
    void notifyOverlayUpdated(Overlay::OverlayType type) override;

private:
    static int decodeThreadProc(void* context);
    void runDecodeLoop();
    void updateDebugOverlayIfDue();

    DECODER_PARAMETERS m_Params {};
    std::unique_ptr<ILosslessRenderer> m_Renderer;
    std::unique_ptr<SdsLosslessClient> m_Client;

    SDL_Thread* m_DecodeThread = nullptr;
    std::atomic_bool m_Stopping {false};
    bool m_OverlayRegistered = false;

    std::atomic<std::uint64_t> m_ReceivedFrames {0};
    std::atomic<std::uint64_t> m_DecodedFrames {0};
    std::atomic<std::uint64_t> m_RenderedFrames {0};
    std::atomic<std::uint64_t> m_HashVerifiedFrames {0};
    std::atomic<std::uint64_t> m_HashMismatches {0};
    std::atomic<std::uint64_t> m_TotalBytes {0};
    std::atomic<std::uint64_t> m_LastDecodeUs {0};
    std::atomic<std::uint64_t> m_LastHashUs {0};
    std::atomic<int> m_StreamWidth {0};
    std::atomic<int> m_StreamHeight {0};
    std::atomic_bool m_LastHostGpu {false};

    std::uint64_t m_LastOverlayMs = 0;
    std::uint64_t m_LastOverlayBytes = 0;
    std::uint64_t m_LastOverlayDecoded = 0;
    std::uint64_t m_LastOverlayRendered = 0;
};
