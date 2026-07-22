import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Paroculus

// The right dock: Layers, Inspector and Reports panels, drag-resizable in width
// and scrollable in height, plus the panel-layout persistence. Owns the lock
// (locked) and the reserved bottom zone's visibility (bottomDockVisible); the
// frame's View menu drives those and the per-panel visibility aliases, and the
// frame's bottom dock binds its visibility here.
//
// The dock collapses to nothing when every panel is closed, so the canvas is the
// largest thing on screen at every default. It does so by shrinking its width,
// never by toggling its own visibility: a Layout that hides itself does not give
// a re-shown child its size back, so a reopened panel would stay a true visible
// property with no pixels.
//
// Vertically the opaque region is only as tall as the enabled panels stack — the
// space below them shows the canvas — and the stack scrolls when it outgrows the
// viewport, which real panel content routinely will. Horizontally the width is
// the user's, dragged from the left edge, defaulting to a quarter of the space
// the dock shares with the canvas.
Item {
    id: rightDock
    property bool locked: false
    property bool bottomDockVisible: false
    property alias layersVisible: layersPanel.visible
    property alias inspectorVisible: inspectorPanel.visible
    property alias reportsVisible: reportsPanel.visible
    readonly property bool anyOpen: layersPanel.visible || inspectorPanel.visible
                                    || reportsPanel.visible

    // Drag-resizable width, defaulting to a quarter of the row the dock shares
    // with the canvas. A binding until the user drags, then a fixed value; the
    // clamp keeps it from vanishing or swallowing the canvas.
    property real dockWidth: Math.round((parent ? parent.width : 900) * 0.25)
    readonly property real minWidth: 180
    readonly property real maxWidth: parent ? parent.width * 0.6 : 600

    Layout.fillHeight: true
    Layout.preferredWidth: anyOpen ? Math.max(minWidth, Math.min(maxWidth, dockWidth)) : 0
    clip: true

    // The default layout, in code, so reset works with no settings file at all.
    function resetPanels() {
        layersPanel.visible = true
        layersPanel.collapsed = false
        inspectorPanel.visible = false
        inspectorPanel.collapsed = false
        reportsPanel.visible = true
        reportsPanel.collapsed = false
        bottomDockVisible = false
        locked = false
        dockWidth = Qt.binding(function() { return Math.round((parent ? parent.width : 900) * 0.25) })
        savePanelLayout()
    }

    // The panel arrangement — zone membership is fixed at U0, so a layout is which
    // panels are open, which are collapsed, the dock width, the reserved bottom
    // zone, and the window lock — persisted to application settings and restored
    // on open. Named multi-layouts are the deferred half; the schema here is one
    // implicit "default" arrangement, versioned by the settings store.
    property bool layoutReady: false
    function savePanelLayout() {
        if (!AppSettings || !layoutReady) return
        AppSettings.saveLayout("default", JSON.stringify({
            layers: [layersPanel.visible, layersPanel.collapsed],
            inspector: [inspectorPanel.visible, inspectorPanel.collapsed],
            reports: [reportsPanel.visible, reportsPanel.collapsed],
            width: dockWidth,
            bottom: bottomDockVisible,
            lock: locked
        }))
    }
    function loadPanelLayout() {
        var raw = AppSettings ? AppSettings.layout("default") : ""
        if (raw) {
            try {
                var o = JSON.parse(raw)
                layersPanel.visible = o.layers[0]; layersPanel.collapsed = o.layers[1]
                inspectorPanel.visible = o.inspector[0]; inspectorPanel.collapsed = o.inspector[1]
                reportsPanel.visible = o.reports[0]; reportsPanel.collapsed = o.reports[1]
                if (o.width) dockWidth = o.width
                bottomDockVisible = o.bottom
                locked = o.lock
            } catch (e) { /* a malformed layout costs the arrangement, nothing more */ }
        }
        layoutReady = true
    }
    Component.onCompleted: loadPanelLayout()
    onLockedChanged: savePanelLayout()
    onBottomDockVisibleChanged: savePanelLayout()

    // The resize grip on the dock's inner edge. Frozen with the panels under the
    // window lock, and gone when the dock is collapsed. A drag on it widens the
    // dock as it moves toward the canvas, the width saved once the drag ends
    // rather than per pixel.
    Rectangle {
        id: resizeHandle
        width: 6
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        visible: rightDock.anyOpen && !rightDock.locked
        color: (hover.hovered || drag.active) ? Theme.borderStrong : "transparent"
        HoverHandler { id: hover; cursorShape: Qt.SizeHorCursor }
        DragHandler {
            id: drag
            target: null
            yAxis.enabled: false
            property real pressSceneX: 0
            property real pressWidth: 0
            onActiveChanged: {
                if (active) { pressSceneX = centroid.scenePressPosition.x; pressWidth = rightDock.dockWidth }
                else rightDock.savePanelLayout()
            }
            onCentroidChanged: {
                if (!active) return
                // The dock is on the right, so moving the grip left widens it.
                var w = pressWidth - (centroid.scenePosition.x - pressSceneX)
                rightDock.dockWidth = Math.max(rightDock.minWidth, Math.min(rightDock.maxWidth, w))
            }
        }
    }

    // The panels, stacked and vertically scrolled. The column hugs its content —
    // no filler pushing it to full height — so the opaque region is the enabled
    // panels' cumulative span and the canvas shows below it; when the span
    // outgrows the viewport the column scrolls rather than the dock growing.
    ScrollView {
        id: scroller
        anchors {
            left: resizeHandle.right; right: parent.right
            top: parent.top; bottom: parent.bottom; margins: 8
        }
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            width: scroller.availableWidth
            spacing: 8

            Panel {
                id: layersPanel
                title: qsTr("Layers")
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                Column {
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: App.active ? App.active.layers : []
                        delegate: Row {
                            required property var modelData
                            spacing: 8
                            Text {
                                color: modelData.visible ? Theme.info : Theme.textDim
                                font.pixelSize: 11
                                font.family: "monospace"
                                text: modelData.visible ? qsTr("shown") : qsTr("hidden")
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    onClicked: App.active.run(
                                        modelData.visible ? "layer.hide" : "layer.show",
                                        { "layer": modelData.id })
                                }
                            }
                            Text {
                                color: modelData.locked ? Theme.warn : Theme.textDim
                                font.pixelSize: 11
                                font.family: "monospace"
                                text: modelData.locked ? qsTr("locked") : qsTr("open")
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    onClicked: App.active.run(
                                        modelData.locked ? "layer.unlock" : "layer.lock",
                                        { "layer": modelData.id })
                                }
                            }
                            Text {
                                color: modelData.visible ? Theme.textPrimary : Theme.textDim
                                font.pixelSize: 12
                                text: modelData.name
                            }
                        }
                    }
                    Text {
                        visible: !App.active || App.active.layers.length === 0
                        color: Theme.textDim; font.pixelSize: 11
                        text: qsTr("base layer only")
                    }
                }
            }

            Panel {
                id: inspectorPanel
                title: qsTr("Inspector")
                visible: false  // closed by default; the deep sections land in U1
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted; font.pixelSize: 11
                    text: App.active ? App.active.selectionText : ""
                }
            }

            Panel {
                id: reportsPanel
                title: qsTr("Reports")
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                // The append-only memory of everything no-silent-changes logs,
                // the same model the toast flashes the newest entry of.
                ListView {
                    width: parent.width
                    height: Math.min(contentHeight, 180)
                    model: App.active ? App.active.reports : null
                    clip: true
                    delegate: Text {
                        required property var model
                        width: ListView.view.width
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary; font.pixelSize: 11
                        bottomPadding: 4
                        text: model.text
                    }
                }
            }
        }
    }
}
