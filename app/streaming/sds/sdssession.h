#pragma once

// secure-desktop-stream: SDL glue for clipboard sync and file transfer during a stream.
// Created by Session::exec() once the connection is up, destroyed before LiStopConnection().

#include "SDL_compat.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

// SDL_USEREVENT codes (upstream uses 0 and 100-105; keep ours apart)
#define SDL_CODE_SDS_TICK 200
#define SDL_CODE_SDS_WAKE 201

class NvComputer;
class StreamingPreferences;
class SdsController;

namespace Overlay {
class OverlayManager;
}

class SdsSession
{
public:
    // Returns nullptr when clipboard sync and file transfer are both disabled in the
    // settings. otherOverlayActive returns true while another feature (gamepad mouse mode)
    // owns the status overlay.
    static std::unique_ptr<SdsSession> create(NvComputer* computer,
                                              StreamingPreferences* prefs,
                                              Overlay::OverlayManager& overlay,
                                              std::function<bool()> otherOverlayActive);

    ~SdsSession();

    SdsSession(const SdsSession&) = delete;
    SdsSession& operator=(const SdsSession&) = delete;

    static bool isSdsUserEvent(int code)
    {
        return code == SDL_CODE_SDS_TICK || code == SDL_CODE_SDS_WAKE;
    }

    // Main thread (SDL loop)
    void handleUserEvent(int code);
    void handleDropFile(const char* path);
    void handleDropFiles(double nx, double ny, const QStringList& paths);
    void downloadOutboxNow();

private:
    SdsSession(Overlay::OverlayManager& overlay, std::function<bool()> otherOverlayActive);

    static Uint32 timerCallback(Uint32 interval, void* param);
    static quint64 nowMs();

    void showStatus(const QString& text);
    void hideStatus();
    bool ownsOverlay() const;

    std::unique_ptr<SdsController> m_Controller;
    SDL_TimerID m_Timer;
    Overlay::OverlayManager& m_Overlay;
    std::function<bool()> m_OtherOverlayActive;
    QByteArray m_ShownText;
};
