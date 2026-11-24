// Settings plumbing: defaults, persistence and the Q_PROPERTY surface used by SettingsView.qml
#include "testutil.h"
#include "settings/streamingpreferences.h"

#include <QSettings>
#include <QtTest>

class TestMicPreferences : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QSettings().clear();
    }

    void cleanupTestCase()
    {
        QSettings().clear();
    }

    void defaultsAreOffAndEchoCancellationOn()
    {
        QSettings().clear();
        StreamingPreferences* prefs = StreamingPreferences::get();
        prefs->reload();
        QCOMPARE(prefs->micPassthrough, false);
        QCOMPARE(prefs->micEchoCancellation, true);
    }

    void valuesPersistAcrossReload()
    {
        StreamingPreferences* prefs = StreamingPreferences::get();
        prefs->micPassthrough = true;
        prefs->micEchoCancellation = false;
        prefs->save();

        QSettings settings;
        QCOMPARE(settings.value("micpassthrough").toBool(), true);
        QCOMPARE(settings.value("micechocancel").toBool(), false);

        prefs->micPassthrough = false;
        prefs->micEchoCancellation = true;
        prefs->reload();
        QCOMPARE(prefs->micPassthrough, true);
        QCOMPARE(prefs->micEchoCancellation, false);
    }

    void qmlPropertiesAreExposed()
    {
        StreamingPreferences* prefs = StreamingPreferences::get();
        QVERIFY(prefs->setProperty("micPassthrough", false));
        QCOMPARE(prefs->property("micPassthrough").toBool(), false);
        QVERIFY(prefs->setProperty("micEchoCancellation", true));
        QCOMPARE(prefs->property("micEchoCancellation").toBool(), true);

        // The test build links the platform stub: no capture backend, so the UI hides the options
        QCOMPARE(prefs->property("micCaptureSupported").toBool(), false);
        QVERIFY(prefs->metaObject()->indexOfMethod("requestMicrophonePermission()") >= 0);
        QVERIFY(QMetaObject::invokeMethod(prefs, "requestMicrophonePermission"));
    }
};

MIC_REGISTER_TEST(TestMicPreferences);
#include "tst_micpreferences.moc"
