import QtQuick
import QtQuick.Layouts
import Paroculus

// The right dock: Layers, Inspector and Reports panels, plus the panel-layout
// persistence. Fixed zone for U0; each panel collapses and closes, and the
// window lock freezes the affordances. Owns the lock (locked) and the reserved
// bottom zone's visibility (bottomDockVisible); the frame's View menu drives
// those and the per-panel visibility aliases, and the frame's bottom dock binds
// its visibility here. The dock hides when every panel is closed, so the canvas
// is the largest thing on screen at every default.
ColumnLayout {
    id: rightDock
    property bool locked: false
    property bool bottomDockVisible: false
    property alias layersVisible: layersPanel.visible
    property alias inspectorVisible: inspectorPanel.visible
    property alias reportsVisible: reportsPanel.visible
    Layout.fillHeight: true
    Layout.preferredWidth: 224
    Layout.margins: 8
    spacing: 8
    visible: layersPanel.visible || inspectorPanel.visible || reportsPanel.visible

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
    }

    // The panel arrangement — zone membership is fixed at U0, so a layout is which
    // panels are open, which are collapsed, the reserved bottom zone, and the
    // window lock — persisted to application settings and restored on open. Named
    // multi-layouts are the deferred half; the schema here is one implicit
    // "default" arrangement, versioned by the settings store.
    property bool layoutReady: false
    function savePanelLayout() {
        if (!AppSettings || !layoutReady) return
        AppSettings.saveLayout("default", JSON.stringify({
            layers: [layersPanel.visible, layersPanel.collapsed],
            inspector: [inspectorPanel.visible, inspectorPanel.collapsed],
            reports: [reportsPanel.visible, reportsPanel.collapsed],
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
                bottomDockVisible = o.bottom
                locked = o.lock
            } catch (e) { /* a malformed layout costs the arrangement, nothing more */ }
        }
        layoutReady = true
    }
    Component.onCompleted: loadPanelLayout()
    onLockedChanged: savePanelLayout()
    onBottomDockVisibleChanged: savePanelLayout()

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

    Item { Layout.fillHeight: true }  // pushes the panels to the top
}
