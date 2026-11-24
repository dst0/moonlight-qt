#pragma once

#import <AVFoundation/AVFoundation.h>

// Returns an owned converter. This helper never opens an input device.
inline AVAudioConverter* newMacMicConverter(AVAudioFormat* inputFormat,
                                          AVAudioFormat* outputFormat,
                                          bool voiceProcessingEnabled,
                                          AVAudioChannelCount deviceChannels)
{
    auto converter = [[AVAudioConverter alloc] initFromFormat:inputFormat toFormat:outputFormat];
    const auto inputChannels = inputFormat.channelCount;
    if (inputChannels > 1 && outputFormat.channelCount == 1) {
        const auto tag = inputFormat.channelLayout.layoutTag;
        const bool discrete = (tag & 0xffff0000u) == kAudioChannelLayoutTag_DiscreteInOrder;
        const bool processingExpandedInput = voiceProcessingEnabled && deviceChannels > 0 &&
                                             inputChannels > deviceChannels;
        if (discrete || processingExpandedInput) {
            // Implicit downmix can produce silence for discrete input. Voice processing
            // may also add auxiliary channels. Select the first capture channel without
            // mixing in unknown inputs or reference audio.
            converter.channelMap = @[@0];
        }
    }
    return converter;
}
