#include "tst_miccapture_mac.h"
#include "testutil.h"
#include "streaming/audio/miccapture_format_mac.h"

#include <QtTest>
#include <cmath>

namespace {

constexpr AVAudioFrameCount TestFrames = 2048;

AVAudioFormat* floatFormat(unsigned channels, AudioChannelLayoutTag tag=0)
{
    if (tag) {
        auto layout = [[[AVAudioChannelLayout alloc] initWithLayoutTag:tag] autorelease];
        return [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000
                                                            channelLayout:layout] autorelease];
    }
    if (channels > 2) {
        auto layout = [[[AVAudioChannelLayout alloc]
            initWithLayoutTag:kAudioChannelLayoutTag_DiscreteInOrder | channels] autorelease];
        return [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000
                                                            channelLayout:layout] autorelease];
    }
    return [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:channels] autorelease];
}

std::vector<float> convert(AVAudioConverter* converter, unsigned signalChannel)
{
    if (!converter) {
        return {};
    }
    auto input = [[[AVAudioPCMBuffer alloc] initWithPCMFormat:converter.inputFormat
                                               frameCapacity:TestFrames] autorelease];
    auto output = [[[AVAudioPCMBuffer alloc] initWithPCMFormat:converter.outputFormat
                                                frameCapacity:TestFrames + 64] autorelease];
    if (!input || !output || signalChannel >= input.format.channelCount) {
        return {};
    }
    input.frameLength = TestFrames;
    for (unsigned ch = 0; ch < input.format.channelCount; ++ch) {
        auto channel = input.floatChannelData[ch];
        for (unsigned frame = 0; frame < TestFrames; ++frame) {
            channel[frame] = ch == signalChannel ? 0.25f * std::sin(2 * M_PI * 997 * frame / 48000) : 0.0f;
        }
    }
    __block BOOL consumed = NO;
    const auto status = [converter convertToBuffer:output error:nil
        withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount, AVAudioConverterInputStatus* inputStatus) {
            if (consumed) {
                *inputStatus = AVAudioConverterInputStatus_NoDataNow;
                return nil;
            }
            consumed = YES;
            *inputStatus = AVAudioConverterInputStatus_HaveData;
            return input;
        }];
    if (status == AVAudioConverterOutputStatus_Error || !output.frameLength) {
        return {};
    }
    return {output.floatChannelData[0], output.floatChannelData[0] + output.frameLength};
}

} // namespace

void TestMacMicConverter::convertsDiscreteOrExpandedInputToMono_data()
{
    QTest::addColumn<int>("channels");
    QTest::addColumn<bool>("processing");
    QTest::addColumn<int>("deviceChannels");
    QTest::addColumn<int>("signalChannel");
    QTest::addColumn<unsigned>("layoutTag");
    QTest::newRow("mono-device-expanded-by-processing") << 9 << true << 1 << 0 << 0u;
    QTest::newRow("mono-device-processing-auxiliary-excluded") << 9 << true << 1 << 8 << 0u;
    QTest::newRow("mac-three-channel-device-expanded-by-processing") << 9 << true << 3 << 0 << 0u;
    QTest::newRow("mac-three-channel-device-processing-auxiliary-excluded") << 9 << true << 3 << 8 << 0u;
    QTest::newRow("mac-three-channel-discrete-without-processing") << 3 << false << 3 << 0 << 0u;
    QTest::newRow("mac-three-channel-discrete-other-channel-excluded") << 3 << false << 3 << 2 << 0u;
    QTest::newRow("labeled-format-expanded-by-processing")
        << 3 << true << 1 << 0 << unsigned(kAudioChannelLayoutTag_MPEG_3_0_A);
    QTest::newRow("labeled-format-processing-auxiliary-excluded")
        << 3 << true << 1 << 2 << unsigned(kAudioChannelLayoutTag_MPEG_3_0_A);
}

void TestMacMicConverter::convertsDiscreteOrExpandedInputToMono()
{
    QFETCH(int, channels);
    QFETCH(bool, processing);
    QFETCH(int, deviceChannels);
    QFETCH(int, signalChannel);
    QFETCH(unsigned, layoutTag);
    @autoreleasepool {
        auto converter = [newMacMicConverter(floatFormat(channels, layoutTag), floatFormat(1),
                                             processing, deviceChannels) autorelease];
        const auto samples = convert(converter, signalChannel);
        QVERIFY(samples.size() >= 960);
        if (signalChannel == 0) {
            QVERIFY2(rms(samples) > 0.15 && rms(samples) < 0.20,
                     qPrintable(QString("expected the 997 Hz input; output RMS was %1").arg(rms(samples))));
        } else {
            QVERIFY(rms(samples) < 0.000001);
        }
    }
}

void TestMacMicConverter::preservesOrdinaryConversion_data()
{
    QTest::addColumn<int>("channels");
    QTest::addColumn<bool>("processing");
    QTest::addColumn<int>("deviceChannels");
    QTest::addColumn<unsigned>("layoutTag");
    QTest::newRow("standard-mono") << 1 << false << 1 << 0u;
    QTest::newRow("processed-mono") << 1 << true << 1 << 0u;
    QTest::newRow("standard-stereo") << 2 << false << 2 << 0u;
    QTest::newRow("labeled-three-channel-without-processing")
        << 3 << false << 1 << unsigned(kAudioChannelLayoutTag_MPEG_3_0_A);
    QTest::newRow("labeled-three-channel-with-no-expansion")
        << 3 << true << 3 << unsigned(kAudioChannelLayoutTag_MPEG_3_0_A);
    QTest::newRow("labeled-three-channel-with-unknown-device-format")
        << 3 << true << 0 << unsigned(kAudioChannelLayoutTag_MPEG_3_0_A);
}

void TestMacMicConverter::preservesOrdinaryConversion()
{
    QFETCH(int, channels);
    QFETCH(bool, processing);
    QFETCH(int, deviceChannels);
    QFETCH(unsigned, layoutTag);
    @autoreleasepool {
        auto input = floatFormat(channels, layoutTag);
        auto output = floatFormat(1);
        auto converter = [newMacMicConverter(input, output, processing, deviceChannels) autorelease];
        auto baseline = [[[AVAudioConverter alloc] initFromFormat:input toFormat:output] autorelease];
        const auto expected = convert(baseline, 0);
        QVERIFY(expected.size() >= 960);
        QCOMPARE(convert(converter, 0), expected);
    }
}

MIC_REGISTER_TEST(TestMacMicConverter);
