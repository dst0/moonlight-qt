// MicStreamer lifecycle with a fake capture source: start/stop, encoder thread, mute, backlog bound
#include "testutil.h"
#include "streaming/audio/micstreamer.h"

#include <QtTest>

#include <chrono>
#include <condition_variable>
#include <thread>

namespace {

struct RecordingSender
{
    std::mutex lock;
    std::condition_variable cond;
    std::vector<std::vector<unsigned char>> frames;
    int result = 0;

    MicStreamer::SendFunction fn()
    {
        return [this](const unsigned char* data, int length) {
            std::lock_guard<std::mutex> guard(lock);
            frames.emplace_back(data, data + length);
            cond.notify_all();
            return result;
        };
    }

    bool waitFor(size_t count, int timeoutMs = 3000)
    {
        std::unique_lock<std::mutex> guard(lock);
        return cond.wait_for(guard, std::chrono::milliseconds(timeoutMs), [&] { return frames.size() >= count; });
    }

    size_t size()
    {
        std::lock_guard<std::mutex> guard(lock);
        return frames.size();
    }
};

}

class TestMicStreamer : public QObject
{
    Q_OBJECT

private slots:
    void startStreamsFramesAndStopJoins()
    {
        auto source = std::make_unique<FakeCaptureSource>();
        FakeCaptureSource* fake = source.get();
        RecordingSender sender;
        MicStreamer streamer(std::move(source), sender.fn());

        QVERIFY(!streamer.isRunning());
        QVERIFY(streamer.start());
        QVERIFY(streamer.isRunning());
        QCOMPARE(fake->starts.load(), 1);
        QVERIFY(streamer.start());  // already running
        QCOMPARE(fake->starts.load(), 1);

        // 1 s of audio from a separate "tap" thread in 10 ms chunks, paced at about 2x real time
        // (an unpaced burst would exceed the 200 ms backlog bound, see the backlog test)
        std::thread tap([fake] {
            SineGenerator sine;
            for (int i = 0; i < 100; i++) {
                auto chunk = sine.next(480);
                fake->deliver(chunk.data(), (int)chunk.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        tap.join();
        QVERIFY(sender.waitFor(50));
        QTRY_COMPARE(streamer.framesSent(), (uint64_t)50);

        streamer.stop();
        QVERIFY(!streamer.isRunning());
        QCOMPARE(fake->stops.load(), 1);

        // After stop nothing is delivered or sent
        SineGenerator sine;
        auto chunk = sine.next(960 * 5);
        QVERIFY(!fake->deliver(chunk.data(), (int)chunk.size()));
        streamer.onCapturedSamples(chunk.data(), (int)chunk.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QCOMPARE(sender.size(), (size_t)50);

        // Decodable 20 ms mono frames
        for (const auto& frame : sender.frames) {
            QCOMPARE((int)decodeOpus(frame.data(), (int)frame.size()).size(), 960);
        }

        streamer.stop();  // idempotent
        QCOMPARE(fake->stops.load(), 1);
    }

    void restartAfterStop()
    {
        auto source = std::make_unique<FakeCaptureSource>();
        FakeCaptureSource* fake = source.get();
        RecordingSender sender;
        MicStreamer streamer(std::move(source), sender.fn());
        QVERIFY(streamer.start());
        streamer.stop();
        QVERIFY(streamer.start());
        SineGenerator sine;
        auto chunk = sine.next(960 * 2);
        QVERIFY(fake->deliver(chunk.data(), (int)chunk.size()));
        QVERIFY(sender.waitFor(2));
    }

    void sourceFailureFailsStartCleanly()
    {
        auto source = std::make_unique<FakeCaptureSource>(false);
        FakeCaptureSource* fake = source.get();
        RecordingSender sender;
        MicStreamer streamer(std::move(source), sender.fn());
        QVERIFY(!streamer.start());
        QVERIFY(!streamer.isRunning());
        QCOMPARE(fake->starts.load(), 1);
        QCOMPARE(fake->stops.load(), 1);  // stop() is safe after a failed start

        MicStreamer noSource(nullptr, sender.fn());
        QVERIFY(!noSource.start());
    }

    void muteDiscardsAudioAndUnmuteResumes()
    {
        auto source = std::make_unique<FakeCaptureSource>();
        FakeCaptureSource* fake = source.get();
        RecordingSender sender;
        MicStreamer streamer(std::move(source), sender.fn());
        QVERIFY(streamer.start());
        SineGenerator sine;

        streamer.setMuted(true);
        QVERIFY(streamer.isMuted());
        auto muted = sine.next(960 * 10);
        fake->deliver(muted.data(), (int)muted.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        QCOMPARE(sender.size(), (size_t)0);

        streamer.setMuted(false);
        auto live = sine.next(960 * 3);
        fake->deliver(live.data(), (int)live.size());
        QVERIFY(sender.waitFor(3));
        QTRY_COMPARE(streamer.framesSent(), (uint64_t)3);
    }

    void failedSendsAreCountedNotFatal()
    {
        auto source = std::make_unique<FakeCaptureSource>();
        FakeCaptureSource* fake = source.get();
        RecordingSender sender;
        sender.result = -5501;  // LI_ERR_UNSUPPORTED: connection gone
        MicStreamer streamer(std::move(source), sender.fn());
        QVERIFY(streamer.start());
        SineGenerator sine;
        auto chunk = sine.next(960 * 4);
        fake->deliver(chunk.data(), (int)chunk.size());
        QVERIFY(sender.waitFor(4));
        QCOMPARE(streamer.framesSent(), (uint64_t)0);
        QTRY_COMPARE(streamer.framesFailed(), (uint64_t)4);
        QVERIFY(streamer.isRunning());
    }

    void backlogIsBoundedWhenTheEncoderStalls()
    {
        auto source = std::make_unique<FakeCaptureSource>();
        FakeCaptureSource* fake = source.get();
        std::mutex gate;
        gate.lock();  // the first send blocks until released
        std::atomic<int> sends{0};
        MicStreamer streamer(std::move(source), [&](const unsigned char*, int) {
            if (sends++ == 0) {
                std::lock_guard<std::mutex> wait(gate);
            }
            return 0;
        });
        QVERIFY(streamer.start());
        SineGenerator sine;
        auto first = sine.next(960);
        fake->deliver(first.data(), (int)first.size());
        while (sends.load() == 0) {
            std::this_thread::yield();
        }

        // 1 s arrives while the encoder thread is stuck: only 200 ms is kept
        auto burst = sine.next(48000);
        fake->deliver(burst.data(), (int)burst.size());
        QCOMPARE(streamer.samplesDropped(), (uint64_t)(48000 - MicStreamer::kMaxQueuedSamples));
        gate.unlock();

        for (int i = 0; i < 300 && sends.load() < 1 + MicStreamer::kMaxQueuedSamples / 960; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        QTRY_COMPARE(sends.load(), 1 + MicStreamer::kMaxQueuedSamples / 960);
        streamer.stop();
    }
};

MIC_REGISTER_TEST(TestMicStreamer);
#include "tst_micstreamer.moc"
