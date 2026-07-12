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

    // Numeric readout text (zoom percentage, and later cost, resolution, timecode).
    QColor textPrimary() const { return m_textPrimary; }
    QColor textSecondary() const { return m_textSecondary; }

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

    // Typed-port dot colors. A port is never identified by color alone, but color is
    // the at-a-glance cue for what a connection carries.
    QColor typeImage() const { return m_typeImage; }
    QColor typeVideo() const { return m_typeVideo; }
    QColor typeAudio() const { return m_typeAudio; }
    QColor typeText() const { return m_typeText; }
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
    QColor m_textPrimary;
    QColor m_textSecondary;
    QColor m_nodeBody;
    QColor m_nodeHeader;
    QColor m_selection;
    QColor m_glowEmphasis;
    QColor m_selectionFill;
    QColor m_emphasis;
    QColor m_typeImage;
    QColor m_typeVideo;
    QColor m_typeAudio;
    QColor m_typeText;
    QColor m_typeControl;
    QColor m_typeAny;
};

} // namespace cutpilot::theme
