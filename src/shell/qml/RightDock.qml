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
    property alias parametersVisible: parametersPanel.visible
    property alias historyVisible: historyPanel.visible
    readonly property bool anyOpen: layersPanel.visible || inspectorPanel.visible
                                    || reportsPanel.visible || parametersPanel.visible
                                    || historyPanel.visible

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
        inspectorPanel.visible = true
        inspectorPanel.collapsed = false
        // Reports, parameters and history are closed by default per the spec's
        // layout: the toast is the notice, the panel is the memory opened when
        // wanted, so the canvas keeps the room.
        reportsPanel.visible = false
        reportsPanel.collapsed = false
        parametersPanel.visible = false
        parametersPanel.collapsed = false
        historyPanel.visible = false
        historyPanel.collapsed = false
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
            parameters: [parametersPanel.visible, parametersPanel.collapsed],
            history: [historyPanel.visible, historyPanel.collapsed],
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
                // Added in U1, so an older saved layout may not carry them.
                if (o.parameters) { parametersPanel.visible = o.parameters[0]; parametersPanel.collapsed = o.parameters[1] }
                if (o.history) { historyPanel.visible = o.history[0]; historyPanel.collapsed = o.history[1] }
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
                    spacing: 2
                    Repeater {
                        // Real layers only. The implicit null base layer has no
                        // record and so no row — activating, renaming, hiding or
                        // locking it would all route through layer id 0, which the
                        // action layer reads as "the selection's layer" rather than
                        // the base. It is named below when no real layer exists so
                        // the panel is never blank.
                        model: App.active ? App.active.layers : []
                        delegate: Rectangle {
                            required property var modelData
                            width: parent.width
                            height: 22
                            radius: 3
                            // A highlight for the active layer, the layer new
                            // geometry lands on.
                            color: modelData.active ? Theme.activeBg : "transparent"
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.leftMargin: 4; anchors.rightMargin: 4
                                spacing: 6
                                Text {  // the visible toggle, projecting layer.hide/show
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: modelData.visible ? Theme.info : Theme.textDim
                                    font.pixelSize: 10; font.family: "monospace"
                                    text: modelData.visible ? qsTr("vis") : qsTr("hid")
                                    MouseArea {
                                        anchors.fill: parent; anchors.margins: -3
                                        onClicked: App.active.run(
                                            modelData.visible ? "layer.hide" : "layer.show",
                                            { "layer": modelData.id })
                                    }
                                }
                                Text {  // the lock toggle, projecting layer.lock/unlock
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: modelData.locked ? Theme.warn : Theme.textDim
                                    font.pixelSize: 10; font.family: "monospace"
                                    text: modelData.locked ? qsTr("lk") : qsTr("—")
                                    MouseArea {
                                        anchors.fill: parent; anchors.margins: -3
                                        onClicked: App.active.run(
                                            modelData.locked ? "layer.unlock" : "layer.lock",
                                            { "layer": modelData.id })
                                    }
                                }
                                Item {
                                    width: parent.width - 120
                                    height: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text {
                                        visible: !renameField.visible
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width; elide: Text.ElideRight
                                        color: modelData.visible ? Theme.textPrimary : Theme.textDim
                                        font.pixelSize: 12
                                        font.bold: modelData.active
                                        text: modelData.name.length > 0 ? modelData.name : qsTr("base")
                                        MouseArea {
                                            anchors.fill: parent
                                            // Click to activate. The base has no
                                            // record to name, and a locked layer
                                            // cannot take new geometry, so activate
                                            // is offered on neither — the row
                                            // projects its own lock, which is what
                                            // the session refuses on.
                                            onClicked: if (modelData.id !== 0 && !modelData.locked)
                                                App.active.run("layer.activate", { "layer": modelData.id })
                                            // Double-click to rename. The base layer
                                            // has no record to name, so it is left.
                                            onDoubleClicked: {
                                                if (modelData.id === 0) return
                                                renameField.text = modelData.name
                                                renameField.visible = true
                                                renameField.forceActiveFocus()
                                                renameField.selectAll()
                                            }
                                        }
                                    }
                                    TextField {
                                        id: renameField
                                        visible: false
                                        anchors.fill: parent
                                        font.pixelSize: 12
                                        onAccepted: {
                                            App.active.run("layer.rename",
                                                { "layer": modelData.id, "name": text })
                                            visible = false
                                        }
                                        onActiveFocusChanged: if (!activeFocus) visible = false
                                    }
                                }
                                Text {  // the entity-count badge
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.textDim; font.pixelSize: 10
                                    text: modelData.count
                                }
                            }
                        }
                    }
                    // The implicit base layer has no record and so no row of its
                    // own — it is the layer every document has without one being
                    // created. Named when no real layer exists so the panel is not
                    // empty on a fresh document.
                    Text {
                        visible: !App.active || App.active.layers.length === 0
                        color: Theme.textDim; font.pixelSize: 11
                        text: qsTr("base layer")
                    }
                    Text {
                        color: Theme.info; font.pixelSize: 11
                        text: qsTr("+ new layer")
                        MouseArea {
                            anchors.fill: parent; anchors.margins: -3
                            onClicked: App.active.run("layer.new", {})
                        }
                    }
                }
            }

            Panel {
                id: inspectorPanel
                title: qsTr("Inspector")
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                Column {
                    width: parent.width
                    spacing: 8

                    // Selection — the signature line.
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted; font.pixelSize: 11
                        text: App.active ? App.active.selectionText : ""
                    }

                    // Rectangle size — width and height driving the tag's slots
                    // through the checked tag.set-width / set-height path.
                    Repeater {
                        model: App.active ? App.active.rectanglePanels : []
                        delegate: Row {
                            required property var modelData
                            spacing: 6
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.textSecondary; font.pixelSize: 11; text: qsTr("size")
                            }
                            SlotField {
                                width: 64
                                label: modelData.widthDriven ? "w" : "w?"
                                value: modelData.width.toFixed(2)
                                onCommitted: App.active.run("tag.set-width",
                                    { "tag": modelData.tag, "value": parseFloat(text) })
                            }
                            SlotField {
                                width: 64
                                label: modelData.heightDriven ? "h" : "h?"
                                value: modelData.height.toFixed(2)
                                onCommitted: App.active.run("tag.set-height",
                                    { "tag": modelData.tag, "value": parseFloat(text) })
                            }
                        }
                    }

                    // Relations — the full constraint list, unbudgeted. Driving
                    // plain, reference bracketed, live; status flags tinted.
                    Column {
                        width: parent.width
                        spacing: 1
                        visible: App.active && App.active.relations.length > 0
                        Text {
                            color: Theme.textDim; font.pixelSize: 10
                            text: qsTr("RELATIONS")
                        }
                        Repeater {
                            model: App.active ? App.active.relations : []
                            delegate: Rectangle {
                                required property var modelData
                                width: parent.width
                                height: relationRow.implicitHeight + 4
                                radius: 2
                                color: rowHover.hovered ? Theme.hover : "transparent"
                                // Hovering the row lights up the relation's
                                // geometry on the canvas — what it points to,
                                // by eye. Repaint-only, so the list under the
                                // cursor is not rebuilt out from under the hover.
                                HoverHandler {
                                    id: rowHover
                                    onHoveredChanged: hovered
                                        ? App.active.setHoveredRelation(modelData.id)
                                        : App.active.clearHoveredRelation()
                                }
                                Row {
                                    id: relationRow
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 4; x: 2
                                    spacing: 6
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 132
                                        elide: Text.ElideRight
                                        color: modelData.conflicting ? Theme.warn
                                             : modelData.frozen ? Theme.textDim
                                             : Theme.textPrimary
                                        font.pixelSize: 11
                                        text: modelData.kind
                                              + (modelData.operands.length > 0 ? " " + modelData.operands : "")
                                              + (modelData.redundant ? " ·redundant" : "")
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: App.active.selectRelation(modelData.id, false)
                                        }
                                    }
                                    // The value, editable through checked set-value.
                                    // A reference is marked rather than bracketed
                                    // so the field stays a plain number to edit.
                                    SlotField {
                                        visible: modelData.valued
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 74
                                        label: modelData.driving ? "" : "ref"
                                        value: modelData.value.toFixed(1)
                                        // Select the relation first: set-value's
                                        // applicability is selection-based (a valued
                                        // constraint selected), so editing the field
                                        // selects the row it edits — which is also
                                        // what highlights the glyph on the canvas.
                                        // A non-finite parse is dropped rather than
                                        // driven into the solve.
                                        onCommitted: {
                                            var v = parseFloat(text)
                                            if (isNaN(v)) return
                                            App.active.selectRelation(modelData.id)
                                            App.active.run("relation.set-value", { "value": v })
                                        }
                                    }
                                    Text {  // driving / reference toggle
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: Theme.textDim; font.pixelSize: 10; text: "D"
                                        MouseArea {
                                            anchors.fill: parent; anchors.margins: -3
                                            onClicked: {
                                                App.active.selectRelation(modelData.id)
                                                App.active.run("relation.toggle-driving", {})
                                            }
                                        }
                                    }
                                    Text {  // flip alternative, when the kind has one
                                        visible: modelData.hasAlternative
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: Theme.textDim; font.pixelSize: 10; text: "⇄"
                                        MouseArea {
                                            anchors.fill: parent; anchors.margins: -3
                                            // Selection-based applicability, so
                                            // select the row before flipping it.
                                            onClicked: {
                                                App.active.selectRelation(modelData.id)
                                                App.active.run("relation.flip-alternative", {})
                                            }
                                        }
                                    }
                                    Text {  // delete
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: Theme.textDim; font.pixelSize: 11; text: "×"
                                        MouseArea {
                                            anchors.fill: parent; anchors.margins: -3
                                            // Selects then deletes. Like every
                                            // relation row action this is a recall
                                            // surface: it edits the live document
                                            // through the selection, which a script
                                            // records as the click on the canvas
                                            // glyph, not as the panel button — the
                                            // recordable delete is select-glyph +
                                            // Delete.
                                            onClicked: {
                                                App.active.selectRelation(modelData.id)
                                                App.active.deleteSelection()
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Axes — the reference each axis relation the selection
                    // carries points at, and the retargets the model offers. The
                    // reference is shown for every axis relation; each retarget
                    // button is offered only when its own action is applicable, so
                    // a row that reads runnable is runnable — the trustworthiness
                    // the whole table keeps. Two targets land here: a new cluster
                    // frame (doc-framed relations) and back to the document frame
                    // (clustered relations). Retarget to an existing named frame
                    // needs a frame picker and is deferred.
                    Column {
                        width: parent.width
                        spacing: 1
                        visible: App.active && App.active.axisReferences.length > 0
                        // Whether each retarget action applies to the current
                        // selection right now — the same predicates the menu dims by.
                        function applies(name) {
                            if (!App.active) return false
                            var rows = App.active.actions.filter(function(a) { return a.name === name })
                            return rows.length > 0 && rows[0].applicable
                        }
                        readonly property bool canNewFrame: applies("relation.retarget-axes")
                        readonly property bool canToDocument: applies("relation.retarget-to-document")
                        Text {
                            color: Theme.textDim; font.pixelSize: 10
                            text: qsTr("AXES")
                        }
                        Repeater {
                            model: App.active ? App.active.axisReferences : []
                            delegate: Text {
                                required property var modelData
                                width: parent.width; elide: Text.ElideRight
                                color: Theme.textSecondary; font.pixelSize: 11
                                text: modelData.kind + " → " + modelData.frameName
                            }
                        }
                        Text {
                            visible: parent.canNewFrame
                            color: Theme.info; font.pixelSize: 11
                            text: qsTr("→ new cluster frame")
                            MouseArea {
                                anchors.fill: parent; anchors.margins: -3
                                onClicked: App.active.run("relation.retarget-axes", {})
                            }
                        }
                        Text {
                            visible: parent.canToDocument
                            color: Theme.info; font.pixelSize: 11
                            text: qsTr("→ document frame")
                            MouseArea {
                                anchors.fill: parent; anchors.margins: -3
                                onClicked: App.active.run("relation.retarget-to-document", {})
                            }
                        }
                    }

                    // Style — the selection's resolved appearance, forking on a
                    // shared edit exactly as the style toolbar does.
                    StyleSection { visible: App.active && App.active.appearance.any }
                }
            }

            Panel {
                id: reportsPanel
                title: qsTr("Reports")
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                // The append-only memory of everything no-silent-changes logs,
                // the same model the toast flashes the newest entry of. An entry
                // that names records is click-to-select: clicking it lands the user
                // on the geometry or relations it is about. One that names none
                // (a deletion, an export) is plain text.
                ListView {
                    width: parent.width
                    height: Math.min(contentHeight, 180)
                    model: App.active ? App.active.reports : null
                    clip: true
                    delegate: Text {
                        required property var model
                        width: ListView.view.width
                        wrapMode: Text.WordWrap
                        color: model.selectable ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: 11
                        font.underline: model.selectable && entryHover.hovered
                        bottomPadding: 4
                        text: model.text
                        HoverHandler { id: entryHover; enabled: model.selectable }
                        MouseArea {
                            anchors.fill: parent
                            enabled: model.selectable
                            cursorShape: Qt.PointingHandCursor
                            onClicked: App.active.selectReported(model.entities, model.constraints)
                        }
                    }
                }
            }

            // Parameters — the payoff surface of the slot thread. A parameter
            // driving twenty widths is edited here, once. Closed by default.
            Panel {
                id: parametersPanel
                title: qsTr("Parameters")
                visible: false
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                Column {
                    width: parent.width
                    spacing: 2
                    Repeater {
                        model: App.active ? App.active.parameters : []
                        delegate: Row {
                            required property var modelData
                            property bool cycleWarn: false
                            width: parent.width
                            spacing: 6
                            Item {
                                width: 70; height: 18
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    id: pName
                                    visible: !pRename.visible
                                    anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    color: Theme.textPrimary; font.pixelSize: 12
                                    text: modelData.name
                                    MouseArea {
                                        anchors.fill: parent
                                        onDoubleClicked: {
                                            pRename.text = modelData.name; pRename.visible = true
                                            pRename.forceActiveFocus(); pRename.selectAll()
                                        }
                                    }
                                }
                                TextField {
                                    id: pRename; visible: false; anchors.fill: parent
                                    font.pixelSize: 12
                                    onAccepted: {
                                        App.active.run("parameter.rename", { "id": modelData.id, "name": text })
                                        visible = false
                                    }
                                    onActiveFocusChanged: if (!activeFocus) visible = false
                                }
                            }
                            SlotField {
                                width: 96
                                anchors.verticalCenter: parent.verticalCenter
                                // The expression text, editable. A parameter value
                                // is always passed as an expression — the parser
                                // reads a bare number as a constant — so references
                                // to other parameters and arithmetic both commit.
                                value: modelData.evaluable ? modelData.expression : qsTr("unevaluable")
                                onCommitted: {
                                    // wouldCycle refused inline, before the commit,
                                    // not after: the check the panel owes the user.
                                    if (App.active.parameterWouldCycle(modelData.id, text)) {
                                        parent.cycleWarn = true
                                    } else {
                                        parent.cycleWarn = false
                                        App.active.run("parameter.set", { "id": modelData.id, "expression": text })
                                    }
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: parent.cycleWarn ? Theme.warn : Theme.textDim
                                font.pixelSize: 9
                                text: parent.cycleWarn ? qsTr("cycle") : ("×" + modelData.usage)
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.textDim; font.pixelSize: 11; text: "×"
                                MouseArea {
                                    anchors.fill: parent; anchors.margins: -3
                                    // Freeze semantics: deleting states that
                                    // referring slots freeze to their value.
                                    onClicked: App.active.run("parameter.delete", { "id": modelData.id })
                                }
                            }
                        }
                    }
                    Text {
                        color: Theme.info; font.pixelSize: 11
                        text: qsTr("+ new parameter")
                        MouseArea {
                            anchors.fill: parent; anchors.margins: -3
                            onClicked: App.active.run("parameter.create", { "name": "param", "value": 0 })
                        }
                    }
                }
            }

            // History — the undo journal as a list, click to walk to a position
            // through the ordinary undo/redo path so branch fidelity holds.
            Panel {
                id: historyPanel
                title: qsTr("History")
                visible: false
                locked: rightDock.locked
                onVisibleChanged: rightDock.savePanelLayout()
                onCollapsedChanged: rightDock.savePanelLayout()
                Column {
                    width: parent.width
                    spacing: 1
                    Text {
                        color: (App.active && App.active.historyPosition === 0) ? Theme.textBright : Theme.textDim
                        font.pixelSize: 11; text: qsTr("○ start")
                        MouseArea { anchors.fill: parent; anchors.margins: -2
                            onClicked: App.active.walkHistory(0) }
                    }
                    Repeater {
                        model: App.active ? App.active.history : []
                        delegate: Text {
                            required property var modelData
                            required property int index
                            width: parent.width; elide: Text.ElideRight
                            // Applied steps are below the cursor; undone ones above
                            // it are dimmed, exactly as the journal's depth divides.
                            color: (App.active && index < App.active.historyPosition)
                                   ? Theme.textPrimary : Theme.textDim
                            font.pixelSize: 11
                            text: (index + 1) + ". " + modelData
                            MouseArea { anchors.fill: parent; anchors.margins: -2
                                onClicked: App.active.walkHistory(index + 1) }
                        }
                    }
                }
            }
        }
    }
}
