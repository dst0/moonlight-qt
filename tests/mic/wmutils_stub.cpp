// WMUtils stubs for linking StreamingPreferences into the tests without SDL/X11/Wayland
#include "utils.h"

namespace WMUtils {
    bool isRunningX11() { return false; }
    bool isRunningNvidiaProprietaryDriverX11() { return false; }
    bool supportsDesktopGLWithEGL() { return false; }
    bool isRunningWayland() { return false; }
    bool isRunningWindowManager() { return true; }
    bool isRunningDesktopEnvironment() { return true; }
    bool hasDisplayUsableBounds() { return true; }
    QString getDrmCardOverride() { return QString(); }
    bool isGpuSlow() { return false; }
}
