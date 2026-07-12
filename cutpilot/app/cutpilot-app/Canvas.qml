import QtQuick
import CutPilot.Render

Item {
    id: root
    anchors.fill: parent
    focus: true

    // One world camera shared by every layer, so the grid and the nodes can never
    // drift apart under pan and zoom.
    CanvasController {
        id: camera
    }

    // The GPU-drawn dotted grid fills the area and reads the shared camera.
    CanvasItem {
        objectName: "grid"
        anchors.fill: parent
        controller: camera
    }

    // The node layer is stacked over the grid, owns pointer and keyboard input, and
    // writes the shared camera. It carries focus so Space-pan and zoom-reset reach it.
    NodeLayerItem {
        objectName: "nodes"
        anchors.fill: parent
        controller: camera
        focus: true
    }
}
