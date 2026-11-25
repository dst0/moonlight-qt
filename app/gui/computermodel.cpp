#include "computermodel.h"

#include <QThreadPool>
#include <QCryptographicHash>

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);

    m_Computers = m_ComputerManager->getComputers();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    Q_ASSERT(index.row() >= 0 && index.row() < m_Computers.count());

    NvComputer* computer = m_Computers[index.row()];
    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case PairedRole:
        return computer->pairState == NvComputer::PS_PAIRED;
    case BusyRole:
        return computer->currentGameId != 0;
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case ServerSupportedRole:
        return computer->isSupportedServerVersion;
    case FileClipboardApprovedRole:
        return !computer->serverCert.isNull() && computer->pairState == NvComputer::PS_PAIRED &&
                StreamingPreferences::get()->fileClipboardHosts.contains(QString::fromLatin1(
                    computer->serverCert.digest(QCryptographicHash::Sha256).toHex()));
    case HasMultipleAdaptersRole:
        return computer->pairState == NvComputer::PS_PAIRED && computer->sdsAdapters.size() > 1;
    case HasHostCaptureAdapterRole:
        return computer->pairState == NvComputer::PS_PAIRED &&
               !computer->isNvidiaServerSoftware &&
               computer->hasSdsFeature(QStringLiteral("host_capture_adapter"));
    case AdaptersRole: {
        QVariantList list;
        for (const NvHostAdapter& adapter : computer->sdsAdapters) {
            QVariantMap entry;
            entry.insert("id", adapter.id);
            entry.insert("name", adapter.name);
            entry.insert("pciSlot", adapter.pciSlot);
            entry.insert("isDefault", adapter.isDefault);
            entry.insert("isCaptureDefault", adapter.isCaptureDefault);
            list.append(entry);
        }
        return list;
    }
    case SelectedAdapterIdRole:
    case SelectedCaptureAdapterIdRole: {
        if (computer->serverCert.isNull() || computer->pairState != NvComputer::PS_PAIRED) {
            return QString();
        }
        QString fp = QString::fromLatin1(computer->serverCert.digest(QCryptographicHash::Sha256).toHex());
        auto prefs = StreamingPreferences::get();
        return role == SelectedCaptureAdapterIdRole
                ? prefs->hostCaptureAdapterOverride(fp)
                : prefs->hostAdapterOverride(fp);
    }
    case SupportsAdaptiveTileRefinementRole:
        return computer->pairState == NvComputer::PS_PAIRED &&
               computer->hasSdsFeature(QStringLiteral("adaptive_tile_refinement"));
    case AdaptiveTileRefinementEnabledRole: {
        if (computer->serverCert.isNull() || computer->pairState != NvComputer::PS_PAIRED) {
            return false;
        }
        QString fp = QString::fromLatin1(computer->serverCert.digest(QCryptographicHash::Sha256).toHex());
        return StreamingPreferences::get()->adaptiveRefinementForHost(fp);
    }
    case SupportsHostEncoderRole:
        return computer->pairState == NvComputer::PS_PAIRED &&
               !computer->isNvidiaServerSoftware &&
               computer->hasSdsFeature(QStringLiteral("host_encoder"));
    case DetailsRole: {
        QString state, pairState;

        switch (computer->state) {
        case NvComputer::CS_ONLINE:
            state = tr("Online");
            break;
        case NvComputer::CS_OFFLINE:
            state = tr("Offline");
            break;
        default:
            state = tr("Unknown");
            break;
        }

        switch (computer->pairState) {
        case NvComputer::PS_PAIRED:
            pairState = tr("Paired");
            break;
        case NvComputer::PS_NOT_PAIRED:
            pairState = tr("Unpaired");
            break;
        default:
            pairState = tr("Unknown");
            break;
        }

        return tr("Name: %1").arg(computer->name) + '\n' +
               tr("Status: %1").arg(state) + '\n' +
               tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
               tr("UUID: %1").arg(computer->uuid) + '\n' +
               tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
               tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
               tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
               tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
               tr("MAC Address: %1").arg(computer->macAddress.isEmpty() ? tr("Unknown") : QString(computer->macAddress.toHex(':'))) + '\n' +
               tr("Pair State: %1").arg(pairState) + '\n' +
               tr("Running Game ID: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->currentGameId) : tr("Unknown")) + '\n' +
               tr("HTTPS Port: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->activeHttpsPort) : tr("Unknown"));
    }
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[PairedRole] = "paired";
    names[BusyRole] = "busy";
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[ServerSupportedRole] = "serverSupported";
    names[DetailsRole] = "details";
    names[FileClipboardApprovedRole] = "fileClipboardApproved";
    names[HasMultipleAdaptersRole] = "hasMultipleAdapters";
    names[AdaptersRole] = "adapters";
    names[SelectedAdapterIdRole] = "selectedAdapterId";
    names[HasHostCaptureAdapterRole] = "hasHostCaptureAdapter";
    names[SelectedCaptureAdapterIdRole] = "selectedCaptureAdapterId";
    names[SupportsAdaptiveTileRefinementRole] = "supportsAdaptiveTileRefinement";
    names[AdaptiveTileRefinementEnabledRole] = "adaptiveTileRefinementEnabled";
    names[SupportsHostEncoderRole] = "supportsHostEncoder";

    return names;
}

QString ComputerModel::fileClipboardFingerprint(int computerIndex) const
{
    if (computerIndex < 0 || computerIndex >= m_Computers.size()) return {};
    const auto computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    if (computer->pairState != NvComputer::PS_PAIRED || computer->serverCert.isNull()) return {};
    return QString::fromLatin1(computer->serverCert.digest(QCryptographicHash::Sha256).toHex());
}

void ComputerModel::setHostAdapter(int computerIndex, QString adapterId)
{
    const auto fingerprint = fileClipboardFingerprint(computerIndex);
    if (fingerprint.isEmpty()) {
        return;
    }
    auto prefs = StreamingPreferences::get();
    prefs->setHostAdapterOverride(fingerprint, adapterId);
    prefs->save();
    emit dataChanged(index(computerIndex), index(computerIndex), {SelectedAdapterIdRole});
}

void ComputerModel::setHostCaptureAdapter(int computerIndex, QString adapterId)
{
    const auto fingerprint = fileClipboardFingerprint(computerIndex);
    if (fingerprint.isEmpty()) {
        return;
    }
    auto prefs = StreamingPreferences::get();
    prefs->setHostCaptureAdapterOverride(fingerprint, adapterId);
    prefs->save();
    emit dataChanged(index(computerIndex), index(computerIndex), {SelectedCaptureAdapterIdRole});
}

void ComputerModel::setAdaptiveTileRefinement(int computerIndex, bool enabled)
{
    const auto fingerprint = fileClipboardFingerprint(computerIndex);
    if (fingerprint.isEmpty()) {
        return;
    }
    auto prefs = StreamingPreferences::get();
    prefs->setAdaptiveRefinementForHost(fingerprint, enabled);
    prefs->save();
    emit dataChanged(index(computerIndex), index(computerIndex), {AdaptiveTileRefinementEnabledRole});
}

QVariantMap ComputerModel::getPcSettings(int computerIndex) const
{
    QVariantMap result;
    if (computerIndex < 0 || computerIndex >= m_Computers.size()) {
        return result;
    }

    const auto computer = m_Computers[computerIndex];
    QVariantList adapterList;
    QString fp;
    bool paired = false;
    bool online = false;
    bool capabilitiesKnown = false;
    bool supportsAdaptive = false;
    bool supportsHostEncoder = false;
    bool hasHostCaptureAdapter = false;
    {
        QReadLocker lock(&computer->lock);
        paired = (computer->pairState == NvComputer::PS_PAIRED);
        online = (computer->state == NvComputer::CS_ONLINE);
        capabilitiesKnown = online || computer->sdsVersion > 0;
        if (paired && !computer->serverCert.isNull()) {
            fp = QString::fromLatin1(computer->serverCert.digest(QCryptographicHash::Sha256).toHex());
        }
        supportsAdaptive = paired && computer->hasSdsFeature(QStringLiteral("adaptive_tile_refinement"));
        supportsHostEncoder = paired && !computer->isNvidiaServerSoftware &&
                              computer->hasSdsFeature(QStringLiteral("host_encoder"));
        hasHostCaptureAdapter = paired && !computer->isNvidiaServerSoftware &&
                                computer->hasSdsFeature(QStringLiteral("host_capture_adapter"));
        for (const NvHostAdapter& adapter : computer->sdsAdapters) {
            QVariantMap entry;
            entry.insert(QStringLiteral("id"), adapter.id);
            entry.insert(QStringLiteral("name"), adapter.name);
            entry.insert(QStringLiteral("pciSlot"), adapter.pciSlot);
            entry.insert(QStringLiteral("isDefault"), adapter.isDefault);
            entry.insert(QStringLiteral("isCaptureDefault"), adapter.isCaptureDefault);
            adapterList.append(entry);
        }
    }

    auto prefs = StreamingPreferences::get();
    const int playAudioState = prefs->playAudioOnHostOverrideState(fp);
    const int muteState = (playAudioState < 0) ? -1 : (playAudioState == 0 ? 1 : 0);

    result.insert(QStringLiteral("fingerprint"), fp);
    result.insert(QStringLiteral("hostOnline"), online);
    result.insert(QStringLiteral("capabilitiesKnown"), capabilitiesKnown);
    result.insert(QStringLiteral("supportsAdaptiveTileRefinement"), supportsAdaptive);
    result.insert(QStringLiteral("adaptiveTileRefinement"), prefs->adaptiveRefinementForHost(fp));
    result.insert(QStringLiteral("supportsHostEncoder"), supportsHostEncoder);
    result.insert(QStringLiteral("hostEncoderConfig"), static_cast<int>(prefs->hostEncoderForHost(fp)));
    result.insert(QStringLiteral("hasMultipleAdapters"), paired && adapterList.size() > 1);
    result.insert(QStringLiteral("hasHostCaptureAdapter"), (paired && adapterList.size() > 1) && hasHostCaptureAdapter);
    result.insert(QStringLiteral("adapters"), adapterList);
    result.insert(QStringLiteral("selectedAdapterId"), prefs->hostAdapterOverride(fp));
    result.insert(QStringLiteral("selectedCaptureAdapterId"), prefs->hostCaptureAdapterOverride(fp));
    result.insert(QStringLiteral("matchHostResolutionOverride"), prefs->matchHostResolutionOverrideState(fp));
    result.insert(QStringLiteral("revertHostResolutionOverride"), prefs->revertHostResolutionOverrideState(fp));
    result.insert(QStringLiteral("muteHostSpeakersOverride"), muteState);
    result.insert(QStringLiteral("gameOptimizationsOverride"), prefs->gameOptimizationsOverrideState(fp));
    result.insert(QStringLiteral("quitAppAfterOverride"), prefs->quitAppAfterOverrideState(fp));
    return result;
}

bool ComputerModel::setPcSettings(int computerIndex, const QString& expectedFingerprint, const QVariantMap& settings)
{
    if (expectedFingerprint.isEmpty()) {
        return false;
    }

    int targetIndex = -1;
    if (computerIndex >= 0 && computerIndex < m_Computers.size() &&
        fileClipboardFingerprint(computerIndex) == expectedFingerprint) {
        targetIndex = computerIndex;
    } else {
        for (int i = 0; i < m_Computers.size(); ++i) {
            if (fileClipboardFingerprint(i) == expectedFingerprint) {
                targetIndex = i;
                break;
            }
        }
    }
    if (targetIndex < 0) {
        return false;
    }

    const QString& fingerprint = expectedFingerprint;
    auto prefs = StreamingPreferences::get();
    if (settings.contains(QStringLiteral("adaptiveTileRefinement"))) {
        prefs->setAdaptiveRefinementForHost(fingerprint, settings.value(QStringLiteral("adaptiveTileRefinement")).toBool());
    }
    if (settings.contains(QStringLiteral("hostEncoderConfig"))) {
        prefs->setHostEncoderForHost(
            fingerprint,
            static_cast<StreamingPreferences::HostEncoderConfig>(settings.value(QStringLiteral("hostEncoderConfig")).toInt()));
    }
    if (settings.contains(QStringLiteral("selectedAdapterId"))) {
        prefs->setHostAdapterOverride(fingerprint, settings.value(QStringLiteral("selectedAdapterId")).toString());
    }
    if (settings.contains(QStringLiteral("selectedCaptureAdapterId"))) {
        prefs->setHostCaptureAdapterOverride(fingerprint, settings.value(QStringLiteral("selectedCaptureAdapterId")).toString());
    }
    if (settings.contains(QStringLiteral("matchHostResolutionOverride"))) {
        prefs->setMatchHostResolutionOverride(fingerprint, settings.value(QStringLiteral("matchHostResolutionOverride")).toInt());
    }
    if (settings.contains(QStringLiteral("revertHostResolutionOverride"))) {
        prefs->setRevertHostResolutionOverride(fingerprint, settings.value(QStringLiteral("revertHostResolutionOverride")).toInt());
    }
    if (settings.contains(QStringLiteral("muteHostSpeakersOverride"))) {
        const int muteState = settings.value(QStringLiteral("muteHostSpeakersOverride")).toInt();
        const int playAudioState = (muteState < 0) ? -1 : (muteState > 0 ? 0 : 1);
        prefs->setPlayAudioOnHostOverride(fingerprint, playAudioState);
    }
    if (settings.contains(QStringLiteral("gameOptimizationsOverride"))) {
        prefs->setGameOptimizationsOverride(fingerprint, settings.value(QStringLiteral("gameOptimizationsOverride")).toInt());
    }
    if (settings.contains(QStringLiteral("quitAppAfterOverride"))) {
        prefs->setQuitAppAfterOverride(fingerprint, settings.value(QStringLiteral("quitAppAfterOverride")).toInt());
    }

    prefs->save();
    emit dataChanged(index(targetIndex), index(targetIndex), {
        SelectedAdapterIdRole,
        SelectedCaptureAdapterIdRole,
        AdaptiveTileRefinementEnabledRole
    });
    return true;
}

bool ComputerModel::setFileClipboardApproval(int computerIndex, QString expectedFingerprint, bool approved)
{
    const auto fingerprint = fileClipboardFingerprint(computerIndex);
    if (fingerprint.isEmpty() || fingerprint != expectedFingerprint) return false;
    auto prefs = StreamingPreferences::get();
    prefs->fileClipboardHosts.removeAll(fingerprint);
    prefs->fileClipboardHosts.removeAll(QString());
    prefs->fileClipboardHosts.removeDuplicates();
    if (approved) {
        const bool hasOtherHosts = !prefs->fileClipboardHosts.isEmpty();
        prefs->fileClipboardHosts.append(fingerprint);
        // Never silently re-enable the global toggle if the user turned it off while other
        // approved hosts remained in fileClipboardHosts.
        if (!prefs->fileClipboard && !hasOtherHosts) {
            prefs->fileClipboard = true;
            emit prefs->fileClipboardChanged();
        }
    }
    prefs->save();
    emit dataChanged(index(computerIndex), index(computerIndex), {FileClipboardApprovedRole});
    return true;
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // We must currently be streaming a game to use this function
    Q_ASSERT(computer->currentGameId != 0);

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app);
        }
    }

    // We have a current running app but it's not in our app list
    Q_ASSERT(false);
    return nullptr;
}

void ComputerModel::deleteComputer(int computerIndex)
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());

    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);

    // m_Computer[computerIndex] will be deleted by this call
    m_ComputerManager->deleteHost(m_Computers[computerIndex]);

    // Remove the now invalid item
    m_Computers.removeAt(computerIndex);

    endRemoveRows();
}

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer)
        : m_Computer(computer) {}

    void run()
    {
        m_Computer->wake();
    }

private:
    NvComputer* m_Computer;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());

    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(m_Computers[computerIndex]);
    QThreadPool::globalInstance()->start(wakeTask);
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

QString ComputerModel::generatePinString()
{
    return m_ComputerManager->generatePinString();
}

void ComputerModel::pairComputer(int computerIndex, QString pin)
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());

    m_ComputerManager->pairHost(m_Computers[computerIndex], pin);
}

void ComputerModel::handlePairingCompleted(NvComputer*, QString error)
{
    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = m_ComputerManager->getComputers();

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;
        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        emit dataChanged(createIndex(index, 0), createIndex(index, 0));
    }
}
