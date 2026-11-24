#pragma once

// secure-desktop-stream: per-stream microphone controller. Decides whether to capture,
// handles the macOS permission flow and the mute hotkey, and reports state for the overlay.
// All methods except the permission callback run on the SDL main thread.

#include "micstreamer.h"

#include <functional>
#include <memory>
#include <string>

// SDL user event posted when an asynchronous permission request completes (data1 = granted)
#define SDL_CODE_MIC_PERMISSION_RESULT 300

class MicSession
{
public:
    enum class State
    {
        Idle,                  // not started or stopped
        NotNegotiated,         // user enabled the mic but the host did not set up the mic stream
        WaitingForPermission,  // macOS permission prompt pending
        PermissionDenied,
        Unsupported,           // no capture backend on this platform
        Failed,                // capture or encoder failed to start
        Running,
    };

    struct Dependencies
    {
        // Whether moonlight-common-c negotiated the mic stream (LiIsMicrophoneStreamActive)
        std::function<bool()> streamNegotiated;
        std::function<MicPermission::Status()> permissionStatus;
        // Must eventually call the callback (any thread) with the answer
        std::function<void(std::function<void(bool)>)> requestPermission;
        std::function<std::unique_ptr<MicCaptureSource>(bool echoCancellation)> createSource;
        // Sends one Opus frame (defaults to LiSendMicrophoneOpusData)
        MicStreamer::SendFunction send;
        // Delivers the permission answer back to the main thread (defaults to an SDL user event)
        std::function<void(bool granted)> postPermissionResult;
        // Overlay text; empty text hides the message
        std::function<void(const std::string& text)> showStatus;
    };

    // Production dependencies (moonlight-common-c, AVFoundation, SDL events)
    static Dependencies platformDependencies();

    MicSession(bool echoCancellation, Dependencies deps);
    ~MicSession();

    MicSession(const MicSession&) = delete;
    MicSession& operator=(const MicSession&) = delete;

    // Called once the connection is established
    void begin();

    // Main thread, in response to SDL_CODE_MIC_PERMISSION_RESULT
    void onPermissionResult(bool granted);

    // Mute hotkey; returns the new muted state
    bool toggleMute();
    bool isMuted() const { return m_Muted; }

    // Called before LiStopConnection(); stops capture. Idempotent.
    void end();

    State state() const { return m_State; }
    MicStreamer* streamer() const { return m_Streamer.get(); }

    static const char* muteHotkeyDescription();

private:
    void startCapture();

    bool m_EchoCancellation;
    Dependencies m_Deps;
    State m_State;
    bool m_Muted;
    bool m_Ended;
    std::unique_ptr<MicStreamer> m_Streamer;
    // Shared with the permission callback so a late answer after end() is ignored
    std::shared_ptr<bool> m_Alive;
};
