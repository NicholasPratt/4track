#include "SamplerEditors.h"
#include "Theme.h"
#include "../Audio/AudioEngine.h"

namespace
{
    constexpr int kMargin = EffectEditor::kMargin;
    const juce::String audioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg";

    bool isAudioFile(const juce::String& path)
    {
        return juce::File(path).hasFileExtension("wav;aif;aiff;flac;mp3;ogg");
    }

    void warn(const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::WarningIcon)
                                         .withTitle(title)
                                         .withMessage(message)
                                         .withButton("OK", 0),
                                     nullptr);
    }

    void drawWave(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<float>& peaks, juce::Colour colour)
    {
        if (peaks.empty())
            return;
        const float mid = r.getCentreY(), half = r.getHeight() * 0.5f - 2.0f;
        const float w = r.getWidth() / (float) peaks.size();
        g.setColour(colour);
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const float h = juce::jmax(0.5f, peaks[i] * half);
            g.fillRect(r.getX() + (float) i * w, mid - h, juce::jmax(1.0f, w - 0.5f), 2.0f * h);
        }
    }
}

//==============================================================================
class DrumSamplerEditor::PadButton : public juce::Component, public juce::SettableTooltipClient, public juce::FileDragAndDropTarget
{
public:
    PadButton(DrumSamplerEditor& o, int p) : owner(o), pad(p)
    {
        setRepaintsOnMouseActivity(true);
        setTooltip("Click to play; drop a sound here, or right-click to load one");
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        const float lit = owner.sampler.getActivity(pad);
        const bool chosen = owner.selected == pad;
        g.setColour(Theme::control.interpolatedWith(Theme::accent, 0.6f * lit).brighter(isMouseOver() ? 0.08f : 0.0f));
        g.fillRoundedRectangle(r, 6.0f);
        if (chosen || dragging)
        {
            g.setColour(Theme::accent);
            g.drawRoundedRectangle(r.reduced(1.0f), 6.0f, 2.0f);
        }
        auto sample = owner.sampler.getPadSample(pad);
        auto text = r.reduced(8.0f, 5.0f);
        g.setColour(Theme::textDim);
        g.setFont(Theme::font(10.5f, true));
        g.drawText(juce::String(pad + 1) + "  " + juce::MidiMessage::getMidiNoteName(DrumSampler::kFirstNote + pad, true, true, 3),
                   text.removeFromTop(13.0f), juce::Justification::topLeft);
        if (sample != nullptr)
        {
            if (sample != shownSample)
            {
                shownSample = sample;
                peaks = sample->peaks(40);
            }
            drawWave(g, text.removeFromBottom(16.0f), peaks, Theme::textDim.withAlpha(0.5f));
            g.setColour(Theme::text);
            g.setFont(Theme::font(11.5f));
            g.drawFittedText(sample->name, text.toNearestInt(), juce::Justification::centredLeft, 1);
        }
        else
        {
            g.setColour(Theme::textDim.withAlpha(0.5f));
            g.setFont(Theme::font(11.0f));
            g.drawText("Empty", text, juce::Justification::centredLeft);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        owner.select(pad);
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem(1, "Load Sound...");
            menu.addItem(2, "Clear", owner.sampler.getPadSample(pad) != nullptr);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                               [safe = SafePointer<PadButton>(this)](int result)
            {
                if (safe == nullptr) return;
                if (result == 1) safe->choose();
                if (result == 2) safe->set({});
            });
            return;
        }
        // Harder towards the top of the pad, like a velocity-sensitive pad
        const int velocity = juce::jlimit(30, 127, juce::roundToInt(127.0f - 70.0f * (float) e.y / (float) getHeight()));
        owner.sampler.audition(pad, velocity);
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override { return files.size() >= 1 && isAudioFile(files[0]); }
    void fileDragEnter(const juce::StringArray&, int, int) override { dragging = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        dragging = false;
        // Several files fill this pad and the ones after it.
        int p = pad;
        for (auto& f : files)
            if (isAudioFile(f) && p < DrumSampler::kPads)
                owner.load(p++, juce::File(f));
        owner.select(pad);
    }

private:
    void choose()
    {
        chooser = std::make_unique<juce::FileChooser>("Load a sound onto pad " + juce::String(pad + 1),
                                                      owner.sampler.getPadFile(pad), audioWildcard);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = SafePointer<PadButton>(this)](const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File())
                safe->set(fc.getResult());
        });
    }
    void set(const juce::File& f) { owner.load(pad, f); }

    DrumSamplerEditor& owner;
    const int pad;
    bool dragging = false;
    std::unique_ptr<juce::FileChooser> chooser;
    Sample::Ptr shownSample;
    std::vector<float> peaks;
};

DrumSamplerEditor::DrumSamplerEditor(DrumSampler& s)
    : sampler(s), kitControls(s, [](const juce::String& row) { return row == "Kit"; })
{
    for (int i = 0; i < DrumSampler::kPads; ++i)
        addAndMakeVisible(padButtons.add(new PadButton(*this, i)));
    addAndMakeVisible(kitControls);
    hint.setText("Drop sounds on the pads. MIDI notes C1 to D#2 play them.", juce::dontSendNotification);
    hint.setFont(Theme::font(11.0f));
    hint.setColour(juce::Label::textColourId, Theme::textDim);
    addAndMakeVisible(hint);
    select(0);
    startTimerHz(30);
}

DrumSamplerEditor::~DrumSamplerEditor() { stopTimer(); }

void DrumSamplerEditor::load(int pad, const juce::File& file)
{
    juce::String error;
    if (!sampler.loadPad(pad, file, error))
        warn("Couldn't load " + file.getFileName(), error);
    padButtons[pad]->repaint();
}

void DrumSamplerEditor::select(int pad)
{
    selected = pad;
    const auto row = DrumSampler::rowFor(pad);
    padControls = std::make_unique<EffectControls>(sampler, [row](const juce::String& r) { return r == row; });
    addAndMakeVisible(*padControls);
    for (auto* b : padButtons)
        b->repaint();
    layout();
}

void DrumSamplerEditor::layout()
{
    const int width = juce::jmax(padControls->naturalWidth(), kitControls.naturalWidth(), 560);
    const int height = kMargin + 4 * kPadHeight + 3 * kGridGap + 8 + 18 + 8 + padControls->heightForWidth(width)
                       + kitControls.heightForWidth(width) + kMargin;
    if (getWidth() != width + 2 * kMargin || getHeight() != height)
        setSize(width + 2 * kMargin, height);
    else
        resized();
}

void DrumSamplerEditor::resized()
{
    auto r = getLocalBounds().reduced(kMargin, 0);
    r.removeFromTop(kMargin);
    auto grid = r.removeFromTop(4 * kPadHeight + 3 * kGridGap);
    const int padWidth = (grid.getWidth() - 3 * kGridGap) / 4;
    for (int i = 0; i < DrumSampler::kPads; ++i)
    {
        // Pad 1 bottom left, like a drum machine
        const int col = i % 4, rowFromBottom = i / 4;
        padButtons[i]->setBounds(grid.getX() + col * (padWidth + kGridGap),
                                 grid.getBottom() - (rowFromBottom + 1) * kPadHeight - rowFromBottom * kGridGap,
                                 padWidth, kPadHeight);
    }
    r.removeFromTop(8);
    hint.setBounds(r.removeFromTop(18));
    r.removeFromTop(8);
    if (padControls != nullptr)
        padControls->setBounds(r.removeFromTop(padControls->heightForWidth(r.getWidth())));
    kitControls.setBounds(r.removeFromTop(kitControls.heightForWidth(r.getWidth())));
}

void DrumSamplerEditor::paint(juce::Graphics& g) { g.fillAll(Theme::panel); }

void DrumSamplerEditor::timerCallback()
{
    for (int i = 0; i < DrumSampler::kPads; ++i)
        if (sampler.getActivity(i) > 0.01f)
            padButtons[i]->repaint();
}

//==============================================================================
KeySamplerEditor::KeySamplerEditor(KeySampler& s, AudioEngine* e) : sampler(s), engine(e), controls(s)
{
    record.setColour(juce::TextButton::buttonOnColourId, Theme::record);
    record.setClickingTogglesState(false);
    record.onClick = [this] { toggleRecording(); };
    record.setTooltip("Record the input into the Sampler; click again to stop");
    detect.onClick = [this]
    {
        if (!sampler.detectRoot())
            warn("No clear note", "Couldn't hear one steady pitch in the sound. Set Root by ear instead.");
    };
    detect.setTooltip("Listen to the sound and set Root to its note");
    input.setTooltip("The input to record from");
    status.setFont(Theme::font(11.0f));
    status.setColour(juce::Label::textColourId, Theme::textDim);
    for (auto* c : std::initializer_list<juce::Component*> { &input, &record, &detect, &status, &keyboard, &controls })
        addAndMakeVisible(c);
    keyboard.setAvailableRange(24, 96);
    keyboard.setLowestVisibleKey(24);
    keyboard.setOctaveForMiddleC(4);   // middle C is C4, as Root shows it
    keyboard.setKeyWidth(14.0f);
    keyboard.setWantsKeyboardFocus(false);
    keyState.addListener(this);
    refreshInputs();
    record.setEnabled(engine != nullptr && !inputs.empty());

    const int width = juce::jmax(controls.naturalWidth(), 560);
    setSize(width + 2 * kMargin, kMargin + kWaveHeight + 8 + kToolbar + 8 + kKeys + 12 + controls.heightForWidth(width) + kMargin);
    startTimerHz(30);
}

KeySamplerEditor::~KeySamplerEditor()
{
    stopTimer();
    keyState.removeListener(this);
    if (recorder != nullptr && recorder->isRecording())
        recorder->stop({});
}

void KeySamplerEditor::refreshInputs()
{
    input.clear(juce::dontSendNotification);
    inputs.clear();
    if (engine == nullptr)
        return;
    for (auto& in : engine->getInputs())
    {
        inputs.push_back({ in.channel, in.numChannels });
        input.addItem(in.label.isNotEmpty() && in.label != in.name ? in.name + " (" + in.label + ")" : in.name, (int) inputs.size());
    }
    if (!inputs.empty())
        input.setSelectedItemIndex(0, juce::dontSendNotification);
}

juce::Rectangle<int> KeySamplerEditor::waveBounds() const
{
    return getLocalBounds().reduced(kMargin).removeFromTop(kWaveHeight);
}

void KeySamplerEditor::resized()
{
    auto r = getLocalBounds().reduced(kMargin);
    r.removeFromTop(kWaveHeight + 8);
    auto bar = r.removeFromTop(kToolbar);
    input.setBounds(bar.removeFromLeft(200));
    bar.removeFromLeft(8);
    record.setBounds(bar.removeFromLeft(90));
    bar.removeFromLeft(8);
    detect.setBounds(bar.removeFromRight(100));
    status.setBounds(bar.reduced(6, 0));
    r.removeFromTop(8);
    keyboard.setBounds(r.removeFromTop(kKeys));
    keyboard.setKeyWidth((float) keyboard.getWidth() / 43.0f);   // C1 to C7: 43 white keys, edge to edge
    r.removeFromTop(12);
    controls.setBounds(r);
}

void KeySamplerEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::panel);
    const auto wave = waveBounds().toFloat();
    g.setColour(Theme::laneA);
    g.fillRoundedRectangle(wave, 6.0f);
    const bool recording = recorder != nullptr && recorder->isRecording();
    auto sample = sampler.getSample();
    if (recording)
    {
        g.setColour(Theme::record);
        g.setFont(Theme::font(14.0f, true));
        g.drawText("Recording  " + juce::String(recorder->getSeconds(), 1) + " s", wave, juce::Justification::centred);
        const float level = juce::jlimit(0.0f, 1.0f, recorder->getLevel());
        g.fillRoundedRectangle(wave.reduced(10.0f).removeFromBottom(6.0f).withWidth((wave.getWidth() - 20.0f) * level), 3.0f);
    }
    else if (sample != nullptr)
    {
        const auto inner = wave.reduced(6.0f, 8.0f);
        drawWave(g, inner, peaks, Theme::accent.withAlpha(0.8f));
        auto marker = [&](float fraction, juce::Colour c, const juce::String& label)
        {
            const float x = inner.getX() + fraction * inner.getWidth();
            g.setColour(c);
            g.fillRect(x - 0.75f, inner.getY(), 1.5f, inner.getHeight());
            g.setFont(Theme::font(10.0f, true));
            g.drawText(label, juce::Rectangle<float>(x + 3.0f, inner.getY(), 60.0f, 12.0f), juce::Justification::topLeft);
        };
        marker(sampler.start->getCurrentValue(), Theme::text, "Start");
        if (sampler.loop->getCurrentValue() >= 0.5f)
            marker(sampler.loopStart->getCurrentValue(), Theme::mute, "Loop");
        g.setColour(Theme::textDim);
        g.setFont(Theme::font(11.0f));
        g.drawText(sample->name + "   " + juce::String(sample->lengthSeconds(), 2) + " s",
                   wave.reduced(10.0f, 6.0f), juce::Justification::bottomRight);
    }
    else
    {
        g.setColour(Theme::textDim);
        g.setFont(Theme::font(13.0f));
        g.drawText("Drop a sound here, load one below, or record one from an input",
                   wave, juce::Justification::centred);
    }
    if (dragging)
    {
        g.setColour(Theme::accent);
        g.drawRoundedRectangle(wave.reduced(1.0f), 6.0f, 2.0f);
    }
}

bool KeySamplerEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    return files.size() == 1 && isAudioFile(files[0]);
}

void KeySamplerEditor::filesDropped(const juce::StringArray& files, int, int)
{
    dragging = false;
    juce::String error;
    if (!sampler.loadFile(juce::File(files[0]), error))
        warn("Couldn't load " + juce::File(files[0]).getFileName(), error);
    repaint();
}

void KeySamplerEditor::toggleRecording()
{
    if (recorder != nullptr && recorder->isRecording())
    {
        finishRecording();
        return;
    }
    if (engine == nullptr || inputs.empty())
        return;
    const auto& in = inputs[(size_t) juce::jmax(0, input.getSelectedItemIndex())];
    recorder = std::make_unique<InputRecorder>(engine->getTracktion().getDeviceManager());
    recorder->onLimitReached = [this] { finishRecording(); };
    recorder->start(in.channel, in.numChannels);
    record.setButtonText("Stop");
    record.setToggleState(true, juce::dontSendNotification);
    input.setEnabled(false);
}

void KeySamplerEditor::finishRecording()
{
    auto sample = recorder->stop("Recording");
    record.setButtonText("Record");
    record.setToggleState(false, juce::dontSendNotification);
    input.setEnabled(true);
    juce::String error;
    if (!sampler.useRecording(sample, error))
        warn("Nothing recorded", error);
    repaint();
}

void KeySamplerEditor::timerCallback()
{
    auto sample = sampler.getSample();
    if (sample != shown)
    {
        shown = sample;
        peaks = sample != nullptr ? sample->peaks(juce::jmax(1, waveBounds().getWidth() - 12) / 2) : std::vector<float>();
        repaint(waveBounds());
    }
    else if ((recorder != nullptr && recorder->isRecording()) || sample != nullptr)
    {
        repaint(waveBounds());   // the level, or the markers following their knobs
    }
    if (recorder != nullptr && recorder->isRecording())
        status.setText({}, juce::dontSendNotification);
    else
        status.setText(sample != nullptr ? "Root " + juce::MidiMessage::getMidiNoteName(juce::roundToInt(sampler.root->getCurrentValue()), true, true, 4)
                                         : juce::String(), juce::dontSendNotification);
}

//==============================================================================
std::unique_ptr<te::Plugin::EditorComponent> createSamplerEditor(EffectPlugin& e, AudioEngine* engine)
{
    if (auto* drums = dynamic_cast<DrumSampler*>(&e))
        return std::make_unique<DrumSamplerEditor>(*drums);
    if (auto* keys = dynamic_cast<KeySampler*>(&e))
        return std::make_unique<KeySamplerEditor>(*keys, engine);
    return nullptr;
}
