#pragma once

#include <QColor>
#include <QString>

namespace cutpilot::theme {

// The set of themes the application ships. Dark is the default for a media tool.
enum class Theme {
    Dark,
    Light,
    DarkDim
};

// Resolved values for every named design token a surface may read. Each surface,
// whether a Qt widget or the GPU canvas, reads from one table so chrome and canvas
// can never drift. Only the canvas surface, grid tint, and subtle border are resolved
// so far; the table is structured to grow token-by-token from here.
class ThemeTable {
public:
    explicit ThemeTable(Theme theme = Theme::Dark);

    Theme theme() const { return m_theme; }
    void setTheme(Theme theme);

    // The working media surface a mode draws on: the darkest neutral so media sits
    // in a neutral frame.
    QColor bgCanvas() const { return m_bgCanvas; }

    // The dotted-grid tint on the canvas. Subtle by design: a faint neutral that
    // reads as structure without competing with content.
    QColor gridDot() const { return m_gridDot; }

    // The brighter grid dot drawn at the major pitch.
    QColor gridDotMajor() const { return m_gridDotMajor; }

    // Hairline separators and group dividers.
    QColor borderSubtle() const { return m_borderSubtle; }

    // Control outline at rest (fields, buttons, cards).
    QColor borderDefault() const { return m_borderDefault; }

    // Hovered dividers and hovered control outlines.
    QColor borderStrong() const { return m_borderStrong; }

    // The keyboard focus ring — the one colored element in the chrome.
    QColor borderFocus() const { return m_borderFocus; }

    // Numeric readout text (zoom percentage, and later cost, resolution, timecode).
    QColor textPrimary() const { return m_textPrimary; }
    QColor textSecondary() const { return m_textSecondary; }

    // Hints, placeholders, and metadata — labels only, never body text.
    QColor textTertiary() const { return m_textTertiary; }
    QColor textDisabled() const { return m_textDisabled; }

    // Text or icon sitting on an emphasis fill (active tab, primary button).
    QColor textOnEmphasis() const { return m_textOnEmphasis; }

    // The chrome surface ramp: window base, panels, raised chrome (toolbar,
    // mode switcher), sunken wells, the hover and pressed washes, and floating
    // overlays (menus, popovers, panels).
    QColor surface0() const { return m_surface0; }
    QColor surface1() const { return m_surface1; }
    QColor surface2() const { return m_surface2; }
    QColor surface3() const { return m_surface3; }
    QColor surfaceHover() const { return m_surfaceHover; }
    QColor surfaceActive() const { return m_surfaceActive; }
    QColor surfaceOverlay() const { return m_surfaceOverlay; }

    // Emphasis fill states and the low-emphasis neutral wash.
    QColor emphasisHover() const { return m_emphasisHover; }
    QColor emphasisActive() const { return m_emphasisActive; }
    QColor emphasisMuted() const { return m_emphasisMuted; }

    // Neutral information: cached/reused results, the sync readout.
    QColor statusInfo() const { return m_statusInfo; }

    // Node card surfaces: the resting body and the slim header strip over it.
    QColor nodeBody() const { return m_nodeBody; }
    QColor nodeHeader() const { return m_nodeHeader; }

    // The neutral 2px selection outline drawn on a selected object, and the neutral
    // luminance halo that lifts a selected card off the surface.
    QColor selection() const { return m_selection; }
    QColor glowEmphasis() const { return m_glowEmphasis; }

    // The neutral low-alpha band fill under the marquee.
    QColor selectionFill() const { return m_selectionFill; }

    // The light-neutral hairline colour drawn for alignment guides.
    QColor emphasis() const { return m_emphasis; }

    // Run-state colors: the progress fill and status text of a generating node,
    // the settled done tick, a failed run, and the add-a-key warning. Paired with
    // a status word in the node's status line, never color alone.
    QColor statusRunning() const { return m_statusRunning; }
    QColor statusDone() const { return m_statusDone; }
    QColor statusError() const { return m_statusError; }
    QColor statusWarning() const { return m_statusWarning; }

    // Typed-port dot colors. A port is never identified by color alone, but color is
    // the at-a-glance cue for what a connection carries.
    QColor typeImage() const { return m_typeImage; }
    QColor typeMask() const { return m_typeMask; }
    QColor typeVideo() const { return m_typeVideo; }
    QColor typeAudio() const { return m_typeAudio; }
    QColor typeText() const { return m_typeText; }
    QColor typeNumber() const { return m_typeNumber; }
    QColor typeControl() const { return m_typeControl; }
    QColor typeAny() const { return m_typeAny; }

private:
    void resolve();

    Theme m_theme;
    QColor m_bgCanvas;
    QColor m_gridDot;
    QColor m_gridDotMajor;
    QColor m_borderSubtle;
    QColor m_borderDefault;
    QColor m_borderStrong;
    QColor m_borderFocus;
    QColor m_textPrimary;
    QColor m_textSecondary;
    QColor m_textTertiary;
    QColor m_textDisabled;
    QColor m_textOnEmphasis;
    QColor m_surface0;
    QColor m_surface1;
    QColor m_surface2;
    QColor m_surface3;
    QColor m_surfaceHover;
    QColor m_surfaceActive;
    QColor m_surfaceOverlay;
    QColor m_emphasisHover;
    QColor m_emphasisActive;
    QColor m_emphasisMuted;
    QColor m_statusInfo;
    QColor m_nodeBody;
    QColor m_nodeHeader;
    QColor m_selection;
    QColor m_glowEmphasis;
    QColor m_selectionFill;
    QColor m_emphasis;
    QColor m_statusRunning;
    QColor m_statusDone;
    QColor m_statusError;
    QColor m_statusWarning;
    QColor m_typeImage;
    QColor m_typeMask;
    QColor m_typeVideo;
    QColor m_typeAudio;
    QColor m_typeText;
    QColor m_typeNumber;
    QColor m_typeControl;
    QColor m_typeAny;
};

} // namespace cutpilot::theme
