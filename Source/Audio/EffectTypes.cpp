// The list of 8track's own effects and instruments. A new one is added here, once.
#include "Effects.h"
#include "Dynamics.h"
#include "EqDeEsser.h"
#include "Drive.h"
#include "ChorusEffect.h"
#include "ReverbEffect.h"
#include "Arpeggiator.h"
#include "AnalogSynth.h"
#include "DrumSampler.h"
#include "KeySampler.h"

namespace
{
    using Kind = EffectPlugin::Kind;

    template <typename T>
    EffectPlugin::Type type(const char* description, Kind kind = Kind::Effect)
    {
        return { T::getPluginName(), T::xmlTypeName, description, kind };
    }
}

std::vector<EffectPlugin::Type> EffectPlugin::getTypes()
{
    return {
        type<GateEffect>("Opens above the threshold, keyed from this track or another track or bus"),
        type<CompressorEffect>("Evens out the level, keyed from any track or bus"),
        type<LimiterEffect>("Stops peaks going over a ceiling: for the master"),
        type<EqEffect>("Five bands of parametric EQ, over a spectrum analyser"),
        type<DeEsserEffect>("Tames sibilance: the S and T sounds of a voice"),
        type<DriveEffect>("Overdrive, distortion, fuzz and tube, into a cabinet or an impulse response"),
        type<DelayEffect>("Echoes, free or in time with the song"),
        type<ChorusEffect>("CE-2, Juno and Dimension-style bucket-brigade chorus"),
        type<ReverbEffect>("Rooms, halls, plates and cathedrals, with shimmer"),
        type<AutoPanEffect>("Moves the sound between left and right"),
        type<FilterEffect>("Low, high, band pass or notch, with an LFO"),
        type<PitchEffect>("Shifts pitch and formants"),
        type<AnalogSynth>("Juno-style poly and SH-101-style mono synth", Kind::Instrument),
        type<DrumSampler>("Sixteen pads of samples, Sitala-style", Kind::Instrument),
        type<KeySampler>("Plays one sound up the keyboard: load it or record it", Kind::Instrument),
        type<ArpeggiatorEffect>("Plays held notes one at a time, in time with the song", Kind::MidiEffect),
    };
}

void EffectPlugin::registerTypes(te::PluginManager& pm)
{
    pm.createBuiltInType<GateEffect>();
    pm.createBuiltInType<CompressorEffect>();
    pm.createBuiltInType<LimiterEffect>();
    pm.createBuiltInType<EqEffect>();
    pm.createBuiltInType<DeEsserEffect>();
    pm.createBuiltInType<DriveEffect>();
    pm.createBuiltInType<DelayEffect>();
    pm.createBuiltInType<ChorusEffect>();
    pm.createBuiltInType<ReverbEffect>();
    pm.createBuiltInType<AutoPanEffect>();
    pm.createBuiltInType<FilterEffect>();
    pm.createBuiltInType<PitchEffect>();
    pm.createBuiltInType<ArpeggiatorEffect>();
    pm.createBuiltInType<AnalogSynth>();
    pm.createBuiltInType<DrumSampler>();
    pm.createBuiltInType<KeySampler>();
}

std::optional<EffectPlugin::Type> EffectPlugin::findType(const juce::String& xmlType)
{
    for (auto& t : getTypes())
        if (t.xmlType == xmlType)
            return t;
    return {};
}

std::vector<EffectPlugin::FileReference> EffectPlugin::findFileReferences(te::Edit& e)
{
    std::vector<FileReference> refs;
    for (auto* p : te::getAllPlugins(e, false))
        if (auto* effect = dynamic_cast<EffectPlugin*>(p))
            effect->getFileReferences(refs);
    return refs;
}
