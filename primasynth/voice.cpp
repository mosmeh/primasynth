#include "voice.h"

namespace primasynth {

StereoValue calculatePannedVolume(double pan) {
    if (pan <= -500.0) {
        return {1.0, 0.0};
    } else if (pan >= 500.0) {
        return {0.0, 1.0};
    } else {
        static constexpr double f = 3.141592653589793 / 2000.0;
        return {std::sin(f * (-pan + 500.0)), std::sin(f * (pan + 500.0))};
    }
}

Voice::Voice(std::size_t noteID, double outputRate, std::shared_ptr<const Sample> sample,
    const GeneratorSet& generators, const ModulatorParameterSet& modparams, std::uint8_t key, std::uint8_t velocity) :
    noteID_(noteID),
    generators_(generators),
    actualKey_(key),
    modulations_(),
    released_(false),
    phase_(sample->start),
    volume_({1.0, 1.0}),
    volEnv_(outputRate),
    modEnv_(outputRate),
    vibLFO_(outputRate),
    modLFO_(outputRate) {

    const std::int16_t overriddenKey = generators.getOrDefault(SFGenerator::keynum);
    key_ = overriddenKey > 0 ? overriddenKey : key;
    const std::int16_t overriddenVelocity = generators.getOrDefault(SFGenerator::velocity);
    velocity_ = overriddenVelocity > 0 ? overriddenVelocity : velocity;

    sample_.buffer = sample->soundFont->getSampleBuffer();
    const std::int16_t overriddenSampleKey = generators.getOrDefault(SFGenerator::overridingRootKey);
    sample_.pitch = (overriddenSampleKey > 0 ? overriddenSampleKey : sample->key) - 0.01 * sample->correction;
    sample_.mode = static_cast<SampleMode>(generators.getOrDefault(SFGenerator::sampleModes));
    sample_.start = sample->start
        + 32768 * generators.getOrDefault(SFGenerator::startAddrsCoarseOffset)
        + generators.getOrDefault(SFGenerator::startAddrsOffset);
    sample_.end = sample->end
        + 32768 * generators.getOrDefault(SFGenerator::endAddrsCoarseOffset)
        + generators.getOrDefault(SFGenerator::endAddrsOffset);
    sample_.startLoop = sample->startLoop
        + 32768 * generators.getOrDefault(SFGenerator::startloopAddrsCoarseOffset)
        + generators.getOrDefault(SFGenerator::startloopAddrsOffset);
    sample_.endLoop = sample->endLoop
        + 32768 * generators.getOrDefault(SFGenerator::endloopAddrsCoarseOffset)
        + generators.getOrDefault(SFGenerator::endloopAddrsOffset);

    deltaPhaseFactor_ = 1.0 / keyToHeltz(sample_.pitch) * sample->sampleRate / outputRate;

    for (const auto& mp : modparams.getParameters()) {
        modulators_.emplace_back(mp);
    }

    updateSFController(SFGeneralController::noteOnVelocity, velocity);
    updateSFController(SFGeneralController::noteOnKeyNumber, key);
    updateSFController(SFGeneralController::pitchWheelSensitivity, 2);

    static const auto INIT_GENERATORS = {
        SFGenerator::pan,
        SFGenerator::delayModLFO,
        SFGenerator::freqModLFO,
        SFGenerator::delayVibLFO,
        SFGenerator::freqVibLFO,
        SFGenerator::delayModEnv,
        SFGenerator::attackModEnv,
        SFGenerator::holdModEnv,
        SFGenerator::decayModEnv,
        SFGenerator::sustainModEnv,
        SFGenerator::releaseModEnv,
        SFGenerator::delayVolEnv,
        SFGenerator::attackVolEnv,
        SFGenerator::holdVolEnv,
        SFGenerator::decayVolEnv,
        SFGenerator::sustainVolEnv,
        SFGenerator::releaseVolEnv,
        SFGenerator::coarseTune
    };
    for (const auto& generator : INIT_GENERATORS) {
        updateModulatedParams(generator);
    }
}

std::size_t Voice::getNoteID() const {
    return noteID_;
}

std::uint8_t Voice::getActualKey() const {
    return actualKey_;
}

std::int16_t Voice::getExclusiveClass() const {
    return static_cast<std::int16_t>(getModulatedGenerator(SFGenerator::exclusiveClass));
}

void Voice::update() {
    if (volEnv_.isFinished()) {
        return;
    }

    phase_ += deltaPhase_;

    switch (sample_.mode) {
    case SampleMode::UnLooped:
    case SampleMode::UnUsed:
        if (phase_.getIntegerPart() > sample_.end - 1) {
            volEnv_.finish();
            return;
        }
        break;
    case SampleMode::Looped:
        if (phase_.getIntegerPart() > sample_.endLoop - 1) {
            if (released_) {
                volEnv_.finish();
                return;
            } else {
                phase_ -= FixedPoint(sample_.endLoop - sample_.startLoop);
            }
        }
        break;
    case SampleMode::LoopedWithRemainder:
        if (released_) {
            if (phase_.getIntegerPart() > sample_.end - 1) {
                volEnv_.finish();
                return;
            }
        } else if (phase_.getIntegerPart() > sample_.endLoop - 1) {
            phase_ -= FixedPoint(sample_.endLoop - sample_.startLoop);
        }
        break;
    }

    vibLFO_.update(deltaPhase_);
    modLFO_.update(deltaPhase_);
    volEnv_.update(deltaPhase_);
    modEnv_.update(deltaPhase_);

    deltaPhase_ = FixedPoint(deltaPhaseFactor_ * keyToHeltz(voicePitch_
        + getModulatedGenerator(SFGenerator::modEnvToPitch) * modEnv_.getValue()
        + getModulatedGenerator(SFGenerator::vibLfoToPitch) * vibLFO_.getValue()
        + getModulatedGenerator(SFGenerator::modLfoToPitch) * modLFO_.getValue()));
}

void Voice::updateSFController(SFGeneralController controller, std::int16_t value) {
    for (auto& mod : modulators_) {
        if (mod.isSourceSFController(controller)) {
            mod.updateSFController(controller, value);
            updateModulatedParams(mod.getDestination());
        }
    }
}

void Voice::updateMIDIController(std::uint8_t controller, std::uint8_t value) {
    for (auto& mod : modulators_) {
        if (mod.isSourceMIDIController(controller)) {
            mod.updateMIDIController(controller, value);
            updateModulatedParams(mod.getDestination());
        }
    }
}

void Voice::overrideGenerator(SFGenerator generator, std::int16_t value) {
    generators_.set(generator, value);
}

StereoValue Voice::render() const {
    if (volEnv_.isFinished()) {
        return {0.0, 0.0};
    } else {
        const std::uint32_t i = phase_.getIntegerPart();
        const double r = phase_.getFractionalPart();
        const double interpolated = (1.0 - r) * sample_.buffer->at(i) + r * sample_.buffer->at(i + 1);
        return volEnv_.getValue()
            * centibelToRatio(getModulatedGenerator(SFGenerator::modLfoToVolume) * modLFO_.getValue())
            * volume_ * (interpolated / INT16_MAX);
    }
}

bool Voice::isSounding() const {
    return !volEnv_.isFinished();
}

void Voice::release() {
    released_ = true;
    volEnv_.release();
    modEnv_.release();
}

double Voice::getModulatedGenerator(SFGenerator type) const {
    return generators_.getOrDefault(type) + modulations_.at(static_cast<std::size_t>(type));
}

void Voice::updateModulatedParams(SFGenerator destination) {
    double& modulation = modulations_.at(static_cast<std::size_t>(destination));
    modulation = 0.0;
    for (auto& mod : modulators_) {
        if (mod.getDestination() == destination) {
            modulation += mod.getValue();
        }
    }
    switch (destination) {
    case SFGenerator::pan:
    case SFGenerator::initialAttenuation: {
        const auto atten = 0.4 * generators_.getOrDefault(SFGenerator::initialAttenuation)
            + modulations_.at(static_cast<std::size_t>(SFGenerator::initialAttenuation));
        volume_ = centibelToRatio(atten) * calculatePannedVolume(getModulatedGenerator(SFGenerator::pan));
        break;
    }
    case SFGenerator::delayModLFO:
        modLFO_.setDelay(getModulatedGenerator(SFGenerator::delayModLFO));
        break;
    case SFGenerator::freqModLFO:
        modLFO_.setFrequency(getModulatedGenerator(SFGenerator::freqModLFO));
        break;
    case SFGenerator::delayVibLFO:
        vibLFO_.setDelay(getModulatedGenerator(SFGenerator::delayVibLFO));
        break;
    case SFGenerator::freqVibLFO:
        vibLFO_.setFrequency(getModulatedGenerator(SFGenerator::freqVibLFO));
        break;
    case SFGenerator::delayModEnv:
        modEnv_.setParameter(Envelope::Section::Delay, getModulatedGenerator(SFGenerator::delayModEnv));
        break;
    case SFGenerator::attackModEnv:
        modEnv_.setParameter(Envelope::Section::Attack, getModulatedGenerator(SFGenerator::attackModEnv));
        break;
    case SFGenerator::holdModEnv:
    case SFGenerator::keynumToModEnvHold:
        modEnv_.setParameter(Envelope::Section::Hold,
            getModulatedGenerator(SFGenerator::holdModEnv) + getModulatedGenerator(SFGenerator::keynumToModEnvHold) * (60 - key_));
        break;
    case SFGenerator::decayModEnv:
    case SFGenerator::keynumToModEnvDecay:
        modEnv_.setParameter(Envelope::Section::Decay,
            getModulatedGenerator(SFGenerator::decayModEnv) + getModulatedGenerator(SFGenerator::keynumToModEnvDecay) * (60 - key_));
        break;
    case SFGenerator::sustainModEnv:
        modEnv_.setParameter(Envelope::Section::Sustain, getModulatedGenerator(SFGenerator::sustainModEnv));
        break;
    case SFGenerator::releaseModEnv:
        modEnv_.setParameter(Envelope::Section::Release, getModulatedGenerator(SFGenerator::releaseModEnv));
        break;
    case SFGenerator::delayVolEnv:
        volEnv_.setParameter(Envelope::Section::Delay, getModulatedGenerator(SFGenerator::delayVolEnv));
        break;
    case SFGenerator::attackVolEnv:
        volEnv_.setParameter(Envelope::Section::Attack, getModulatedGenerator(SFGenerator::attackVolEnv));
        break;
    case SFGenerator::holdVolEnv:
    case SFGenerator::keynumToVolEnvHold:
        volEnv_.setParameter(Envelope::Section::Hold,
            getModulatedGenerator(SFGenerator::holdVolEnv) + getModulatedGenerator(SFGenerator::keynumToVolEnvHold) * (60 - key_));
        break;
    case SFGenerator::decayVolEnv:
    case SFGenerator::keynumToVolEnvDecay:
        volEnv_.setParameter(Envelope::Section::Decay,
            getModulatedGenerator(SFGenerator::decayVolEnv) + getModulatedGenerator(SFGenerator::keynumToVolEnvDecay) * (60 - key_));
        break;
    case SFGenerator::sustainVolEnv:
        volEnv_.setParameter(Envelope::Section::Sustain, getModulatedGenerator(SFGenerator::sustainVolEnv));
        break;
    case SFGenerator::releaseVolEnv:
        volEnv_.setParameter(Envelope::Section::Release, getModulatedGenerator(SFGenerator::releaseVolEnv));
        break;
    case SFGenerator::coarseTune:
    case SFGenerator::fineTune:
    case SFGenerator::scaleTuning:
    case SFGenerator::pitch:
        voicePitch_ = sample_.pitch
            + 1e-4 * modulations_.at(static_cast<size_t>(SFGenerator::pitch))
            + 0.01 * getModulatedGenerator(SFGenerator::scaleTuning) * (actualKey_ - sample_.pitch)
            + getModulatedGenerator(SFGenerator::coarseTune)
            + 0.01 * getModulatedGenerator(SFGenerator::fineTune);;
        break;
    }
}

}