#include "testutil.h"

#include <opus.h>

#include <cmath>

FakeCaptureSource::FakeCaptureSource(bool startResult)
    : startResult(startResult)
{
}

bool FakeCaptureSource::start(SampleCallback callback)
{
    starts++;
    if (!startResult) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Callback = std::move(callback);
    return true;
}

void FakeCaptureSource::stop()
{
    stops++;
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Callback = nullptr;
}

bool FakeCaptureSource::deliver(const float* samples, int count)
{
    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_Callback) {
        return false;
    }
    m_Callback(samples, count);
    return true;
}

SineGenerator::SineGenerator(double frequency, double amplitude)
    : m_Frequency(frequency), m_Amplitude(amplitude), m_Phase(0)
{
}

std::vector<float> SineGenerator::next(int count)
{
    std::vector<float> out(count);
    for (int i = 0; i < count; i++) {
        out[i] = (float)(m_Amplitude * std::sin(2.0 * M_PI * m_Frequency * (double)(m_Phase++) / 48000.0));
    }
    return out;
}

std::vector<float> decodeOpus(const unsigned char* data, int length)
{
    int err = 0;
    OpusDecoder* decoder = opus_decoder_create(48000, 1, &err);
    std::vector<float> out(5760);
    int n = opus_decode_float(decoder, data, length, out.data(), (int)out.size(), 0);
    opus_decoder_destroy(decoder);
    if (n < 0) {
        return {};
    }
    out.resize(n);
    return out;
}

StatefulOpusDecoder::StatefulOpusDecoder()
{
    int err = 0;
    m_Decoder = opus_decoder_create(48000, 1, &err);
}

StatefulOpusDecoder::~StatefulOpusDecoder()
{
    opus_decoder_destroy(m_Decoder);
}

std::vector<float> StatefulOpusDecoder::decode(const unsigned char* data, int length)
{
    std::vector<float> out(5760);
    int n = opus_decode_float(m_Decoder, data, length, out.data(), (int)out.size(), 0);
    if (n < 0) {
        return {};
    }
    out.resize(n);
    return out;
}

double rms(const std::vector<float>& samples, size_t from)
{
    if (samples.size() <= from) {
        return 0;
    }
    double sum = 0;
    for (size_t i = from; i < samples.size(); i++) {
        sum += (double)samples[i] * samples[i];
    }
    return std::sqrt(sum / (double)(samples.size() - from));
}

std::vector<QObject*>& micTestObjects()
{
    static std::vector<QObject*> objects;
    return objects;
}
