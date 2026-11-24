#pragma once

#include "streaming/audio/miccapture.h"

#include <QObject>

#include <atomic>
#include <mutex>
#include <vector>

extern "C" void micTestInitCommonC(void);

// Capture source driven by the test: deliver() calls the streamer's callback like the
// AVAudioEngine tap thread would.
class FakeCaptureSource : public MicCaptureSource
{
public:
    explicit FakeCaptureSource(bool startResult = true);

    bool start(SampleCallback callback) override;
    void stop() override;
    const char* name() const override { return "fake"; }

    // Returns false when not started (callback absent)
    bool deliver(const float* samples, int count);

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    bool startResult;

private:
    std::mutex m_Lock;
    SampleCallback m_Callback;
};

// 48 kHz mono sine, continuous across calls
class SineGenerator
{
public:
    explicit SineGenerator(double frequency = 440.0, double amplitude = 0.25);
    std::vector<float> next(int count);

private:
    double m_Frequency;
    double m_Amplitude;
    uint64_t m_Phase;
};

// Decodes one Opus packet (48 kHz mono) with a fresh decoder; returns the samples (empty on error)
std::vector<float> decodeOpus(const unsigned char* data, int length);

struct OpusDecoder;

// Keeps decoder state across packets, as a real receiver does
class StatefulOpusDecoder
{
public:
    StatefulOpusDecoder();
    ~StatefulOpusDecoder();
    std::vector<float> decode(const unsigned char* data, int length);

private:
    OpusDecoder* m_Decoder;
};

// RMS of a sample range
double rms(const std::vector<float>& samples, size_t from = 0);

// Test object registry used by main.cpp
std::vector<QObject*>& micTestObjects();

#define MIC_REGISTER_TEST(Type) \
    static const bool s_registered_##Type = [] { micTestObjects().push_back(new Type()); return true; }()
