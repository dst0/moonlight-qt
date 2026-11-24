#include "micstreamer.h"

#include <Limelight.h>
#include <opus.h>

#include <SDL.h>

#include <algorithm>

MicOpusEncoder::MicOpusEncoder()
    : m_Encoder(nullptr)
{
}

MicOpusEncoder::~MicOpusEncoder()
{
    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
    }
}

bool MicOpusEncoder::init()
{
    int err = OPUS_OK;

    if (m_Encoder != nullptr) {
        return true;
    }

    m_Encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (m_Encoder == nullptr || err != OPUS_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: opus_encoder_create() failed: %s",
                     opus_strerror(err));
        m_Encoder = nullptr;
        return false;
    }

    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_PACKET_LOSS_PERC(kExpectedLossPercent));
    opus_encoder_ctl(m_Encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(m_Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    m_Pending.reserve(kFrameSamples * 4);
    return true;
}

int MicOpusEncoder::pushSamples(const float* samples, int count, const PacketSink& sink)
{
    unsigned char packet[kMaxPacketBytes];
    int emitted = 0;

    if (m_Encoder == nullptr || count < 0 || (count > 0 && samples == nullptr)) {
        return -1;
    }

    m_Pending.insert(m_Pending.end(), samples, samples + count);

    size_t offset = 0;
    while (m_Pending.size() - offset >= (size_t)kFrameSamples) {
        int bytes = opus_encode_float(m_Encoder, &m_Pending[offset], kFrameSamples, packet, sizeof(packet));
        offset += kFrameSamples;
        if (bytes < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Microphone: opus_encode_float() failed: %s",
                         opus_strerror(bytes));
            m_Pending.erase(m_Pending.begin(), m_Pending.begin() + offset);
            return -1;
        }

        // DTX is off, so every frame produces at least the TOC byte
        if (bytes > 0 && sink) {
            sink(packet, bytes);
        }
        emitted++;
    }

    m_Pending.erase(m_Pending.begin(), m_Pending.begin() + offset);
    return emitted;
}

void MicOpusEncoder::reset()
{
    m_Pending.clear();
    if (m_Encoder != nullptr) {
        opus_encoder_ctl(m_Encoder, OPUS_RESET_STATE);
    }
}

#define QUERY_CTL(request)                                      \
    do {                                                        \
        opus_int32 value = -1;                                  \
        if (m_Encoder == nullptr ||                             \
            opus_encoder_ctl(m_Encoder, request(&value)) != OPUS_OK) { \
            return -1;                                          \
        }                                                       \
        return value;                                           \
    } while (0)

int MicOpusEncoder::queryBitrate() const { QUERY_CTL(OPUS_GET_BITRATE); }
int MicOpusEncoder::queryInbandFec() const { QUERY_CTL(OPUS_GET_INBAND_FEC); }
int MicOpusEncoder::queryDtx() const { QUERY_CTL(OPUS_GET_DTX); }
int MicOpusEncoder::queryVbr() const { QUERY_CTL(OPUS_GET_VBR); }
int MicOpusEncoder::queryApplication() const { QUERY_CTL(OPUS_GET_APPLICATION); }

#undef QUERY_CTL

MicStreamer::MicStreamer(std::unique_ptr<MicCaptureSource> source, SendFunction send)
    : m_Source(std::move(source)),
      m_Send(std::move(send)),
      m_StopRequested(false),
      m_ResetRequested(false),
      m_Running(false),
      m_Muted(false),
      m_FramesSent(0),
      m_FramesFailed(0),
      m_SamplesDropped(0)
{
    if (!m_Send) {
        m_Send = [](const unsigned char* data, int length) {
            return LiSendMicrophoneOpusData(data, length);
        };
    }
}

MicStreamer::~MicStreamer()
{
    stop();
}

bool MicStreamer::start()
{
    if (m_Running) {
        return true;
    }

    if (!m_Source || !m_Encoder.init()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_StopRequested = false;
        m_ResetRequested = false;
        m_Queue.clear();
    }

    m_Thread = std::thread(&MicStreamer::encoderThreadProc, this);
    m_Running = true;

    if (!m_Source->start([this](const float* samples, int count) {
            onCapturedSamples(samples, count);
        })) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone: capture source '%s' failed to start",
                    m_Source->name());
        stop();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone: streaming from '%s'",
                m_Source->name());
    return true;
}

void MicStreamer::stop()
{
    if (!m_Running && !m_Thread.joinable()) {
        return;
    }

    // Stop the source first so no callback races with the encoder shutdown
    if (m_Source) {
        m_Source->stop();
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_StopRequested = true;
    }
    m_Cond.notify_all();

    if (m_Thread.joinable()) {
        m_Thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.clear();
    }
    m_Encoder.reset();
    m_Running = false;
}

void MicStreamer::setMuted(bool muted)
{
    bool wasMuted = m_Muted.exchange(muted);
    if (wasMuted != muted) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.clear();
        if (!muted) {
            // Start a fresh talk spurt: no stale codec prediction or partial frame
            m_ResetRequested = true;
        }
    }
}

void MicStreamer::onCapturedSamples(const float* samples, int count)
{
    if (count <= 0 || samples == nullptr || m_Muted || !m_Running) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_StopRequested) {
            return;
        }

        m_Queue.insert(m_Queue.end(), samples, samples + count);
        if (m_Queue.size() > (size_t)kMaxQueuedSamples) {
            size_t excess = m_Queue.size() - kMaxQueuedSamples;
            m_Queue.erase(m_Queue.begin(), m_Queue.begin() + excess);
            m_SamplesDropped += excess;
        }
    }
    m_Cond.notify_one();
}

void MicStreamer::encoderThreadProc()
{
    std::vector<float> chunk;
    chunk.reserve(kMaxQueuedSamples);

    for (;;) {
        bool reset;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cond.wait(lock, [this] {
                return m_StopRequested || m_ResetRequested || m_Queue.size() >= (size_t)MicOpusEncoder::kFrameSamples;
            });
            if (m_StopRequested) {
                return;
            }

            reset = m_ResetRequested;
            m_ResetRequested = false;
            chunk.assign(m_Queue.begin(), m_Queue.end());
            m_Queue.clear();
        }

        if (reset) {
            m_Encoder.reset();
        }

        if (!chunk.empty()) {
            m_Encoder.pushSamples(chunk.data(), (int)chunk.size(), [this](const unsigned char* data, int length) {
                if (m_Muted) {
                    return;
                }
                if (m_Send(data, length) == 0) {
                    m_FramesSent++;
                }
                else {
                    m_FramesFailed++;
                }
            });
        }
    }
}
