#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace Shortcuts
{
    enum class Context { Global, Arrangement, PianoRoll, Songwriting, Text, Mixer };

    enum Command
    {
        cmdNew = 1, cmdOpen, cmdSave, cmdSaveAs, cmdImport, cmdExport, cmdAudioSettings, cmdMidiSettings, cmdPlugins,
        cmdUndo, cmdRedo, cmdCut, cmdCopy, cmdPaste, cmdDuplicate, cmdSplit, cmdDelete, cmdSelectAll,
        cmdPlayStop, cmdRecord, cmdRecall, cmdToStart, cmdPunch, cmdPunchMode, cmdLoop, cmdPunchIn, cmdPunchOut, cmdClick, cmdCountIn,
        cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdMixer, cmdShowShortcuts, cmdShowTuner,
        cmdPrevious, cmdNext, cmdPreviousBar, cmdNextBar,
        cmdQuantize, cmdPitchUp, cmdPitchDown, cmdOctaveUp, cmdOctaveDown, cmdCloseEditor, cmdEditSongNote
    };

    struct Binding
    {
        Context context;
        Command command;
        const char* description;
        int key;
        int modifiers = 0;
        int alternate = 0;
        int alternateModifiers = 0;
    };

    inline const Binding bindings[] {
        { Context::Global, cmdShowShortcuts, "Keyboard shortcuts", juce::KeyPress::F1Key, 0, '/', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdNew, "New project", 'N', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdOpen, "Open project", 'O', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdSave, "Save project", 'S', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdSaveAs, "Save project as", 'S', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier },
        { Context::Global, cmdImport, "Import audio", 'I', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdExport, "Export audio", 'E', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdAudioSettings, "Audio settings", ',', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdUndo, "Undo project edit", 'Z', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdRedo, "Redo project edit", 'Z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 'Y', juce::ModifierKeys::commandModifier },
        { Context::Global, cmdPlayStop, "Play / stop", juce::KeyPress::spaceKey },
        { Context::Global, cmdRecord, "Record / stop recording", 'R' },
        { Context::Global, cmdToStart, "Go to start", juce::KeyPress::homeKey },
        { Context::Global, cmdToStart, "Go to start outside the songwriting lanes", juce::KeyPress::returnKey },
        { Context::Global, cmdPunch, "Cycle off / punch / loop", 'P' },
        { Context::Global, cmdLoop, "Toggle loop", 'L' },
        { Context::Global, cmdPunchIn, "Set range start at playhead", 'I' },
        { Context::Global, cmdPunchOut, "Set range end at playhead", 'O' },
        { Context::Global, cmdClick, "Toggle metronome", 'C' },
        { Context::Global, cmdShowTuner, "Open tuner", 'T' },
        { Context::Global, cmdZoomIn, "Zoom in on arrangement", '=', 0, '=', juce::ModifierKeys::shiftModifier },
        { Context::Global, cmdZoomIn, "Zoom in on arrangement", '+', 0, '+', juce::ModifierKeys::shiftModifier },
        { Context::Global, cmdZoomOut, "Zoom out on arrangement", '-' },
        { Context::Global, cmdZoomFit, "Zoom arrangement to fit", 'Z' },
        { Context::Global, cmdMixer, "Show / hide mixer", 'X' },
        { Context::Arrangement, cmdCut, "Cut selected clips", 'X', juce::ModifierKeys::commandModifier },
        { Context::Arrangement, cmdCopy, "Copy selected clips", 'C', juce::ModifierKeys::commandModifier },
        { Context::Arrangement, cmdPaste, "Paste clips at playhead", 'V', juce::ModifierKeys::commandModifier },
        { Context::Arrangement, cmdDuplicate, "Duplicate selected clips", 'D', juce::ModifierKeys::commandModifier },
        { Context::Arrangement, cmdSelectAll, "Select all clips", 'A', juce::ModifierKeys::commandModifier },
        { Context::Arrangement, cmdDelete, "Delete selected clips", juce::KeyPress::backspaceKey, 0, juce::KeyPress::deleteKey },
        { Context::Arrangement, cmdSplit, "Split clips at playhead", 'S' },
        { Context::Arrangement, cmdPrevious, "Move playhead left by one grid step", juce::KeyPress::leftKey },
        { Context::Arrangement, cmdNext, "Move playhead right by one grid step", juce::KeyPress::rightKey },
        { Context::Arrangement, cmdPreviousBar, "Move playhead left by one bar", juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier },
        { Context::Arrangement, cmdNextBar, "Move playhead right by one bar", juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier },
        { Context::PianoRoll, cmdCut, "Cut selected notes", 'X', juce::ModifierKeys::commandModifier },
        { Context::PianoRoll, cmdCopy, "Copy selected notes", 'C', juce::ModifierKeys::commandModifier },
        { Context::PianoRoll, cmdPaste, "Paste notes at insertion beat", 'V', juce::ModifierKeys::commandModifier },
        { Context::PianoRoll, cmdDuplicate, "Duplicate selected notes", 'D', juce::ModifierKeys::commandModifier },
        { Context::PianoRoll, cmdSelectAll, "Select all notes", 'A', juce::ModifierKeys::commandModifier },
        { Context::PianoRoll, cmdDelete, "Delete selected notes", juce::KeyPress::backspaceKey, 0, juce::KeyPress::deleteKey },
        { Context::PianoRoll, cmdQuantize, "Quantize selected notes", 'Q' },
        { Context::PianoRoll, cmdPrevious, "Move selected notes left by one grid step", juce::KeyPress::leftKey },
        { Context::PianoRoll, cmdNext, "Move selected notes right by one grid step", juce::KeyPress::rightKey },
        { Context::PianoRoll, cmdPitchUp, "Raise selected notes by one semitone", juce::KeyPress::upKey },
        { Context::PianoRoll, cmdPitchDown, "Lower selected notes by one semitone", juce::KeyPress::downKey },
        { Context::PianoRoll, cmdOctaveUp, "Raise selected notes by one octave", juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier },
        { Context::PianoRoll, cmdOctaveDown, "Lower selected notes by one octave", juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier },
        { Context::PianoRoll, cmdCloseEditor, "Close piano roll", juce::KeyPress::escapeKey },
        { Context::Songwriting, cmdEditSongNote, "Add or edit a chord or lyric", juce::KeyPress::returnKey },
        { Context::Songwriting, cmdCut, "Cut selected chord or lyric", 'X', juce::ModifierKeys::commandModifier },
        { Context::Songwriting, cmdCopy, "Copy selected chord or lyric", 'C', juce::ModifierKeys::commandModifier },
        { Context::Songwriting, cmdPaste, "Paste text at the lane cursor", 'V', juce::ModifierKeys::commandModifier },
        { Context::Songwriting, cmdDelete, "Delete selected chord or lyric", juce::KeyPress::backspaceKey, 0, juce::KeyPress::deleteKey },
        { Context::Songwriting, cmdPrevious, "Move the lane cursor one beat left", juce::KeyPress::leftKey },
        { Context::Songwriting, cmdNext, "Move the lane cursor one beat right", juce::KeyPress::rightKey },
        { Context::Songwriting, cmdCloseEditor, "Return keyboard focus to the arrangement", juce::KeyPress::escapeKey }
    };

    inline const Binding* find(Context context, const juce::KeyPress& key)
    {
        const auto matches = [&](const Binding& binding)
        {
            const auto code = key.getKeyCode();
            const auto modifiers = key.getModifiers().getRawFlags() & juce::ModifierKeys::allKeyboardModifiers;
            const auto match = [&](int candidate, int flags)
            {
                return candidate != 0 && flags == modifiers
                    && (code == candidate || (candidate < 128 && juce::CharacterFunctions::toUpperCase(
                                                static_cast<juce::juce_wchar>(code)) == static_cast<juce::juce_wchar>(candidate)));
            };
            return match(binding.key, binding.modifiers) || match(binding.alternate, binding.alternateModifiers);
        };

        for (const auto wanted : { context, Context::Global })
            for (const auto& binding : bindings)
                if (binding.context == wanted && matches(binding)
                    && (context != Context::Text || binding.command == cmdShowShortcuts))
                    return &binding;
        return nullptr;
    }

    inline juce::String keys(const Binding& binding)
    {
        const auto describe = [](int code, int modifiers)
        {
            if (code == juce::KeyPress::backspaceKey)
                return juce::String(JUCE_MAC ? "Delete" : "Backspace");
            if (code == juce::KeyPress::deleteKey)
                return juce::String(JUCE_MAC ? "Forward Delete" : "Delete");
            return juce::KeyPress(code, juce::ModifierKeys(modifiers), 0).getTextDescriptionWithIcons();
        };
        auto result = describe(binding.key, binding.modifiers);
        if (binding.alternate != 0 && binding.alternate != binding.key)
            result += "  /  " + describe(binding.alternate, binding.alternateModifiers);
        return result;
    }

    inline juce::String keysFor(Context context, int command)
    {
        if (context == Context::Text)
            context = Context::Arrangement;
        if (command == cmdToStart && context != Context::Songwriting)
            return juce::KeyPress(juce::KeyPress::returnKey, {}, 0).getTextDescriptionWithIcons();
        for (const auto wanted : { context, Context::Global })
            for (const auto& binding : bindings)
                if (binding.context == wanted && binding.command == command)
                    return keys(binding);
        return {};
    }
}
