#pragma once

// secure-desktop-stream: factory for the platform clipboard backend.
// macOS: NSPasteboard (clipboard_mac.mm). Other platforms: unavailable (clipboard_null.cpp).

#include "sdstypes.h"

#include <QString>

#include <memory>

namespace Sds {

// The general (system) clipboard by default. Tests pass a private pasteboard name so they
// never touch the user's clipboard; such a pasteboard is released when the backend is
// destroyed.
std::unique_ptr<IClipboardBackend> createPlatformClipboard(const QString& privatePasteboardName = QString());

} // namespace Sds
