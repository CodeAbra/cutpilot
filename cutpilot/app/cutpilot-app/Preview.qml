import QtQuick
import CutPilot.Render

// The preview surface fills its panel; all state arrives through the
// controller on the C++ side.
PreviewItem {
    objectName: "preview"
    anchors.fill: parent
}
