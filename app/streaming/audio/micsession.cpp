#include "micsession.h"

#include <Limelight.h>
#include <SDL.h>

MicSession::Dependencies MicSession::platformDependencies()
{
    Dependencies deps;
    deps.streamNegotiated = [] { return LiIsMicrophoneStreamActive(); };
    deps.permissionStatus = [] { return MicPermission::status(); };
    deps.requestPermission = [](std::function<void(bool)> callback) { MicPermission::request(std::move(callback)); };
    deps.createSource = [](bool echoCancellation) { return createPlatformMicCaptureSource(echoCancellation); };
    deps.postPermissionResult = [](bool granted) {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_MIC_PERMISSION_RESULT;
        event.user.data1 = (void*)(uintptr_t)(granted ? 1 : 0);
        SDL_PushEvent(&event);
    };
    return deps;
}

MicSession::MicSession(bool echoCancellation, Dependencies deps)
    : m_EchoCancellation(echoCancellation),
      m_Deps(std::move(deps)),
      m_State(State::Idle),
      m_Muted(false),
      m_Ended(false),
      m_Alive(std::make_shared<bool>(true))
{
}

MicSession::~MicSession()
{
    end();
}

const char* MicSession::muteHotkeyDescription()
{
#ifdef __APPLE__
    return "Ctrl+Option+Shift+U";
#else
    return "Ctrl+Alt+Shift+U";
#endif
}

void MicSession::begin()
{
    if (m_Ended || m_State != State::Idle) {
        return;
    }

    if (!m_Deps.streamNegotiated || !m_Deps.streamNegotiated()) {
        m_State = State::NotNegotiated;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone: the host did not offer microphone passthrough for this stream");
        return;
    }

    switch (m_Deps.permissionStatus ? m_Deps.permissionStatus() : MicPermission::Status::Unsupported) {
    case MicPermission::Status::Authorized:
        startCapture();
        break;

    case MicPermission::Status::NotDetermined: {
        m_State = State::WaitingForPermission;
        std::weak_ptr<bool> alive = m_Alive;
        auto post = m_Deps.postPermissionResult;
        m_Deps.requestPermission([alive, post](bool granted) {
            // The session may have ended while the prompt was open
            if (!alive.expired() && post) {
                post(granted);
            }
        });
        break;
    }

    case MicPermission::Status::Denied:
        m_State = State::PermissionDenied;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone: access denied in System Settings > Privacy & Security > Microphone");
        if (m_Deps.showStatus) {
            m_Deps.showStatus("Microphone access denied (System Settings > Privacy & Security)");
        }
        break;

    case MicPermission::Status::Unsupported:
        m_State = State::Unsupported;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone: capture is not supported on this platform");
        break;
    }
}

void MicSession::onPermissionResult(bool granted)
{
    if (m_Ended || m_State != State::WaitingForPermission) {
        return;
    }

    if (granted) {
        startCapture();
    }
    else {
        m_State = State::PermissionDenied;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Microphone: access was not granted");
    }
}

void MicSession::startCapture()
{
    std::unique_ptr<MicCaptureSource> source = m_Deps.createSource ? m_Deps.createSource(m_EchoCancellation) : nullptr;
    if (!source) {
        m_State = State::Unsupported;
        return;
    }

    m_Streamer = std::make_unique<MicStreamer>(std::move(source), m_Deps.send);
    m_Streamer->setMuted(m_Muted);
    if (!m_Streamer->start()) {
        m_Streamer.reset();
        m_State = State::Failed;
        if (m_Deps.showStatus) {
            m_Deps.showStatus("Microphone could not be started");
        }
        return;
    }

    m_State = State::Running;
}

bool MicSession::toggleMute()
{
    m_Muted = !m_Muted;
    if (m_Streamer) {
        m_Streamer->setMuted(m_Muted);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone %s", m_Muted ? "muted" : "unmuted");
    if (m_Deps.showStatus) {
        if (m_Muted) {
            m_Deps.showStatus(std::string("Microphone muted\nPress ") + muteHotkeyDescription() + " to unmute");
        }
        else {
            m_Deps.showStatus(std::string());
        }
    }
    return m_Muted;
}

void MicSession::end()
{
    if (m_Ended) {
        return;
    }

    m_Ended = true;
    m_Alive.reset();
    if (m_Streamer) {
        m_Streamer->stop();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone: stopped (%llu frames sent, %llu failed, %llu samples dropped)",
                    (unsigned long long)m_Streamer->framesSent(),
                    (unsigned long long)m_Streamer->framesFailed(),
                    (unsigned long long)m_Streamer->samplesDropped());
        m_Streamer.reset();
    }
    m_State = State::Idle;
}
