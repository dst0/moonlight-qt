// secure-desktop-stream: platforms without a microphone capture backend (everything but macOS
// for now). The mic stream is never requested because micPassthrough cannot take effect.
#include "miccapture.h"

std::unique_ptr<MicCaptureSource> createPlatformMicCaptureSource(bool)
{
    return nullptr;
}

namespace MicPermission
{
    Status status()
    {
        return Status::Unsupported;
    }

    void request(std::function<void(bool granted)> callback)
    {
        if (callback) {
            callback(false);
        }
    }
}
