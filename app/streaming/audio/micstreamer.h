#pragma once

// secure-desktop-stream: microphone capture -> Opus -> LiSendMicrophoneOpusData()
// (DESIGN.md section 7.2/7.4). Platform independent; the capture source is injected.

#include "miccapture.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct OpusEncoder;

// Encodes 48 kHz mono PCM into 20 ms Opus frames: VOIP, 48 kbit/s VBR, in-band FEC, no DTX.
class MicOpusEncoder
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSamples = 960;      // 20 ms
    static constexpr int kBitrate = 48000;
    static constexpr int kMaxPacketBytes = 1372;   // largest frame that fits one mic datagram
    static constexpr int kExpectedLossPercent = 10;

    using PacketSink = std::function<void(const unsigned char* data, int length)>;

    MicOpusEncoder();
    ~MicOpusEncoder();

    MicOpusEncoder(const MicOpusEncoder&) = delete;
    MicOpusEncoder& operator=(const MicOpusEncoder&) = delete;

    // Creates the libopus encoder. Returns false on failure.
    bool init();

    // Appends samples and emits one packet per complete 20 ms frame. Returns the number of
    // packets emitted, or -1 if the encoder is not initialized or encoding failed.
    int pushSamples(const float* samples, int count, const PacketSink& sink);

    // Drops any partial frame and resets the codec state (used after mute).
    void reset();

    // Number of buffered samples that do not yet form a complete frame
    int pendingSamples() const { return (int)m_Pending.size(); }

    // Encoder settings as reported by libopus (for tests); -1 if unavailable
    int queryBitrate() const;
    int queryInbandFec() const;
    int queryDtx() const;
    int queryVbr() const;
    int queryApplication() const;

private:
    OpusEncoder* m_Encoder;
    std::vector<float> m_Pending;
};

class MicStreamer
{
public:
    // Sends one encoded frame; returns 0 on success (LiSendMicrophoneOpusData() semantics)
    using SendFunction = std::function<int(const unsigned char* data, int length)>;

    // Capture backlog beyond which the oldest samples are dropped (200 ms)
    static constexpr int kMaxQueuedSamples = MicOpusEncoder::kSampleRate / 5;

    // Uses LiSendMicrophoneOpusData() when send is empty
    explicit MicStreamer(std::unique_ptr<MicCaptureSource> source, SendFunction send = {});
    ~MicStreamer();

    MicStreamer(const MicStreamer&) = delete;
    MicStreamer& operator=(const MicStreamer&) = delete;

    // Starts the encoder thread and the capture source. Returns false if either fails.
    bool start();

    // Stops capture first, then the encoder thread. Idempotent.
    void stop();

    bool isRunning() const { return m_Running; }

    // Muted: captured audio is discarded and nothing is sent
    void setMuted(bool muted);
    bool isMuted() const { return m_Muted; }

    uint64_t framesSent() const { return m_FramesSent; }
    uint64_t framesFailed() const { return m_FramesFailed; }
    uint64_t samplesDropped() const { return m_SamplesDropped; }

    // Feeds samples as if they came from the capture source (used by the source callback)
    void onCapturedSamples(const float* samples, int count);

private:
    void encoderThreadProc();

    std::unique_ptr<MicCaptureSource> m_Source;
    SendFunction m_Send;
    MicOpusEncoder m_Encoder;

    std::mutex m_Mutex;
    std::condition_variable m_Cond;
    std::deque<float> m_Queue;
    bool m_StopRequested;
    bool m_ResetRequested;

    std::thread m_Thread;
    std::atomic<bool> m_Running;
    std::atomic<bool> m_Muted;
    std::atomic<uint64_t> m_FramesSent;
    std::atomic<uint64_t> m_FramesFailed;
    std::atomic<uint64_t> m_SamplesDropped;
};
