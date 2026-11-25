#pragma once

#include "Theme.h"
#include <functional>

/**
    Shows that 8track is busy (writing or reading files, rendering, loading) with an
    animated panel over the window, and blocks the mouse and keyboard until it's done.

    run() does the work on a background thread while the window keeps painting, so the
    spinner turns and an optional progress bar fills. It only appears if the work takes
    longer than a moment, so quick jobs don't flash it. The work must not touch the
    Edit or any component.

    runHere() is for work that has to happen on the message thread (opening a project,
    loading a plugin): the panel is shown first, but it can't animate while the work runs.
*/
namespace Busy
{
    /** The component the panel covers (the main window's content). Set once at start-up. */
    void setHost(juce::Component*);

    /** True while a job is running: commands that would start another should wait. */
    bool isBusy();

    /** Runs work on a background thread and returns once it has finished. progress, if
        given, is polled for a value from 0 to 1 to show a progress bar. */
    void run(const juce::String& message, const std::function<void()>& work,
             std::function<float()> progress = {});

    /** Shows the panel, then runs work on the message thread. */
    void runHere(const juce::String& message, const std::function<void()>& work);
}
