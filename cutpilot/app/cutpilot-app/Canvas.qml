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
        id: nodesLayer
        objectName: "nodes"
        anchors.fill: parent
        controller: camera
        focus: true
    }

    // The board overview floats over the canvas's bottom-right corner, reading
    // the same node model and camera the main canvas uses.
    MinimapItem {
        objectName: "minimap"
        width: 220
        height: 150
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 12
        anchors.bottomMargin: 12
        nodeLayer: nodesLayer
        controller: camera
    }
}
