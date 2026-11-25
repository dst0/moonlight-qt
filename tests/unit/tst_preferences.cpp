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

        // Per-PC host encoder & universal host overrides (Global Default / Enabled / Disabled)
        QSignalSpy hostEncSpy(prefs, &StreamingPreferences::hostEncoderOverridesChanged);
        QSignalSpy matchSpy(prefs, &StreamingPreferences::hostMatchResolutionOverridesChanged);
        QSignalSpy revertSpy(prefs, &StreamingPreferences::hostRevertResolutionOverridesChanged);
        QSignalSpy gameOptSpy(prefs, &StreamingPreferences::hostGameOptimizationsOverridesChanged);
        QSignalSpy quitSpy(prefs, &StreamingPreferences::hostQuitAppAfterOverridesChanged);
        QSignalSpy audioSpy(prefs, &StreamingPreferences::hostPlayAudioOverridesChanged);

        // Unset host inherits global defaults
        prefs->hostEncoderConfig = StreamingPreferences::HEC_AUTO;
        prefs->matchHostResolution = false;
        prefs->revertHostResolution = true;
        prefs->gameOptimizations = true;
        prefs->quitAppAfter = false;
        prefs->playAudioOnHost = false;
        QCOMPARE(prefs->hostEncoderForHost(QStringLiteral("fp_host_a")), StreamingPreferences::HEC_AUTO);
        QCOMPARE(prefs->matchHostResolutionOverrideState(QStringLiteral("fp_host_a")), -1);
        QVERIFY(!prefs->matchHostResolutionForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->effectiveRevertHostResolutionForHost(QStringLiteral("fp_host_a")));
        QVERIFY(prefs->gameOptimizationsForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->quitAppAfterForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->playAudioOnHostForHost(QStringLiteral("fp_host_a")));

        // Override only host A
        prefs->setHostEncoderForHost(QStringLiteral("fp_host_a"), StreamingPreferences::HEC_HARDWARE_VAAPI);
        prefs->setMatchHostResolutionOverride(QStringLiteral("fp_host_a"), 1);
        prefs->setRevertHostResolutionOverride(QStringLiteral("fp_host_a"), 1);
        prefs->setGameOptimizationsOverride(QStringLiteral("fp_host_a"), 0);
        prefs->setQuitAppAfterOverride(QStringLiteral("fp_host_a"), 1);
        prefs->setPlayAudioOnHostOverride(QStringLiteral("fp_host_a"), 1);
        QCOMPARE(hostEncSpy.count(), 1);
        QCOMPARE(matchSpy.count(), 1);
        QCOMPARE(revertSpy.count(), 1);
        QCOMPARE(gameOptSpy.count(), 1);
        QCOMPARE(quitSpy.count(), 1);
        QCOMPARE(audioSpy.count(), 1);
        prefs->save();
        prefs->reload();

        QCOMPARE(prefs->hostEncoderForHost(QStringLiteral("fp_host_a")), StreamingPreferences::HEC_HARDWARE_VAAPI);
        QCOMPARE(prefs->hostEncoderForHost(QStringLiteral("fp_host_b")), StreamingPreferences::HEC_AUTO);
        QCOMPARE(prefs->matchHostResolutionOverrideState(QStringLiteral("fp_host_a")), 1);
        QVERIFY(prefs->matchHostResolutionForHost(QStringLiteral("fp_host_a")));
        QVERIFY(prefs->effectiveRevertHostResolutionForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->matchHostResolutionForHost(QStringLiteral("fp_host_b")));
        QCOMPARE(prefs->gameOptimizationsOverrideState(QStringLiteral("fp_host_a")), 0);
        QVERIFY(!prefs->gameOptimizationsForHost(QStringLiteral("fp_host_a")));
        QVERIFY(prefs->gameOptimizationsForHost(QStringLiteral("fp_host_b")));
        QVERIFY(prefs->quitAppAfterForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->quitAppAfterForHost(QStringLiteral("fp_host_b")));
        QVERIFY(prefs->playAudioOnHostForHost(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->playAudioOnHostForHost(QStringLiteral("fp_host_b")));

        // Reset back to global default (-1) and HEC_AUTO removes entries cleanly
        prefs->setHostEncoderForHost(QStringLiteral("fp_host_a"), StreamingPreferences::HEC_AUTO);
        prefs->setMatchHostResolutionOverride(QStringLiteral("fp_host_a"), -1);
        prefs->setRevertHostResolutionOverride(QStringLiteral("fp_host_a"), -1);
        prefs->setGameOptimizationsOverride(QStringLiteral("fp_host_a"), -1);
        prefs->setQuitAppAfterOverride(QStringLiteral("fp_host_a"), -1);
        prefs->setPlayAudioOnHostOverride(QStringLiteral("fp_host_a"), -1);
        QCOMPARE(prefs->matchHostResolutionOverrideState(QStringLiteral("fp_host_a")), -1);
        QVERIFY(!prefs->hostEncoderOverrides.contains(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->hostMatchResolutionOverrides.contains(QStringLiteral("fp_host_a")));

        // clearHostOverrides removes all per-PC overrides for deleted host without touching others
        prefs->setHostEncoderForHost(QStringLiteral("fp_host_a"), StreamingPreferences::HEC_SOFTWARE);
        prefs->setHostAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("/dev/dri/renderD128"));
        prefs->setHostCaptureAdapterOverride(QStringLiteral("fp_host_a"), QStringLiteral("/dev/dri/renderD129"));
        prefs->setAdaptiveRefinementForHost(QStringLiteral("fp_host_a"), true);
        prefs->setMatchHostResolutionOverride(QStringLiteral("fp_host_a"), 1);
        prefs->setRevertHostResolutionOverride(QStringLiteral("fp_host_a"), 1);
        prefs->setGameOptimizationsOverride(QStringLiteral("fp_host_a"), 0);
        prefs->setQuitAppAfterOverride(QStringLiteral("fp_host_a"), 1);
        prefs->setPlayAudioOnHostOverride(QStringLiteral("fp_host_a"), 1);

        prefs->setHostEncoderForHost(QStringLiteral("fp_host_b"), StreamingPreferences::HEC_HARDWARE_VULKAN);
        prefs->setAdaptiveRefinementForHost(QStringLiteral("fp_host_b"), true);
        prefs->setMatchHostResolutionOverride(QStringLiteral("fp_host_b"), 1);

        QVERIFY(prefs->clearHostOverrides(QStringLiteral("fp_host_a")));
        QVERIFY(!prefs->clearHostOverrides(QStringLiteral("fp_host_a")));
        prefs->save();
        prefs->reload();

        QCOMPARE(prefs->hostEncoderForHost(QStringLiteral("fp_host_a")), StreamingPreferences::HEC_AUTO);
        QVERIFY(prefs->hostAdapterOverride(QStringLiteral("fp_host_a")).isEmpty());
        QVERIFY(prefs->hostCaptureAdapterOverride(QStringLiteral("fp_host_a")).isEmpty());
        QVERIFY(!prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_a")));
        QCOMPARE(prefs->matchHostResolutionOverrideState(QStringLiteral("fp_host_a")), -1);
        QCOMPARE(prefs->revertHostResolutionOverrideState(QStringLiteral("fp_host_a")), -1);
        QCOMPARE(prefs->gameOptimizationsOverrideState(QStringLiteral("fp_host_a")), -1);
        QCOMPARE(prefs->quitAppAfterOverrideState(QStringLiteral("fp_host_a")), -1);
        QCOMPARE(prefs->playAudioOnHostOverrideState(QStringLiteral("fp_host_a")), -1);

        QCOMPARE(prefs->hostEncoderForHost(QStringLiteral("fp_host_b")), StreamingPreferences::HEC_HARDWARE_VULKAN);
        QVERIFY(prefs->adaptiveRefinementForHost(QStringLiteral("fp_host_b")));
        QCOMPARE(prefs->matchHostResolutionOverrideState(QStringLiteral("fp_host_b")), 1);
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
