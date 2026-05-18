#include "DropZoneComponent.h"

//==============================================================================
DropZoneComponent::DropZoneComponent (MorphAudioProcessor& processor)
    : proc (processor)
{
    importFolderButton.setButtonText ("Import Folder");
    importFolderButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a2a));
    importFolderButton.setColour (juce::TextButton::textColourOffId, juce::Colour (kText).withAlpha (0.8f));
    importFolderButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> (
            "Select a folder to import",
            juce::File::getSpecialLocation (juce::File::userHomeDirectory),
            "*",
            false);

        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser] (const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result.isDirectory())
                {
                    auto files = collectAudioFiles (result);
                    if (!files.isEmpty())
                        uploadFiles (files);
                }
            });
    };
    addAndMakeVisible (importFolderButton);
}

//==============================================================================
void DropZoneComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (4.0f, 4.0f);

    // Background
    juce::Colour bgColour = juce::Colour (kBG);
    if (currentState == State::DragOver)
        bgColour = juce::Colour (kTeal).withAlpha (0.15f);
    else if (currentState == State::Success)
        bgColour = juce::Colour (kGreen).withAlpha (0.15f);

    g.setColour (bgColour);
    g.fillRoundedRectangle (bounds, 6.0f);

    // Dashed border
    juce::Colour borderColour;
    if (currentState == State::DragOver)
        borderColour = juce::Colour (kTeal);
    else if (currentState == State::Success)
        borderColour = juce::Colour (kGreen);
    else
        borderColour = juce::Colour (kTeal).withAlpha (0.5f);

    g.setColour (borderColour);
    juce::Path dashedRect;
    dashedRect.addRoundedRectangle (bounds, 6.0f);
    const float dashes[] = { 6.0f, 4.0f };
    juce::Path dashedPath;
    juce::PathStrokeType strokeType (1.5f);
    strokeType.createDashedStroke (dashedPath, dashedRect, dashes, juce::numElementsInArray (dashes));
    g.strokePath (dashedPath, strokeType);

    // Icon (simple cloud shape via unicode) + text
    juce::String label;
    switch (currentState)
    {
        case State::Idle:
            label = "Drop audio here  or";
            break;
        case State::DragOver:
            label = "Release to upload";
            break;
        case State::Uploading:
            label = "Uploading... " + juce::String (uploadedCount) + " / " + juce::String (totalCount);
            break;
        case State::Success:
            label = "Added to Water";
            break;
    }

    g.setColour (juce::Colour (kText).withAlpha (currentState == State::DragOver ? 1.0f : 0.7f));
    g.setFont (juce::Font (12.0f));

    // Leave room for the Import Folder button on the right
    auto textArea = getLocalBounds().withTrimmedRight (110);
    g.drawText (label, textArea, juce::Justification::centred, true);
}

//==============================================================================
void DropZoneComponent::resized()
{
    importFolderButton.setBounds (getWidth() - 100, (getHeight() - 24) / 2, 92, 24);
}

//==============================================================================
bool DropZoneComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& path : files)
        if (isAudioFile (juce::File (path)))
            return true;
    return false;
}

void DropZoneComponent::fileDragEnter (const juce::StringArray& /*files*/, int, int)
{
    setState (State::DragOver);
}

void DropZoneComponent::fileDragExit (const juce::StringArray& /*files*/)
{
    setState (State::Idle);
}

void DropZoneComponent::filesDropped (const juce::StringArray& filePaths, int, int)
{
    setState (State::Idle);

    juce::Array<juce::File> audioFiles;
    for (auto& p : filePaths)
    {
        juce::File f (p);
        if (f.isDirectory())
        {
            for (auto& af : collectAudioFiles (f))
                audioFiles.addIfNotAlreadyThere (af);
        }
        else if (isAudioFile (f))
        {
            audioFiles.addIfNotAlreadyThere (f);
        }
    }

    if (!audioFiles.isEmpty())
    {
        // Analyze the first file immediately (BRAIN → BPM + key, no playback needed)
        proc.analyzeFileDirectly (audioFiles.getFirst());
        // Upload all files to Water — triggers collaborator prompt on the web
        uploadFiles (audioFiles);
    }
}

//==============================================================================
bool DropZoneComponent::isAudioFile (const juce::File& f)
{
    static const juce::StringArray exts { ".mp3", ".wav", ".aiff", ".aif",
                                           ".flac", ".ogg", ".m4a" };
    return exts.contains (f.getFileExtension().toLowerCase());
}

juce::Array<juce::File> DropZoneComponent::collectAudioFiles (const juce::File& folder)
{
    juce::Array<juce::File> result;
    for (auto& f : folder.findChildFiles (juce::File::findFiles, true,
                                          "*.mp3;*.wav;*.aiff;*.aif;*.flac;*.ogg;*.m4a"))
    {
        result.add (f);
    }
    return result;
}

//==============================================================================
void DropZoneComponent::uploadFiles (const juce::Array<juce::File>& files)
{
    if (files.isEmpty()) return;

    totalCount    = files.size();
    uploadedCount = 0;
    setState (State::Uploading);

    uploadNext (files, 0);
}

void DropZoneComponent::uploadNext (juce::Array<juce::File> files, int index)
{
    if (index >= files.size())
    {
        // All done
        setState (State::Success);

        // Flash success then return to idle after 2 seconds
        juce::Timer::callAfterDelay (2000, [this]
        {
            setState (State::Idle);
            if (onUploadComplete) onUploadComplete();
        });
        return;
    }

    proc.uploadFile (files[index],
        [this, files, index] (bool /*ok*/, juce::String /*result*/)
        {
            uploadedCount++;
            repaint();
            uploadNext (files, index + 1);
        });
}

//==============================================================================
void DropZoneComponent::setState (State s)
{
    currentState = s;
    repaint();
}
