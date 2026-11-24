// secure-desktop-stream: macOS microphone capture with AVAudioEngine (DESIGN.md section 7.4).
//
// SDL audio capture is not used because the SDL audio renderer owns SDL's audio subsystem
// initialization. AVAudioEngine gives us Apple's voice processing (echo cancellation, so the
// host's audio played on this Mac is not sent back to the host).
//
// This file is compiled without ARC (like the rest of moonlight-qt's Objective-C++).

#include "miccapture.h"

#include <SDL.h>

#include "miccapture_format_mac.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace {

class AVAudioEngineMicSource : public MicCaptureSource
{
public:
    explicit AVAudioEngineMicSource(bool echoCancellation)
        : m_EchoCancellation(echoCancellation),
          m_Engine(nil),
          m_Converter(nil),
          m_OutputFormat(nil),
          m_TapInstalled(false),
          m_Running(false)
    {
    }

    ~AVAudioEngineMicSource() override
    {
        stop();
    }

    bool start(SampleCallback callback) override
    { @autoreleasepool {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_Running) {
            return true;
        }

        m_Callback = std::move(callback);
        m_Engine = [[AVAudioEngine alloc] init];
        AVAudioInputNode* input = m_Engine.inputNode;

        const AVAudioChannelCount deviceChannels = [input inputFormatForBus:0].channelCount;
        if (m_EchoCancellation) {
            NSError* error = nil;
            if (![input setVoiceProcessingEnabled:YES error:&error]) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Microphone: voice processing unavailable: %s",
                            error.localizedDescription.UTF8String);
            }
            else if (@available(macOS 14.0, *)) {
                // Keep the stream audio (and everything else) at full volume while talking
                AVAudioVoiceProcessingOtherAudioDuckingConfiguration ducking;
                ducking.enableAdvancedDucking = NO;
                ducking.duckingLevel = AVAudioVoiceProcessingOtherAudioDuckingLevelMin;
                input.voiceProcessingOtherAudioDuckingConfiguration = ducking;
            }
        }

        // Query the format after enabling voice processing, which can change it
        AVAudioFormat* inputFormat = [input outputFormatForBus:0];
        if (inputFormat.sampleRate <= 0 || inputFormat.channelCount == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Microphone: no input device available");
            releaseObjects();
            return false;
        }

        m_OutputFormat = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                          sampleRate:48000
                                                            channels:1
                                                         interleaved:NO];
        m_Converter = newMacMicConverter(inputFormat, m_OutputFormat,
                                         input.voiceProcessingEnabled, deviceChannels);
        if (m_Converter == nil) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Microphone: no converter from %.0f Hz / %u ch",
                        inputFormat.sampleRate, (unsigned)inputFormat.channelCount);
            releaseObjects();
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Microphone: capturing %.0f Hz / %u ch, voice processing %s",
                    inputFormat.sampleRate, (unsigned)inputFormat.channelCount,
                    input.voiceProcessingEnabled ? "on" : "off");

        // ~20 ms taps at the device rate; CoreAudio may choose a different size
        AVAudioFrameCount bufferSize = (AVAudioFrameCount)(inputFormat.sampleRate / 50);
        AVAudioNodeTapBlock tapBlock = ^(AVAudioPCMBuffer* buffer, AVAudioTime* when) {
            (void)when;
            convertAndDeliver(buffer);
        };
        if (@available(macOS 27.0, *)) {
            NSError* tapError = nil;
            if (![input installTapOnBus:0 bufferSize:bufferSize format:inputFormat error:&tapError block:tapBlock]) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Microphone: couldn't install the input tap: %s",
                            tapError.localizedDescription.UTF8String);
                releaseObjects();
                return false;
            }
        }
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [input installTapOnBus:0 bufferSize:bufferSize format:inputFormat block:tapBlock];
#pragma clang diagnostic pop
        }
        m_TapInstalled = true;

        [m_Engine prepare];
        NSError* error = nil;
        if (![m_Engine startAndReturnError:&error]) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Microphone: AVAudioEngine failed to start: %s",
                        error.localizedDescription.UTF8String);
            releaseObjects();
            return false;
        }

        m_Running = true;
        return true;
    }}

    void stop() override
    { @autoreleasepool {
        std::lock_guard<std::mutex> lock(m_Lock);
        releaseObjects();
    }}

    const char* name() const override
    {
        return "AVAudioEngine";
    }

private:
    // Must be called with m_Lock held
    void releaseObjects()
    {
        m_Running = false;
        if (m_Engine != nil) {
            if (m_TapInstalled) {
                // Removing the tap waits for an in-flight tap block to finish
                [m_Engine.inputNode removeTapOnBus:0];
                m_TapInstalled = false;
            }
            [m_Engine stop];
            [m_Engine release];
            m_Engine = nil;
        }
        if (m_Converter != nil) {
            [m_Converter release];
            m_Converter = nil;
        }
        if (m_OutputFormat != nil) {
            [m_OutputFormat release];
            m_OutputFormat = nil;
        }
        m_Callback = nullptr;
    }

    // Runs on AVAudioEngine's tap thread (not the real-time IO thread)
    void convertAndDeliver(AVAudioPCMBuffer* buffer)
    {
        if (!m_Running || buffer == nil || buffer.frameLength == 0 || buffer.format.sampleRate <= 0) {
            return;
        }

        double ratio = m_OutputFormat.sampleRate / buffer.format.sampleRate;
        AVAudioFrameCount capacity = (AVAudioFrameCount)std::ceil(buffer.frameLength * ratio) + 64;
        AVAudioPCMBuffer* converted = [[AVAudioPCMBuffer alloc] initWithPCMFormat:m_OutputFormat frameCapacity:capacity];

        __block BOOL consumed = NO;
        NSError* error = nil;
        AVAudioConverterOutputStatus status =
            [m_Converter convertToBuffer:converted
                                   error:&error
                      withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount packetCount, AVAudioConverterInputStatus* inputStatus) {
                          (void)packetCount;
                          if (consumed) {
                              *inputStatus = AVAudioConverterInputStatus_NoDataNow;
                              return nil;
                          }
                          consumed = YES;
                          *inputStatus = AVAudioConverterInputStatus_HaveData;
                          return buffer;
                      }];

        if (status != AVAudioConverterOutputStatus_Error && converted.frameLength > 0 && m_Callback) {
            m_Callback(converted.floatChannelData[0], (int)converted.frameLength);
        }
        [converted release];
    }

    bool m_EchoCancellation;
    std::mutex m_Lock;
    SampleCallback m_Callback;
    AVAudioEngine* m_Engine;
    AVAudioConverter* m_Converter;
    AVAudioFormat* m_OutputFormat;
    bool m_TapInstalled;
    std::atomic<bool> m_Running;
};

}

std::unique_ptr<MicCaptureSource> createPlatformMicCaptureSource(bool echoCancellation)
{
    return std::make_unique<AVAudioEngineMicSource>(echoCancellation);
}

namespace MicPermission
{
    Status status()
    {
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized:
            return Status::Authorized;
        case AVAuthorizationStatusNotDetermined:
            return Status::NotDetermined;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
        default:
            return Status::Denied;
        }
    }

    void request(std::function<void(bool granted)> callback)
    {
        if (status() != Status::NotDetermined) {
            if (callback) {
                callback(status() == Status::Authorized);
            }
            return;
        }

        // The block copies the std::function; it is released with the block
        auto* heapCallback = new std::function<void(bool)>(std::move(callback));
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            if (*heapCallback) {
                (*heapCallback)(granted == YES);
            }
            delete heapCallback;
        }];
    }
}
