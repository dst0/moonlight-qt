#include "settings/streamingpreferences.h"
#include "streaming/audio/miccapture.h"
#include "tests.h"
#include "utils.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace MicPermission {
Status status() { return Status::NotDetermined; }
void request(std::function<void(bool)> callback)
{
    if (callback) {
        callback(false);
    }
}
} // namespace MicPermission

namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
} // namespace WMUtils

class TestPreferences : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_OrigOrg = QCoreApplication::organizationName();
        m_OrigApp = QCoreApplication::applicationName();
        m_OrigFormat = QSettings::defaultFormat();
    }

    void cleanup()
    {
        QSettings::setDefaultFormat(m_OrigFormat);
        QCoreApplication::setOrganizationName(m_OrigOrg);
        QCoreApplication::setApplicationName(m_OrigApp);
    }

    void defaultsAndPersistenceEdgeCases()
    {
        QTemporaryDir profileDir;
        QVERIFY(profileDir.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("SdsUnitTest"));
        QCoreApplication::setApplicationName(QStringLiteral("PrefsTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profileDir.path());

        StreamingPreferences* prefs = StreamingPreferences::get();
        prefs->reload();

        // Fresh defaults: matchHostResolution & revertHostResolution both default to false
        QCOMPARE(prefs->hostEncoderConfig, StreamingPreferences::HEC_AUTO);
        QVERIFY(!prefs->matchHostResolution);
        QVERIFY(!prefs->revertHostResolution);
        QVERIFY(!prefs->effectiveRevertHostResolution());
        QVERIFY(prefs->hostAdapterOverrides.isEmpty());
        QVERIFY(prefs->hostCaptureAdapterOverrides.isEmpty());
        QCOMPARE(StreamingPreferences::hostEncoderQueryValue(StreamingPreferences::HEC_AUTO), QStringLiteral("auto"));
        QCOMPARE(StreamingPreferences::hostEncoderQueryValue(StreamingPreferences::HEC_HARDWARE_VAAPI), QStringLiteral("vaapi"));
        QCOMPARE(StreamingPreferences::hostEncoderQueryValue(StreamingPreferences::HEC_HARDWARE_VULKAN), QStringLiteral("vulkan"));
        QCOMPARE(StreamingPreferences::hostEncoderQueryValue(StreamingPreferences::HEC_SOFTWARE), QStringLiteral("software"));

        // Per-host GPU adapter override trimming, persistence, and removal
        QSignalSpy adapterSpy(prefs, &StreamingPreferences::hostAdapterOverridesChanged);
        prefs->setHostAdapterOverride(QStringLiteral(""), QStringLiteral("/dev/dri/by-path/ignored"));
        QCOMPARE(adapterSpy.count(), 0);
        prefs->setHostAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("  /dev/dri/by-path/pci-0000:01:00.0-render  "));
        QCOMPARE(adapterSpy.count(), 1);
        QCOMPARE(prefs->hostAdapterOverride(QStringLiteral("fp_host_a")), QStringLiteral("/dev/dri/by-path/pci-0000:01:00.0-render"));
        QCOMPARE(prefs->hostAdapterOverride(QStringLiteral("fp_other")), QString());

        // Setting identical value does not emit again
        prefs->setHostAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("/dev/dri/by-path/pci-0000:01:00.0-render"));
        QCOMPARE(adapterSpy.count(), 1);

        QSignalSpy captureAdapterSpy(prefs, &StreamingPreferences::hostCaptureAdapterOverridesChanged);
        prefs->setHostCaptureAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("  /dev/dri/by-path/pci-0000:02:00.0-render  "));
        QCOMPARE(captureAdapterSpy.count(), 1);
        QCOMPARE(prefs->hostCaptureAdapterOverride(QStringLiteral("fp_host_a")), QStringLiteral("/dev/dri/by-path/pci-0000:02:00.0-render"));
        QCOMPARE(prefs->hostAdapterOverride(QStringLiteral("fp_host_a")), QStringLiteral("/dev/dri/by-path/pci-0000:01:00.0-render"));

        prefs->hostEncoderConfig = StreamingPreferences::HEC_SOFTWARE;
        prefs->matchHostResolution = true;
        prefs->revertHostResolution = false;
        QVERIFY(!prefs->effectiveRevertHostResolution());
        prefs->save();

        prefs->hostEncoderConfig = StreamingPreferences::HEC_AUTO;
        prefs->matchHostResolution = false;
        prefs->revertHostResolution = true;
        QVERIFY(!prefs->effectiveRevertHostResolution()); // match=false always guards revert=true!
        prefs->reload();

        QCOMPARE(prefs->hostEncoderConfig, StreamingPreferences::HEC_SOFTWARE);
        QVERIFY(prefs->matchHostResolution);
        QVERIFY(!prefs->revertHostResolution);
        QCOMPARE(prefs->hostAdapterOverride(QStringLiteral("fp_host_a")), QStringLiteral("/dev/dri/by-path/pci-0000:01:00.0-render"));
        QCOMPARE(prefs->hostCaptureAdapterOverride(QStringLiteral("fp_host_a")), QStringLiteral("/dev/dri/by-path/pci-0000:02:00.0-render"));

        // Clear adapter override with whitespace -> removed cleanly
        prefs->setHostAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("   "));
        QCOMPARE(adapterSpy.count(), 2);
        QVERIFY(prefs->hostAdapterOverride(QStringLiteral("fp_host_a")).isEmpty());
        prefs->setHostAdapterOverride(QStringLiteral("fp_host_a"), QString());
        QCOMPARE(adapterSpy.count(), 2);

        prefs->setHostCaptureAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("   "));
        QCOMPARE(captureAdapterSpy.count(), 2);
        QVERIFY(prefs->hostCaptureAdapterOverride(QStringLiteral("fp_host_a")).isEmpty());

        // Out-of-range host encoder enum in persisted settings clamps safely to HEC_AUTO
        {
            QSettings raw;
            raw.setValue(QStringLiteral("sdshostencoder"), 999);
            raw.setValue(QStringLiteral("sdsmatchhostres"), false);
            raw.setValue(QStringLiteral("sdsreverthostres"), true);
        }
        prefs->reload();
        QCOMPARE(prefs->hostEncoderConfig, StreamingPreferences::HEC_AUTO);
        QVERIFY(!prefs->matchHostResolution);
        QVERIFY(prefs->revertHostResolution);
        QVERIFY(!prefs->effectiveRevertHostResolution());

        // Per-PC adaptive dirty-tile refinement defaults false, persists keyed by cert fingerprint
        QSignalSpy adaptiveSpy(prefs, &StreamingPreferences::hostAdaptiveRefinementChanged);
        QVERIFY(!prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->adaptiveRefinementForHost(QString()));

        prefs->setAdaptiveRefinementForHost(QString(), true);
        QCOMPARE(adaptiveSpy.count(), 0);
        prefs->setAdaptiveRefinementForHost(QStringLiteral("fp_host_a"), true);
        QCOMPARE(adaptiveSpy.count(), 1);
        QVERIFY(prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_a")));
        prefs->save();

        prefs->setAdaptiveRefinementForHost(QStringLiteral("fp_host_a"), false);
        QCOMPARE(adaptiveSpy.count(), 2);
        QVERIFY(!prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_a")));
        prefs->reload();
        QVERIFY(prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->adaptiveRefinementForHost(QStringLiteral("fp_other")));

        // Clearing removes the map entry cleanly
        prefs->setAdaptiveRefinementForHost(QStringLiteral("fp_host_a"), false);
        QVERIFY(!prefs->hostAdaptiveRefinement.contains(QStringLiteral("fp_host_a")));
        prefs->save();
        prefs->reload();
        QVERIFY(!prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_a")));
    }

private:
    QString m_OrigOrg;
    QString m_OrigApp;
    QSettings::Format m_OrigFormat = QSettings::NativeFormat;
};

int runPreferencesTests(int argc, char** argv)
{
    TestPreferences test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_preferences.moc"
