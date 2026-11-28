#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        DetailsRole,
        FileClipboardApprovedRole,
        HasMultipleAdaptersRole,
        AdaptersRole,
        SelectedAdapterIdRole,
        HasHostCaptureAdapterRole,
        SelectedCaptureAdapterIdRole,
        SupportsAdaptiveTileRefinementRole,
        AdaptiveTileRefinementEnabledRole,
        SupportsHostEncoderRole,
        SupportsLosslessVideoRole
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);


    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);
    Q_INVOKABLE QString fileClipboardFingerprint(int computerIndex) const;
    Q_INVOKABLE bool setFileClipboardApproval(int computerIndex, QString expectedFingerprint, bool approved);
    Q_INVOKABLE void setHostAdapter(int computerIndex, QString adapterId);
    Q_INVOKABLE void setHostCaptureAdapter(int computerIndex, QString adapterId);
    Q_INVOKABLE void setAdaptiveTileRefinement(int computerIndex, bool enabled);
    Q_INVOKABLE QVariantMap getPcSettings(int computerIndex) const;
    Q_INVOKABLE bool setPcSettings(int computerIndex, const QString& expectedFingerprint, const QVariantMap& settings);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

signals:
    void pairingCompleted(QVariant error);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
};
