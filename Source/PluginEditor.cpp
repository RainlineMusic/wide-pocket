/*
    Wide Pocket - plugin editor.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.
*/

#include "PluginEditor.h"

namespace
{

juce::Font uiFont (float size)
{
    static const juce::String family = []
    {
        const auto fonts = juce::Font::findAllTypefaceNames();
#if JUCE_WINDOWS
        for (auto name : { "Segoe UI Variable Text", "Segoe UI Variable", "Segoe UI" })
            if (fonts.contains (name))
                return juce::String (name);
#elif JUCE_MAC
        for (auto name : { "SF Pro Text", "SF Pro", ".AppleSystemUIFont" })
            if (fonts.contains (name))
                return juce::String (name);
#else
        if (fonts.contains ("Liberation Sans"))
            return juce::String ("Liberation Sans");
#endif
        return juce::Font::getDefaultSansSerifFontName();
    }();

    juce::Font font (juce::FontOptions (family, size, juce::Font::plain));

#if JUCE_WINDOWS
    static const juce::String style = []
    {
        auto styles = juce::Font::findAllTypefaceStyles (family);
        for (auto s : { "Semilight Text", "Semilight", "SemiLight" })
            if (styles.contains (s))
                return juce::String (s);
        return juce::String();
    }();

    if (style.isNotEmpty())
        font.setTypefaceStyle (style);
#endif

    return font;
}

void text (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, float size, juce::Colour c,
           int align = juce::Justification::centredLeft, float glow = 0.0f)
{
    g.setFont (uiFont (size));

    if (glow > 0.0f)
    {
        g.setColour (c.withAlpha (glow));
        const float offsets[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 } };
        for (auto& d : offsets)
            g.drawText (s, r.translated (d[0], d[1]), align);
    }

    g.setColour (c);
    g.drawText (s, r, align);
}

void stroke (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width)
{
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void glowStroke (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width, bool glow)
{
    if (glow)
    {
        stroke (g, p, c.withAlpha (0.09f), width * 4.5f);
        stroke (g, p, c.withAlpha (0.15f), width * 2.3f);
    }

    stroke (g, p, c, width);
}

juce::String hzLabel (double v)
{
    return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
}

} // namespace

// ---------------------------------------------------------------------------
// PocketLook: unchanged from v0.8.
// ---------------------------------------------------------------------------

juce::Colour PocketLook::pick (juce::uint32 neon, juce::uint32 dark, juce::uint32 white) const
{
    return juce::Colour (theme == PocketTheme::Neon ? neon : (theme == PocketTheme::SolidDark ? dark : white));
}

juce::Colour PocketLook::ink() const { return pick (0xffdce5fc, 0xfff0f0f0, 0xff242527); }
juce::Colour PocketLook::muted() const { return pick (0xff97a6c2, 0xffa7a7a7, 0xff6b6c70); }

juce::Font PocketLook::getTextButtonFont (juce::TextButton&, int) { return uiFont (15.0f); }

void PocketLook::drawButtonBackground (juce::Graphics& g, juce::Button&, const juce::Colour&, bool hover, bool down)
{
    auto r = g.getClipBounds().toFloat().reduced (2.0f);

    if (isNeon())
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r.translated (0, 2), 9.0f);
        g.setGradientFill (juce::ColourGradient (pick (hover ? 0xff26354c : 0xff172130, 0, 0), 0, r.getY(),
                                                pick (0xff070d16, 0, 0), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 9.0f);
    }
    else
    {
        auto fill = pick (0, hover ? 0xff3b3b3b : 0xff292929, hover ? 0xffffffff : 0xfff4f4f4);
        if (down)
            fill = fill.contrasting (0.08f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 8.0f);
    }

    g.setColour (pick (0xff34445b, 0xff555555, 0xffc2c3c6));
    g.drawRoundedRectangle (r, 8.0f, 0.8f);
}

void PocketLook::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const auto name = b.getButtonText();
    const auto c = r.getCentre();
    const float s = juce::jmin (r.getWidth(), r.getHeight()) / 40.0f;

    juce::Path p;

    if (name == "power")
    {
        p.addCentredArc (0, 1, 8, 8, 0, 0.65f, juce::MathConstants<float>::twoPi - 0.65f, true);
        p.startNewSubPath (0, -10);
        p.lineTo (0, -1);
        p.applyTransform (juce::AffineTransform::scale (s).translated (c.x, c.y));
        stroke (g, p, ink(), 1.7f * s);
        return;
    }

    if (name == "settings")
    {
        constexpr int teeth = 10;
        for (int i = 0; i < teeth * 4; ++i)
        {
            const float a = float (i) * juce::MathConstants<float>::twoPi / float (teeth * 4) - juce::MathConstants<float>::halfPi;
            const float radius = (i % 4 == 1 || i % 4 == 2) ? 10.0f : 7.7f;
            const auto pt = juce::Point<float> (std::cos (a) * radius, std::sin (a) * radius);
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        p.closeSubPath();
        p.applyTransform (juce::AffineTransform::scale (s).translated (c.x, c.y));
        stroke (g, p, ink(), 1.65f * s);
        g.setColour (ink());
        g.drawEllipse (c.x - 3.2f * s, c.y - 3.2f * s, 6.4f * s, 6.4f * s, 1.65f * s);
        return;
    }

    // Ordinary labelled button, such as the drawer arrow.
    text (g, name, r, 14.0f, ink(), juce::Justification::centred);
}

void PocketLook::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                   juce::Slider::SliderStyle style, juce::Slider&)
{
    const float cy = float (y) + float (h) * 0.5f;
    const float left = style == juce::Slider::TwoValueHorizontal ? minPos : float (x);
    const float right = style == juce::Slider::TwoValueHorizontal ? maxPos : pos;

    g.setColour (pick (0xff172334, 0xff343434, 0xffc9cacc));
    g.fillRoundedRectangle (float (x), cy - 3, float (w), 6, 3);

    if (isNeon())
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4e78ff), left, cy, juce::Colour (0xff35d6dc), right + 1, cy, false));
    else
        g.setColour (pick (0, 0xffd7d7d7, 0xff303236));

    g.fillRoundedRectangle (left, cy - 3, juce::jmax (0.1f, right - left), 6, 3);

    auto thumb = [&] (float px)
    {
        g.setColour (pick (0xffdfebff, 0xffeeeeee, 0xfff2f2f3));
        g.fillEllipse (px - 6, cy - 6, 12, 12);
        g.setColour (pick (0xff638bda, 0xff777777, 0xff85878b));
        g.drawEllipse (px - 6, cy - 6, 12, 12, 1);
    };

    if (style == juce::Slider::TwoValueHorizontal)
    {
        thumb (minPos);
        thumb (maxPos);
    }
    else
    {
        thumb (pos);
    }
}

void PocketLook::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hover, bool)
{
    auto r = b.getLocalBounds().toFloat();
    auto box = juce::Rectangle<float> (18, 18).withCentre ({ r.getX() + 11.0f, r.getCentreY() });

    g.setColour (pick (0xff0b1421, 0xff1d1d1d, 0xffffffff));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (pick (hover ? 0xff4e78ff : 0xff34445b, hover ? 0xff777777 : 0xff555555, 0xffbabcc0));
    g.drawRoundedRectangle (box, 5.0f, 1.0f);

    if (b.getToggleState())
    {
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 4.5f, box.getCentreY());
        tick.lineTo (box.getCentreX() - 1.0f, box.getBottom() - 5.0f);
        tick.lineTo (box.getRight() - 4.0f, box.getY() + 5.0f);

        const auto accent = isNeon() ? juce::Colour (0xff5987ff) : ink();
        glowStroke (g, tick, accent, 2.0f, isNeon());
    }

    text (g, b.getButtonText(), r.withTrimmedLeft (26.0f), 13.0f, ink(), juce::Justification::centredLeft);
}

// ---------------------------------------------------------------------------
// ModernDial
// ---------------------------------------------------------------------------

ModernDial::ModernDial (PocketLook& l, juce::String t, juce::String sub, juce::String u, juce::uint32 a,
                        bool useCompact, int decimalPlaces)
    : look (l), title (t), subtitle (sub), unit (u), accent (a), compact (useCompact), decimals (decimalPlaces)
{
    setSliderStyle (juce::Slider::RotaryVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setName (t);
    setWantsKeyboardFocus (true);
}

void ModernDial::paint (juce::Graphics& g)
{
    const float design = compact ? 100.0f : 200.0f;
    const float s = float (getWidth()) / design;
    const float r = (compact ? 34.0f : 77.0f) * s;
    const float ring = r + (compact ? 5.0f : 8.0f) * s;

    const juce::Point<float> c (float (getWidth()) * 0.5f, float (getHeight()) * 0.5f);
    const auto face = juce::Rectangle<float> (2 * r, 2 * r).withCentre (c);

    if (look.isNeon())
        for (int i = compact ? 3 : 5; i > 0; --i)
        {
            g.setColour (juce::Colour (accent).withAlpha (0.035f));
            g.fillEllipse (face.expanded (float (i) * 2 * s));
        }

    if (look.isNeon())
        g.setGradientFill (juce::ColourGradient (look.pick (0xff233349, 0, 0), c.x - r, c.y - r,
                                                look.pick (0xff060b12, 0, 0), c.x + r, c.y + r, false));
    else
        g.setColour (look.pick (0, 0xff252525, 0xfff8f8f8));

    g.fillEllipse (face);
    g.setColour (look.pick (0xff344963, 0xff555555, 0xffbabcc0));
    g.drawEllipse (face, s);

    const float proportion = float (valueToProportionOfLength (getValue()));
    const float start = juce::MathConstants<float>::pi * 1.25f;
    const float end = start + juce::MathConstants<float>::pi * 1.5f * proportion;

    juce::Path track, arc;
    track.addCentredArc (c.x, c.y, ring, ring, 0, start, juce::MathConstants<float>::pi * 2.75f, true);
    stroke (g, track, look.pick (0xff050a11, 0xff101010, 0xffc7c8ca), (compact ? 4.0f : 6.0f) * s);

    if (proportion > 0.0f)
    {
        arc.addCentredArc (c.x, c.y, ring, ring, 0, start, end, true);
        const auto a = look.isNeon() ? juce::Colour (accent) : look.pick (0, 0xffe8e8e8, 0xff303235);

        if (look.isNeon())
        {
            stroke (g, arc, a.withAlpha (0.10f), (compact ? 8.0f : 14.0f) * s);
            stroke (g, arc, a.withAlpha (0.18f), (compact ? 6.0f : 9.0f) * s);
            g.setGradientFill (juce::ColourGradient (a.brighter (0.2f), c.x - r, c.y + r, juce::Colour (0xff8c86ed), c.x + r, c.y - r, false));
        }
        else
        {
            g.setColour (a);
        }

        g.strokePath (arc, juce::PathStrokeType ((compact ? 3.5f : 5.0f) * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const auto marker = juce::Point<float> (c.x + ring * std::sin (end), c.y - ring * std::cos (end));
    g.setColour (look.pick (0xffdeebff, 0xfff4f4f4, 0xff303030));
    g.fillEllipse (marker.x - (compact ? 3.5f : 5.5f) * s, marker.y - (compact ? 3.5f : 5.5f) * s,
                   (compact ? 7.0f : 11.0f) * s, (compact ? 7.0f : 11.0f) * s);

    juce::String value;
    if (unit == "Hz")
        value = hzLabel (getValue());
    else if (unit == "dB")
        value = juce::String (getValue(), decimals) + " dB";
    else
        value = juce::String (getValue(), decimals) + unit;

    if (compact)
    {
        text (g, title, { c.x - r, c.y - 22 * s, 2 * r, 15 * s }, 12 * s, look.ink(), juce::Justification::centred);
        text (g, value, { c.x - r, c.y - 7 * s, 2 * r, 23 * s }, 17 * s, look.ink(), juce::Justification::centred);
        text (g, subtitle, { c.x - r, c.y + 16 * s, 2 * r, 13 * s }, 10 * s, look.muted(), juce::Justification::centred);
    }
    else
    {
        text (g, title, { c.x - r, c.y - 46 * s, 2 * r, 25 * s }, 18 * s, look.ink(), juce::Justification::centred);
        text (g, value, { c.x - r, c.y - 19 * s, 2 * r, 43 * s }, 33 * s, look.ink(), juce::Justification::centred);
        text (g, subtitle, { c.x - r, c.y + 28 * s, 2 * r, 23 * s }, 13 * s, look.muted(), juce::Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

WidePocketAudioProcessorEditor::WidePocketAudioProcessorEditor (WidePocketAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "WidePocket";
    options.filenameSuffix = "settings";
    options.folderName = "RainlineMusic";
    options.osxLibrarySubFolder = "Application Support";
    preferences = std::make_unique<juce::PropertiesFile> (options);

    const auto saved = preferences->getValue ("widePocket.ui.theme", "neon");
    setTheme (saved == "solidDark" ? PocketTheme::SolidDark
                                   : (saved == "solidWhite" ? PocketTheme::SolidWhite : PocketTheme::Neon),
              false);

    setLookAndFeel (&look);
    setOpaque (true);
    setResizable (true, true);

    for (auto* c : std::initializer_list<juce::Component*> { &widthDial, &focusDial, &airDial, &stabilityDial,
                                                            &sibilanceDial, &transientDial, &outputDial,
                                                            &settingsButton, &bypassButton })
        addAndMakeVisible (c);

    widthAttach = std::make_unique<SliderAttachment> (p.parameters, "width", widthDial);
    focusAttach = std::make_unique<SliderAttachment> (p.parameters, "focus", focusDial);
    airAttach = std::make_unique<SliderAttachment> (p.parameters, "air", airDial);
    stabilityAttach = std::make_unique<SliderAttachment> (p.parameters, "stability", stabilityDial);
    sibilanceAttach = std::make_unique<SliderAttachment> (p.parameters, "sibilanceGuard", sibilanceDial);
    transientAttach = std::make_unique<SliderAttachment> (p.parameters, "transientFocus", transientDial);
    outputAttach = std::make_unique<SliderAttachment> (p.parameters, "output", outputDial);
    outputDial.setDoubleClickReturnValue (true, 0.0);

    bypassButton.setClickingTogglesState (true);
    bypassAttach = std::make_unique<ButtonAttachment> (p.parameters, "bypass", bypassButton);

    // The bypass overlay has to appear on the very click, with no ramp: the
    // previous version eased bypassMix in the timer, which read as lag.
    bypassButton.onClick = [this]
    {
        bypassTarget = bypassButton.getToggleState();

        if (bypassTarget)
            captureBlurSnapshot();
        else
            blurredSnapshot = juce::Image();

        bypassMix = bypassTarget ? 1.0f : 0.0f;
        repaint();
    };

    settingsButton.onClick = [this] { showSettingsMenu(); };

    widthDial.setTooltip ("Stereo spread of the synthesised Side signal. The mono sum never changes.");
    focusDial.setTooltip ("Keeps the intelligibility range of the voice centred while the rest spreads.");
    airDial.setTooltip ("Extra width in the top octaves, as a pure band gain, so the image cannot move.");
    stabilityDial.setTooltip ("How quickly the width follows the voice. Higher is calmer.");
    sibilanceDial.setTooltip ("Pulls the width back on s and t sounds, which is where widening gets harsh.");
    transientDial.setTooltip ("Pulls the width back on plosives and syllable attacks.");
    outputDial.setTooltip ("Output gain. Double-click resets to 0 dB.");
    bypassButton.setTooltip ("Enable / bypass processing");
    settingsButton.setTooltip ("Settings");

    int width = p.editorWidth.load();
    if (width < 800 || width > 1500)
        width = preferences->getIntValue ("widePocket.ui.width", 960);
    width = juce::jlimit (800, 1500, width);

    setResizeLimits (800, 530, 1500, 1400);
    getConstrainer()->setFixedAspectRatio (960.0 / 636.0);
    setSize (width, juce::roundToInt (width * 636.0 / 960.0));

    ready = true;
    p.editorWidth.store (width);

    WideTrace discard;
    while (p.popTrace (discard)) {}

    p.editorOpen.store (true);
    timerCallback();
    startTimerHz (60);
}

WidePocketAudioProcessorEditor::~WidePocketAudioProcessorEditor()
{
    stopTimer();
    saveSize();
    audioProcessor.editorOpen.store (false);
    setLookAndFeel (nullptr);
}

void WidePocketAudioProcessorEditor::saveSize()
{
    if (! ready || preferences == nullptr)
        return;

    audioProcessor.editorWidth.store (getWidth());
    preferences->setValue ("widePocket.ui.width", getWidth());
    preferences->saveIfNeeded();
    resizeStamp = 0.0;
}

void WidePocketAudioProcessorEditor::setTheme (PocketTheme t, bool persist)
{
    look.theme = t;

    if (persist && preferences != nullptr)
    {
        preferences->setValue ("widePocket.ui.theme", t == PocketTheme::Neon ? "neon"
                                                                            : (t == PocketTheme::SolidDark ? "solidDark" : "solidWhite"));
        preferences->saveIfNeeded();
    }

    repaint();
    for (auto* c : getChildren())
        c->repaint();

    if (bypassMix > 0.0f)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<WidePocketAudioProcessorEditor> (this)]
                                        {
                                            if (safe != nullptr)
                                                safe->captureBlurSnapshot();
                                        });
}

void WidePocketAudioProcessorEditor::showSettingsMenu()
{
    juce::PopupMenu root, theme;

    theme.addItem (201, "Neon", true, look.theme == PocketTheme::Neon);
    theme.addItem (202, "Solid Dark", true, look.theme == PocketTheme::SolidDark);
    theme.addItem (203, "Solid White", true, look.theme == PocketTheme::SolidWhite);

    root.addSubMenu ("Theme", theme);

    auto safe = juce::Component::SafePointer<WidePocketAudioProcessorEditor> (this);
    root.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (settingsButton),
                        [safe] (int id)
                        {
                            if (safe == nullptr || id == 0)
                                return;

                            if (id == 201)
                                safe->setTheme (PocketTheme::Neon);
                            else if (id == 202)
                                safe->setTheme (PocketTheme::SolidDark);
                            else if (id == 203)
                                safe->setTheme (PocketTheme::SolidWhite);
                        });
}

void WidePocketAudioProcessorEditor::captureBlurSnapshot()
{
    if (capturingBlur || blurArea.isEmpty())
        return;

    // Exactly the Phase Pocket recipe: snapshot without the overlay, shrink
    // to a sixth, blur that, and stretch it back. Cheap enough to run on the
    // click instead of a slow full resolution box blur.
    capturingBlur = true;
    const float previousMix = bypassMix;
    bypassMix = 0.0f;
    auto source = createComponentSnapshot (blurArea, true, 1.0f);
    bypassMix = previousMix;
    capturingBlur = false;

    if (! source.isValid() || source.getWidth() < 8 || source.getHeight() < 8)
        return;

    const int w = juce::jmax (16, source.getWidth() / 6);
    const int h = juce::jmax (16, source.getHeight() / 6);

    juce::Image small (juce::Image::ARGB, w, h, true);
    {
        juce::Graphics sg (small);
        sg.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        sg.drawImage (source, juce::Rectangle<float> (0, 0, float (w), float (h)), juce::RectanglePlacement::stretchToFit);
    }

    juce::Image soft (juce::Image::ARGB, w, h, true);
    juce::ImageConvolutionKernel kernel (9);
    kernel.createGaussianBlur (2.2f);
    kernel.applyToImage (soft, small, small.getBounds());

    blurredSnapshot = soft;
}

juce::Rectangle<int> WidePocketAudioProcessorEditor::scaled (float x, float y, float w, float h) const
{
    const float s = float (getWidth()) / 960.0f;
    return { juce::roundToInt (x * s), juce::roundToInt (y * s), juce::roundToInt (w * s), juce::roundToInt (h * s) };
}

void WidePocketAudioProcessorEditor::resized()
{
    settingsButton.setBounds (scaled (838, 22, 40, 40));
    bypassButton.setBounds (scaled (888, 22, 40, 40));

    // Right hand column, on the Phase Pocket grid: the two big dials inside
    // the single outlined panel, with the small Output between them.
    widthDial.setBounds (scaled (725, 104, 200, 220));
    focusDial.setBounds (scaled (725, 350, 200, 220));
    outputDial.setBounds (scaled (848, 293, 84, 84));
    outputDial.toFront (false);

    // Voice control row, centred inside the lower block that now starts
    // under the taller scope.
    airDial.setBounds (scaled (75, 444, 116, 116));
    stabilityDial.setBounds (scaled (227, 444, 116, 116));
    sibilanceDial.setBounds (scaled (379, 444, 116, 116));
    transientDial.setBounds (scaled (531, 444, 116, 116));

    blurArea = scaled (12, 82, 936, 542);
    blurredSnapshot = {};

    if (ready)
    {
        audioProcessor.editorWidth.store (getWidth());
        resizeStamp = juce::Time::getMillisecondCounterHiRes();
    }
}

void WidePocketAudioProcessorEditor::timerCallback()
{
    WideTrace trace;

    while (audioProcessor.popTrace (trace))
    {
        latest = trace;

        scatter[(std::size_t) scatterCursor] = { trace.left, trace.right };
        scatterCursor = (scatterCursor + 1) % (int) scatter.size();
        scatterFilled = juce::jmin ((int) scatter.size(), scatterFilled + 1);
    }

    const bool bypassed = audioProcessor.displayBypass.load (std::memory_order_relaxed)
                          || bypassButton.getToggleState();

    if (bypassed != bypassTarget)
    {
        bypassTarget = bypassed;

        if (bypassed)
            captureBlurSnapshot();
        else
            blurredSnapshot = juce::Image();
    }

    // No easing: the overlay is either on or off on the same frame as the
    // parameter change.
    const float target = bypassTarget ? 1.0f : 0.0f;
    if (bypassMix != target)
        bypassMix = target;

    if (resizeStamp > 0.0 && juce::Time::getMillisecondCounterHiRes() - resizeStamp > 600.0)
        saveSize();

    repaint();
}

void WidePocketAudioProcessorEditor::panel (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (look.isNeon())
    {
        for (int i = 4; i > 0; --i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.05f));
            g.fillRoundedRectangle (r.expanded (float (i)).translated (0, float (i) * 2), 16.0f + float (i));
        }

        g.setGradientFill (juce::ColourGradient (look.pick (0xff182538, 0, 0), r.getX(), r.getY(),
                                                look.pick (0xff060c14, 0, 0), r.getRight(), r.getBottom(), false));
    }
    else
    {
        g.setColour (look.pick (0, 0xff202020, 0xffffffff));
    }

    g.fillRoundedRectangle (r, 16.0f);
    g.setColour (look.pick (0xff304259, 0xff505050, 0xffbfc0c2));
    g.drawRoundedRectangle (r, 16.0f, 1.0f);
}

void WidePocketAudioProcessorEditor::vectorScope (juce::Graphics& g, juce::Rectangle<float> box)
{
    const bool glow = look.theme != PocketTheme::SolidWhite;
    const float textGlow = glow ? 0.075f : 0.0f;
    const float s = float (getWidth()) / 960.0f;

    g.setColour (look.pick (0xff050b13, 0xff111111, 0xffffffff));
    g.fillRoundedRectangle (box, 12.0f);

    const auto label = look.pick (0xffdce5fc, 0xffeeeeee, 0xff242527);
    const auto secondary = look.muted();
    const auto grid = look.pick (0xff223349, 0xff363636, 0xffdddddd);

    text (g, "STEREO IMAGE", { box.getX() + 18.0f * s, box.getY() + 12.0f * s, 260.0f * s, 24.0f * s }, 13.0f * s, label,
          juce::Justification::centredLeft, textGlow);

    auto field = box.withTrimmedTop (40.0f * s).reduced (26.0f * s, 16.0f * s);

    const float halfWidth = field.getWidth() * 0.5f;
    const float height = field.getHeight();
    const float centreX = field.getCentreX();
    const float baseY = field.getBottom();

    const auto domePoint = [centreX, baseY, halfWidth, height] (float angle, float magnitude)
    {
        return juce::Point<float> (centreX + std::sin (angle) * magnitude * halfWidth,
                                   baseY - std::cos (angle) * magnitude * height);
    };

    // Grid: three stretched arcs plus the L / C / R guide lines.
    for (const float magnitude : { 0.33f, 0.66f, 1.0f })
    {
        juce::Path arc;
        for (int step = 0; step <= 48; ++step)
        {
            const float angle = juce::MathConstants<float>::halfPi * (-1.0f + 2.0f * float (step) / 48.0f);
            const auto point = domePoint (angle, magnitude);

            if (step == 0)
                arc.startNewSubPath (point);
            else
                arc.lineTo (point);
        }

        g.setColour (grid.withAlpha (magnitude >= 1.0f ? 1.0f : 0.6f));
        g.strokePath (arc, juce::PathStrokeType (1.0f));
    }

    for (const float angle : { -juce::MathConstants<float>::halfPi,
                               -juce::MathConstants<float>::pi * 0.25f,
                               0.0f,
                               juce::MathConstants<float>::pi * 0.25f,
                               juce::MathConstants<float>::halfPi })
    {
        g.setColour (grid.withAlpha (0.55f));
        const auto point = domePoint (angle, 1.0f);
        g.drawLine (centreX, baseY, point.x, point.y, angle == 0.0f ? 0.9f : 0.6f);
    }

    text (g, "L", { field.getX() - 4.0f * s, baseY - 18.0f * s, 20.0f * s, 18.0f * s }, 11.0f * s, secondary,
          juce::Justification::centred, textGlow);
    text (g, "C", { centreX - 10.0f * s, field.getY() - 18.0f * s, 20.0f * s, 18.0f * s }, 11.0f * s, secondary,
          juce::Justification::centred, textGlow);
    text (g, "R", { field.getRight() - 16.0f * s, baseY - 18.0f * s, 20.0f * s, 18.0f * s }, 11.0f * s, secondary,
          juce::Justification::centred, textGlow);

    // Correlation scale on the right, as on the reference display.
    const auto scaleArea = juce::Rectangle<float> (box.getRight() - 22.0f * s, field.getY(), 20.0f * s, height);
    for (int i = 0; i <= 2; ++i)
        text (g, i == 0 ? "+1" : (i == 1 ? "0" : "-1"),
              { scaleArea.getX(), scaleArea.getY() + height * 0.5f * float (i) - 8.0f * s, 20.0f * s, 16.0f * s },
              9.5f * s, secondary, juce::Justification::centredRight, textGlow);

    const auto accent = look.isNeon() ? juce::Colour (0xff32d4cb) : look.pick (0, 0xffd8d8d8, 0xff3a3c40);
    const int count = scatterFilled;

    const auto indexOf = [this, count] (int i)
    {
        return (std::size_t) ((scatterCursor - count + i + (int) scatter.size()) % (int) scatter.size());
    };

    // Auto gain for the display only: the dome is always filled to the top,
    // so a quiet take is just as readable as a loud one. The peak of the
    // visible history sets the scale, never the absolute level.
    float peak = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        const auto point = scatter[indexOf (i)];
        const float mid = (point.x + point.y) * 0.7071f;
        const float side = (point.y - point.x) * 0.7071f;
        peak = juce::jmax (peak, std::sqrt (mid * mid + side * side));
    }

    const float normalisation = peak > 1.0e-5f ? 1.0f / peak : 1.0f;

    for (int i = 0; i < count; ++i)
    {
        const auto point = scatter[indexOf (i)];

        const float mid = (point.x + point.y) * 0.7071f;

        // Left heavy material must sit on the left. The previous version used
        // (L - R), which mirrored the display against every other meter.
        const float side = (point.y - point.x) * 0.7071f;

        const float magnitude = juce::jlimit (0.0f, 1.0f, std::sqrt (mid * mid + side * side) * normalisation);
        const float angle = std::atan2 (side, std::abs (mid));
        const auto position = domePoint (angle, magnitude);

        // Newest points are brightest, which is what makes fast movement
        // readable without slowing the display down.
        const float age = float (i) / float (juce::jmax (1, count));
        g.setColour (accent.withAlpha (0.05f + 0.55f * age * age));
        g.fillEllipse (position.x - 1.0f, position.y - 1.0f, 2.0f, 2.0f);
    }

    const float correlation = juce::jlimit (-1.0f, 1.0f, latest.correlation);
    g.setColour (accent.withAlpha (0.9f));
    g.fillEllipse (scaleArea.getX() - 6.0f * s,
                   scaleArea.getY() + height * 0.5f * (1.0f - correlation) - 2.0f,
                   4.0f * s, 4.0f * s);
}

void WidePocketAudioProcessorEditor::paint (juce::Graphics& g)
{
    const float s = float (getWidth()) / 960.0f;

    g.fillAll (look.pick (0xff060b12, 0xff171717, 0xfff1f1f1));

    // Header on the Phase Pocket grid: the same 39 px centred product name
    // and the same thin separator underneath it.
    text (g, "WIDE POCKET", scaled (180, 14, 600, 54).toFloat(), 39.0f * s, look.ink(),
          juce::Justification::centred);

    g.setColour (look.pick (0xff263347, 0xff444444, 0xffc5c6c8));
    g.fillRect (scaled (24, 78, 912, 1).toFloat());

    // The only outlined panel sits around the two big dials on the right.
    panel (g, scaled (714, 94, 222, 480).toFloat());

    // Stereo image, 30 percent taller than before so the wide dome no longer
    // looks stretched.
    vectorScope (g, scaled (24, 94, 674, 299).toFloat());

    // Lower block: the four voice control dials, drawn like the scope rather
    // than as a second outlined panel.
    {
        const auto box = scaled (24, 407, 674, 167).toFloat();

        g.setColour (look.pick (0xff050b13, 0xff111111, 0xffffffff));
        g.fillRoundedRectangle (box, 12.0f);

        text (g, "VOICE CONTROL", { box.getX() + 18.0f * s, box.getY() + 12.0f * s, 260.0f * s, 24.0f * s }, 13.0f * s,
              look.ink(), juce::Justification::centredLeft, look.isNeon() ? 0.075f : 0.0f);
    }
}

void WidePocketAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    if (capturingBlur || bypassMix < 0.5f || ! blurredSnapshot.isValid())
        return;

    // Same overlay as Phase Pocket, including the wording and the position.
    juce::Graphics::ScopedSaveState save (g);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (blurredSnapshot, blurArea.toFloat(), juce::RectanglePlacement::stretchToFit);

    const bool dark = look.theme != PocketTheme::SolidWhite;

    g.setColour ((dark ? juce::Colours::black : juce::Colours::white).withAlpha (0.18f));
    g.fillRoundedRectangle (blurArea.toFloat(), 12.0f);

    const auto centre = blurArea.toFloat().translated (0, -float (blurArea.getHeight()) * 0.05f);
    const float size = juce::jmax (34.0f, float (getWidth()) / 18.0f);
    const auto ink = dark ? juce::Colours::white : juce::Colour (0xff202124);

    g.setColour ((dark ? juce::Colours::black : juce::Colours::white).withAlpha (0.55f));
    g.setFont (uiFont (size));
    g.drawText ("BYPASSED", centre.translated (0, 2), juce::Justification::centred);

    text (g, "BYPASSED", centre, size, ink, juce::Justification::centred, dark ? 0.1f : 0.0f);
}
