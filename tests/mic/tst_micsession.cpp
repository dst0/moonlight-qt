// MicSession: negotiation gate, permission flow, capture start/stop, mute hotkey, overlay text
#include "testutil.h"
#include "streaming/audio/micsession.h"

#include <QtTest>

namespace {

struct Harness
{
    bool negotiated = true;
    MicPermission::Status permission = MicPermission::Status::Authorized;
    int requests = 0;
    std::function<void(bool)> pendingRequest;
    std::vector<bool> posted;
    std::vector<std::string> statuses;
    int sourcesCreated = 0;
    bool lastEchoCancellation = false;
    bool sourceStartResult = true;
    bool createNullSource = false;
    FakeCaptureSource* lastSource = nullptr;
    std::atomic<int> sent{0};

    MicSession::Dependencies deps()
    {
        MicSession::Dependencies d;
        d.streamNegotiated = [this] { return negotiated; };
        d.permissionStatus = [this] { return permission; };
        d.requestPermission = [this](std::function<void(bool)> cb) {
            requests++;
            pendingRequest = std::move(cb);
        };
        d.createSource = [this](bool echo) -> std::unique_ptr<MicCaptureSource> {
            sourcesCreated++;
            lastEchoCancellation = echo;
            if (createNullSource) {
                return nullptr;
            }
            auto source = std::make_unique<FakeCaptureSource>(sourceStartResult);
            lastSource = source.get();
            return source;
        };
        d.send = [this](const unsigned char*, int) {
            sent++;
            return 0;
        };
        d.postPermissionResult = [this](bool granted) { posted.push_back(granted); };
        d.showStatus = [this](const std::string& text) { statuses.push_back(text); };
        return d;
    }
};

}

class TestMicSession : public QObject
{
    Q_OBJECT

private slots:
    void notNegotiatedNeverCaptures()
    {
        Harness h;
        h.negotiated = false;
        MicSession session(true, h.deps());
        session.begin();
        QCOMPARE(session.state(), MicSession::State::NotNegotiated);
        QCOMPARE(h.sourcesCreated, 0);
        QCOMPARE(h.requests, 0);
    }

    void authorizedStartsCaptureWithEchoCancellationSetting()
    {
        for (bool echo : {true, false}) {
            Harness h;
            MicSession session(echo, h.deps());
            session.begin();
            QCOMPARE(session.state(), MicSession::State::Running);
            QCOMPARE(h.sourcesCreated, 1);
            QCOMPARE(h.lastEchoCancellation, echo);
            QVERIFY(session.streamer() != nullptr);
            QVERIFY(session.streamer()->isRunning());
            QCOMPARE(h.lastSource->starts.load(), 1);

            session.begin();  // no double start
            QCOMPARE(h.sourcesCreated, 1);

            session.end();
            QCOMPARE(session.state(), MicSession::State::Idle);
            QVERIFY(session.streamer() == nullptr);
        }
    }

    void permissionPromptThenGranted()
    {
        Harness h;
        h.permission = MicPermission::Status::NotDetermined;
        MicSession session(true, h.deps());
        session.begin();
        QCOMPARE(session.state(), MicSession::State::WaitingForPermission);
        QCOMPARE(h.requests, 1);
        QCOMPARE(h.sourcesCreated, 0);

        // Answer arrives on another thread and is posted to the main loop
        h.pendingRequest(true);
        QCOMPARE(h.posted.size(), (size_t)1);
        QCOMPARE(h.posted[0], true);
        session.onPermissionResult(true);
        QCOMPARE(session.state(), MicSession::State::Running);
        QCOMPARE(h.sourcesCreated, 1);

        // Duplicate or stale answers are ignored
        session.onPermissionResult(false);
        QCOMPARE(session.state(), MicSession::State::Running);
    }

    void permissionPromptThenRefused()
    {
        Harness h;
        h.permission = MicPermission::Status::NotDetermined;
        MicSession session(true, h.deps());
        session.begin();
        h.pendingRequest(false);
        session.onPermissionResult(false);
        QCOMPARE(session.state(), MicSession::State::PermissionDenied);
        QCOMPARE(h.sourcesCreated, 0);
    }

    void lateAnswerAfterEndIsDropped()
    {
        Harness h;
        h.permission = MicPermission::Status::NotDetermined;
        {
            MicSession session(true, h.deps());
            session.begin();
            session.end();
            session.onPermissionResult(true);
            QCOMPARE(h.sourcesCreated, 0);
        }
        h.pendingRequest(true);  // session gone: nothing is posted
        QCOMPARE(h.posted.size(), (size_t)0);
    }

    void deniedAndUnsupported()
    {
        Harness denied;
        denied.permission = MicPermission::Status::Denied;
        MicSession s1(true, denied.deps());
        s1.begin();
        QCOMPARE(s1.state(), MicSession::State::PermissionDenied);
        QCOMPARE(denied.sourcesCreated, 0);
        QCOMPARE(denied.statuses.size(), (size_t)1);

        Harness unsupported;
        unsupported.permission = MicPermission::Status::Unsupported;
        MicSession s2(true, unsupported.deps());
        s2.begin();
        QCOMPARE(s2.state(), MicSession::State::Unsupported);

        Harness noBackend;
        noBackend.createNullSource = true;
        MicSession s3(true, noBackend.deps());
        s3.begin();
        QCOMPARE(s3.state(), MicSession::State::Unsupported);

        Harness failing;
        failing.sourceStartResult = false;
        MicSession s4(true, failing.deps());
        s4.begin();
        QCOMPARE(s4.state(), MicSession::State::Failed);
        QVERIFY(s4.streamer() == nullptr);
        QCOMPARE(failing.statuses.size(), (size_t)1);
    }

    void muteToggleControlsStreamerAndOverlay()
    {
        Harness h;
        MicSession session(true, h.deps());
        session.begin();
        QVERIFY(!session.isMuted());

        QVERIFY(session.toggleMute());
        QVERIFY(session.streamer()->isMuted());
        QCOMPARE(h.statuses.size(), (size_t)1);
        QVERIFY(QString::fromStdString(h.statuses.back()).contains("muted"));
        QVERIFY(QString::fromStdString(h.statuses.back()).contains(MicSession::muteHotkeyDescription()));

        SineGenerator sine;
        auto muted = sine.next(960 * 5);
        h.lastSource->deliver(muted.data(), (int)muted.size());
        QTest::qWait(50);
        QCOMPARE(h.sent.load(), 0);

        QVERIFY(!session.toggleMute());
        QVERIFY(!session.streamer()->isMuted());
        QVERIFY(h.statuses.back().empty());  // overlay hidden
        auto live = sine.next(960 * 2);
        h.lastSource->deliver(live.data(), (int)live.size());
        QTRY_COMPARE(session.streamer()->framesSent(), (uint64_t)2);
    }

    void muteBeforeCaptureStartsCarriesOver()
    {
        Harness h;
        h.permission = MicPermission::Status::NotDetermined;
        MicSession session(true, h.deps());
        session.begin();
        session.toggleMute();
        session.onPermissionResult(true);
        QVERIFY(session.streamer()->isMuted());
    }

    void destructorStopsCapture()
    {
        Harness h;
        FakeCaptureSource* source = nullptr;
        {
            MicSession session(true, h.deps());
            session.begin();
            source = h.lastSource;
            QCOMPARE(source->stops.load(), 0);
        }
        // The source is destroyed with the session; reaching here without a hang or crash
        // means the encoder thread was joined.
        QVERIFY(source != nullptr);
    }
};

MIC_REGISTER_TEST(TestMicSession);
#include "tst_micsession.moc"
