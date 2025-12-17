#include "TransportController.h"

TransportController::TransportController()
{
    juce::Logger::writeToLog("TransportController: Initialized");
}

TransportController::~TransportController()
{
}

void TransportController::play()
{
    if (currentState.load() != State::PLAYING)
    {
        setState(State::PLAYING);
        juce::Logger::writeToLog("Transport: PLAY");
    }
}

void TransportController::stop()
{
    if (currentState.load() != State::STOPPED)
    {
        setState(State::STOPPED);
        resetPosition();
        juce::Logger::writeToLog("Transport: STOP (position reset)");
    }
}

void TransportController::pause()
{
    if (currentState.load() != State::STOPPED)
    {
        setState(State::STOPPED);
        juce::Logger::writeToLog("Transport: PAUSE (position preserved)");
    }
}

void TransportController::record()
{
    if (currentState.load() != State::RECORDING)
    {
        setState(State::RECORDING);
        juce::Logger::writeToLog("Transport: RECORD");
    }
}

void TransportController::togglePlayback()
{
    State current = currentState.load();

    if (current == State::PLAYING || current == State::RECORDING)
    {
        // If playing or recording, pause (but don't reset position)
        pause();
    }
    else
    {
        // If stopped, start playing
        play();
    }
}

void TransportController::fastForward(double seconds, double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    juce::int64 samplesToSkip = static_cast<juce::int64>(seconds * sampleRate);
    juce::int64 newPosition = position.load() + samplesToSkip;

    // Don't go negative
    if (newPosition < 0)
        newPosition = 0;

    position = newPosition;
    juce::Logger::writeToLog("Transport: FFD to " + juce::String(getPositionInSeconds(sampleRate), 2) + "s");
}

void TransportController::rewind(double seconds, double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    juce::int64 samplesToSkip = static_cast<juce::int64>(seconds * sampleRate);
    juce::int64 newPosition = position.load() - samplesToSkip;

    // Don't go negative
    if (newPosition < 0)
        newPosition = 0;

    position = newPosition;
    juce::Logger::writeToLog("Transport: RWD to " + juce::String(getPositionInSeconds(sampleRate), 2) + "s");
}

void TransportController::setState(State newState)
{
    State oldState = currentState.exchange(newState);

    if (oldState != newState && stateChangedCallback)
    {
        stateChangedCallback(newState);
    }
}

double TransportController::getPositionInSeconds(double sampleRate) const
{
    if (sampleRate <= 0.0)
        return 0.0;

    return static_cast<double>(position.load()) / sampleRate;
}

void TransportController::setPositionInSeconds(double seconds, double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    position = static_cast<juce::int64>(seconds * sampleRate);
}
