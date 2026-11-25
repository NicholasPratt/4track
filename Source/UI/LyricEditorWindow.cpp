#include "LyricEditorWindow.h"
#include "Theme.h"

namespace
{
    class DraftTextEditor final : public juce::TextEditor
    {
    public:
        void importLines(const juce::String& lines, bool replace)
        {
            newTransaction();
            if (replace)
                selectAll();
            else
                moveCaretToEnd();
            insertTextAtCaret(lines);
            newTransaction();
        }
    };
}

class LyricEditor final : public juce::Component, private juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit LyricEditor(Project& p) : project(p), handles(*this)
    {
        hint.setText("Drag a line's handle onto a beat in the Lyrics lane.", juce::dontSendNotification);
        hint.setColour(juce::Label::textColourId, Theme::textDim);
        hint.setFont(Theme::font(13));
        addAndMakeVisible(hint);

        text.setTitle("Lyric draft");
        text.setMultiLine(true, true);
        text.setReturnKeyStartsNewLine(true);
        text.setScrollbarsShown(true);
        text.setFont(Theme::font(16));
        text.setIndents(8, 6);
        text.setTextToShowWhenEmpty("Write your lyrics here...", Theme::textDim);
        text.onTextChange = [this] { draftChanged(); };
        addAndMakeVisible(text);
        addAndMakeVisible(handles);

        importButton.onClick = [this] { importTimeline(); };
        exportButton.onClick = [this] { exportSheet(); };
        addAndMakeVisible(importButton);
        addAndMakeVisible(exportButton);
        setSize(540, 390);
        refreshProject();
    }

    void refreshProject()
    {
        const auto current = project.getEdit().state;
        importButton.setEnabled(!project.timelineLyricText().isEmpty());
        if (document == current)
            return;
        document = current;
        text.clear();
        text.setText(project.getLyricDraft(), false);
        text.setCaretPosition(0);
        draftChanged();
    }

    void storeDraft()
    {
        if (document == project.getEdit().state)
            project.setLyricDraft(text.getText());
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(14);
        hint.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(6);
        auto footer = bounds.removeFromBottom(28);
        exportButton.setBounds(footer.removeFromRight(132));
        importButton.setBounds(footer.removeFromLeft(138));
        bounds.removeFromBottom(10);
        handles.setBounds(bounds.removeFromLeft(25));
        text.setBounds(bounds);
        handles.repaint();
    }

    void trackScroll(bool track)
    {
        if (track)
            startTimerHz(24);
        else
            stopTimer();
    }

private:
    class LineHandles final : public juce::Component
    {
    public:
        explicit LineHandles(LyricEditor& owner) : editor(owner)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(Theme::panelLight);
            visitVisibleLines([&](juce::Range<int>, juce::Rectangle<int> row)
            {
                g.setColour(Theme::textDim);
                for (int y = -4; y <= 4; y += 4)
                    for (int x = -3; x <= 1; x += 4)
                        g.fillEllipse(static_cast<float>(getWidth() / 2 + x),
                                      static_cast<float>(row.getCentreY() + y), 2.0f, 2.0f);
            });
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            pendingLine.clear();
            if (e.mods.isPopupMenu() || !e.mods.isLeftButtonDown())
                return;
            visitVisibleLines([&](juce::Range<int> line, juce::Rectangle<int> row)
            {
                if (row.contains(e.getPosition()))
                    pendingLine = editor.draft.substring(line.getStart(), line.getEnd()).trim();
            });
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (pendingLine.isEmpty() || !e.mouseWasDraggedSinceMouseDown())
                return;
            auto line = std::exchange(pendingLine, {});
            const auto font = Theme::font(14);
            const int width = juce::jlimit(70, 340, juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, line)) + 20);
            juce::Image image(juce::Image::ARGB, width, 26, true);
            juce::Graphics g(image);
            g.setColour(Theme::accent.withAlpha(0.9f));
            g.fillRoundedRectangle(image.getBounds().toFloat(), 4);
            g.setFont(font);
            g.setColour(Theme::bg);
            g.drawText(line, image.getBounds().reduced(8, 0), juce::Justification::centredLeft, true);
            editor.startDragging(juce::var(new LyricLineDrag(editor.document, line)), this,
                                 juce::ScaledImage(image), true, nullptr, &e.source);
        }

    private:
        template <typename Visit>
        void visitVisibleLines(Visit&& visit)
        {
            const auto firstChar = editor.text.getTextIndexAt(0, 0);
            const auto& ranges = editor.lines;
            auto it = std::upper_bound(ranges.begin(), ranges.end(), firstChar,
                                       [](int index, const auto& range) { return index < range.getStart(); });
            if (it != ranges.begin())
                --it;
            for (; it != ranges.end(); ++it)
            {
                if (it->isEmpty())
                    continue;
                const auto caret = editor.text.getCaretRectangleForCharIndex(it->getStart());
                if (caret.getY() >= getHeight())
                    break;
                if (caret.getBottom() > 0 && editor.draft.substring(it->getStart(), it->getEnd()).trim().isNotEmpty())
                    visit(*it, juce::Rectangle<int>(0, caret.getY(), getWidth(), caret.getHeight()));
            }
        }

        LyricEditor& editor;
        juce::String pendingLine;
    };

    void timerCallback() override
    {
        const int y = text.getCaretRectangleForCharIndex(0).getY();
        if (y != firstLineY)
        {
            firstLineY = y;
            handles.repaint();
        }
    }

    void draftChanged()
    {
        storeDraft();
        draft = text.getText();
        lines.clear();
        int start = 0, end = 0;
        for (auto ch = draft.getCharPointer(); !ch.isEmpty(); ++end)
            if (ch.getAndAdvance() == '\n')
            {
                lines.emplace_back(start, end);
                start = end + 1;
            }
        lines.emplace_back(start, end);
        exportButton.setEnabled(draft.trim().isNotEmpty());
        handles.repaint();
    }

    void importTimeline()
    {
        const auto timeline = project.timelineLyricText();
        if (timeline.isEmpty())
            return;
        if (text.getText().isEmpty())
        {
            text.importLines(timeline, false);
            draftChanged();
            return;
        }

        const auto savedDocument = document;
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Import Timeline Lyrics")
            .withMessage("Add all timeline lyrics after the draft, or use them to replace it.")
            .withButton("Append", 1).withButton("Replace", 2).withButton("Cancel", 0)
            .withAssociatedComponent(this),
            [safe = SafePointer<LyricEditor>(this), savedDocument, timeline](int result)
            {
                if (safe == nullptr || (result != 1 && result != 2) || safe->document != savedDocument)
                    return;
                const auto current = safe->text.getText();
                const auto gap = current.endsWith("\n\n") ? "" : current.endsWithChar('\n') ? "\n" : "\n\n";
                safe->text.importLines(result == 1 ? gap + timeline : timeline, result == 2);
                safe->draftChanged();
            });
    }

    void exportSheet()
    {
        storeDraft();
        const auto sheet = project.lyricSheetText();
        const auto suggested = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(project.getName() + " - Lyrics.txt");
        chooser = std::make_unique<juce::FileChooser>("Export lyric sheet", suggested, "*.txt", true, false, this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = SafePointer<LyricEditor>(this), sheet](const juce::FileChooser& fc)
            {
                const auto chosen = fc.getResult();
                if (safe == nullptr || chosen == juce::File())
                    return;
                const auto target = chosen.hasFileExtension("txt") ? chosen : chosen.withFileExtension("txt");
                const auto save = [target, sheet]
                {
                    if (target.replaceWithText(sheet, false, false, "\n"))
                        target.revealToUser();
                    else
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                            "Couldn't export lyrics", "The lyric sheet could not be saved to " + target.getFullPathName());
                };
                if (target != chosen && target.existsAsFile())
                {
                    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Replace lyric sheet?")
                        .withMessage(target.getFileName() + " already exists.")
                        .withButton("Replace", 1).withButton("Cancel", 0)
                        .withAssociatedComponent(safe.getComponent()),
                        [save](int result) { if (result == 1) save(); });
                }
                else
                    save();
            });
    }

    Project& project;
    juce::ValueTree document;
    DraftTextEditor text;
    LineHandles handles;
    juce::Label hint;
    juce::TextButton importButton { "Import Timeline..." }, exportButton { "Export Text..." };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String draft;
    std::vector<juce::Range<int>> lines;
    int firstLineY = 0;
};

LyricEditorWindow::LyricEditorWindow(Project& project, juce::Rectangle<int> anchor)
    : DocumentWindow("Lyric Editor", Theme::panel, closeButton)
{
    setUsingNativeTitleBar(true);
    setAlwaysOnTop(true);
    editor = new LyricEditor(project);
    setContentOwned(editor, true);
    setResizable(true, false);
    setResizeLimits(380, 250, 1100, 1000);
    centreAroundComponent(nullptr, getWidth(), getHeight());
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(anchor))
    {
        const auto screen = display->userBounds.toNearestInt();
        const int x = juce::jlimit(screen.getX(), juce::jmax(screen.getX(), screen.getRight() - getWidth()), anchor.getX());
        const int y = juce::jlimit(screen.getY(), juce::jmax(screen.getY(), screen.getBottom() - getHeight()), anchor.getBottom() + 16);
        setTopLeftPosition(x, y);
    }
}

void LyricEditorWindow::refreshProject()
{
    editor->refreshProject();
}

void LyricEditorWindow::visibilityChanged()
{
    DocumentWindow::visibilityChanged();
    if (editor != nullptr)
        editor->trackScroll(isVisible());
}

void LyricEditorWindow::closeButtonPressed()
{
    editor->storeDraft();
    setVisible(false);
}
