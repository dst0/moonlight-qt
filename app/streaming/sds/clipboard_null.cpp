#include "clipboard_platform.h"

namespace Sds {

namespace {

class NullClipboard : public IClipboardBackend
{
public:
    bool isAvailable() const override { return false; }
    qint64 changeCount() override { return 0; }
    ReadResult read(qint64, qint64) override { return ReadResult(); }
    qint64 write(const ClipboardItem&) override { return 0; }
};

} // namespace

std::unique_ptr<IClipboardBackend> createPlatformClipboard(const QString&)
{
    return std::make_unique<NullClipboard>();
}

} // namespace Sds
