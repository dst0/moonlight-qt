#include "boxartmanager.h"
#include "../path.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>

namespace {

QString resolveVerifiedBoxArtDir(const QString& rootPath, const QString& uuid, bool createIfMissing)
{
    if (rootPath.isEmpty() || !NvComputer::isValidUuid(uuid)) {
        return QString();
    }

    const QString hostDirName = QString::fromLatin1(
        QCryptographicHash::hash(uuid.toUtf8(), QCryptographicHash::Sha256).toHex());

    QDir rootDir(rootPath);
    if (createIfMissing && !rootDir.exists()) {
        rootDir.mkpath(QStringLiteral("."));
    }

    QFileInfo rootInfo(rootDir.absolutePath());
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink()) {
        return QString();
    }
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        return QString();
    }

    const QString hostPath = QDir(canonicalRoot).filePath(hostDirName);
    QFileInfo hostInfo(hostPath);
    if (hostInfo.isSymLink()) {
        return QString();
    }
    if (!hostInfo.exists()) {
        if (!createIfMissing) {
            return QString();
        }
        if (!QDir(canonicalRoot).mkdir(hostDirName)) {
            return QString();
        }
        hostInfo.refresh();
    }
    if (hostInfo.isSymLink() || !hostInfo.isDir()) {
        return QString();
    }

    const QString canonicalHost = hostInfo.canonicalFilePath();
    if (canonicalHost.isEmpty() ||
            QFileInfo(canonicalHost).dir().canonicalPath() != canonicalRoot ||
            canonicalHost != QDir(canonicalRoot).filePath(hostDirName)) {
        return QString();
    }

    return canonicalHost;
}

} // namespace

BoxArtManager::BoxArtManager(QObject *parent) :
    QObject(parent),
    m_BoxArtDir(Path::getBoxArtCacheDir()),
    m_ThreadPool(this)
{
    // 4 is a good balance between fast loading for large
    // app grids and not crushing GFE with tons of requests
    // and causing UI jank from constantly stalling to decode
    // new images.
    m_ThreadPool.setMaxThreadCount(4);
    if (!m_BoxArtDir.exists()) {
        m_BoxArtDir.mkpath(".");
    }
}

QString
BoxArtManager::getFilePathForBoxArt(NvComputer* computer, int appId)
{
    if (computer == nullptr) {
        return QString();
    }

    QString uuid;
    {
        QReadLocker lock(&computer->lock);
        uuid = computer->uuid;
    }

    const QString hostDirPath = resolveVerifiedBoxArtDir(Path::getBoxArtCacheDir(), uuid, true);
    if (hostDirPath.isEmpty()) {
        return QString();
    }

    return QDir(hostDirPath).filePath(QString::number(appId) + ".png");
}

class NetworkBoxArtLoadTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    NetworkBoxArtLoadTask(BoxArtManager* boxArtManager, NvComputer* computer, NvApp& app)
        : m_Bam(boxArtManager),
          m_Computer(computer),
          m_App(app)
    {
        connect(this, &NetworkBoxArtLoadTask::boxArtFetchCompleted,
                boxArtManager, &BoxArtManager::handleBoxArtLoadComplete);
    }

signals:
    void boxArtFetchCompleted(NvComputer* computer, NvApp app, QUrl image);

private:
    void run()
    {
        QUrl image = m_Bam->loadBoxArtFromNetwork(m_Computer, m_App.id);
        if (image.isEmpty()) {
            // Give it another shot if it fails once
            image = m_Bam->loadBoxArtFromNetwork(m_Computer, m_App.id);
        }
        emit boxArtFetchCompleted(m_Computer, m_App, image);
    }

    BoxArtManager* m_Bam;
    NvComputer* m_Computer;
    NvApp m_App;
};

QUrl BoxArtManager::loadBoxArt(NvComputer* computer, NvApp& app)
{
    // Try to open the cached file if it exists and contains data
    const QString cachePath = getFilePathForBoxArt(computer, app.id);
    if (cachePath.isEmpty()) {
        return QUrl("qrc:/res/no_app_image.png");
    }

    QFileInfo cacheInfo(cachePath);
    if (cacheInfo.isSymLink()) {
        return QUrl("qrc:/res/no_app_image.png");
    }

    QFile cacheFile(cachePath);
    if (cacheFile.exists() && cacheFile.size() > 0) {
        return QUrl::fromLocalFile(cacheFile.fileName());
    }

    // If we get here, we need to fetch asynchronously.
    // Kick off a worker on our thread pool to do just that.
    NetworkBoxArtLoadTask* netLoadTask = new NetworkBoxArtLoadTask(this, computer, app);
    m_ThreadPool.start(netLoadTask);

    // Return the placeholder then we can notify the caller
    // later when the real image is ready.
    return QUrl("qrc:/res/no_app_image.png");
}

void BoxArtManager::deleteBoxArt(NvComputer* computer)
{
    if (computer == nullptr) {
        return;
    }

    QString uuid;
    {
        QReadLocker lock(&computer->lock);
        uuid = computer->uuid;
    }

    const QString hostDirPath = resolveVerifiedBoxArtDir(Path::getBoxArtCacheDir(), uuid, false);
    if (hostDirPath.isEmpty()) {
        return;
    }

    QDir dir(hostDirPath);
    dir.removeRecursively();
}

void BoxArtManager::handleBoxArtLoadComplete(NvComputer* computer, NvApp app, QUrl image)
{
    if (!image.isEmpty()) {
        emit boxArtLoadComplete(computer, app, image);
    }
}

QUrl BoxArtManager::loadBoxArtFromNetwork(NvComputer* computer, int appId)
{
    QString cachePath = getFilePathForBoxArt(computer, appId);
    if (cachePath.isEmpty() || QFileInfo(cachePath).isSymLink()) {
        return QUrl();
    }

    NvHTTP http(computer);
    QImage image;
    try {
        image = http.getBoxArt(appId);
    } catch (...) {}

    // Cache the box art on disk if it loaded
    if (!image.isNull()) {
        if (image.save(cachePath)) {
            return QUrl::fromLocalFile(cachePath);
        }
        else {
            // A failed save() may leave a zero byte file. Make sure that's removed.
            QFile(cachePath).remove();
        }
    }

    return QUrl();
}

#include "boxartmanager.moc"
