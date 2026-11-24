#pragma once

// secure-desktop-stream: microphone capture abstraction (DESIGN.md section 7.4).
//
// A capture source delivers 48 kHz mono float PCM in arbitrary chunk sizes from its own
// thread. The macOS implementation (miccapture_mac.mm) uses AVAudioEngine with voice
// processing; other platforms currently have no capture backend.

#include <functional>
#include <memory>

class MicCaptureSource
{
public:
    // Called from the capture thread with 48 kHz mono samples in [-1, 1]
    using SampleCallback = std::function<void(const float* samples, int count)>;

    virtual ~MicCaptureSource() = default;

    // Starts capturing. Returns false if the device could not be opened.
    virtual bool start(SampleCallback callback) = 0;

    // Stops capturing. After it returns, the callback is never invoked again. Idempotent.
    virtual void stop() = 0;

    virtual const char* name() const = 0;
};

// Creates the platform capture source, or nullptr when the platform has none.
std::unique_ptr<MicCaptureSource> createPlatformMicCaptureSource(bool echoCancellation);

namespace MicPermission
{
    enum class Status
    {
        Authorized,
        Denied,
        NotDetermined,
        Unsupported,
    };

    // Current microphone authorization (never prompts)
    Status status();

    // Asks the user for microphone access if not determined yet. The callback runs on an
    // arbitrary thread with the final answer.
    void request(std::function<void(bool granted)> callback);
}
