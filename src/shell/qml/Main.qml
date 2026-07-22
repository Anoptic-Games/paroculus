import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Paroculus

// The production window frame: a menu bar, a tab bar over the open set, the
// canvas bound to the active workspace, and the surfaces that project it. Every
// mutation still dispatches through the workspace's run()/invokeAction entrance,
// and every read binds a projection — no QML calls a Session directly. The heavy
// regions are components — LeftDock, RightDock, StripView, CommandPalette, Toast,
// StatusBarView and the registry-projected menus — that this frame composes and
// wires; what is here is the layout skeleton, the file lifecycle they need and
// the connections between them.
ApplicationWindow {
    id: root
    width: 1100
    height: 720
    visible: true
    color: Theme.windowBg
    title: (App.active ? App.active.title : "paroculus") +
           (App.active && App.active.dirty ? " •" : "") + " — paroculus"

    // A file:// url to a local path. Linux is the verified platform; a QUrl-based
    // path would generalise it, and is the seam to reach for when the others are.
    function pathOf(url) { return decodeURIComponent(url.toString().replace(/^file:\/\//, "")) }

    // Save routes to Save As when the document has no path yet, exactly as the
    // spec fixes: a fast save with no path goes straight to the chooser.
    function doSave() {
        if (App.activeNeedsPath()) { saveDialog.closeAfterSave = false; saveDialog.open() }
        else App.saveActiveTo(App.activePath())
    }

    // Closing a dirty tab is the one legitimate modal beyond file choosing:
    // imminent data loss. A clean tab closes with no prompt. The closing tab is
    // made active first, so the unsaved prompt and its Save operate on the tab
    // being closed rather than on whatever tab happened to be active — a middle-
    // click on a non-active dirty tab must not save-and-close the wrong document.
    property int pendingCloseIndex: -1
    function requestClose(index) {
        var ws = App.tabs[index]
        if (ws && ws.dirty) {
            App.activeIndex = index
            pendingCloseIndex = index
            unsavedDialog.open()
        } else {
            App.closeWorkspace(index)
        }
    }

    // Quit and window-close route through here so imminent loss gets exactly one
    // honest prompt. forceQuit is the second-pass flag: the prompt's Discard sets
    // it and closes again, and the guard then lets the close through.
    property bool forceQuit: false
    function anyDirty() {
        for (var i = 0; i < App.tabs.length; i++) if (App.tabs[i].dirty) return true
        return false
    }
    onClosing: function(close) {
        if (!forceQuit && anyDirty()) { close.accepted = false; quitDialog.open() }
    }

    // ---- Modal moments: file choosing and imminent loss, nothing else ----
    FileDialog {
        id: openDialog
        title: qsTr("Open document")
        nameFilters: ["Paroculus documents (*.paro)", "All files (*)"]
        currentFolder: "file://" + App.defaultDirectory()
        onAccepted: App.openFile(root.pathOf(selectedFile))
    }
    FileDialog {
        id: saveDialog
        title: qsTr("Save document")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "paro"
        nameFilters: ["Paroculus documents (*.paro)", "All files (*)"]
        currentFolder: "file://" + App.defaultDirectory()
        // Set when the save is the first half of a close-with-save on a no-path
        // document, so the tab is closed only after the write actually lands.
        property bool closeAfterSave: false
        onAccepted: {
            var ok = App.saveActiveTo(root.pathOf(selectedFile))
            if (ok && closeAfterSave) App.closeWorkspace(root.pendingCloseIndex)
            closeAfterSave = false
        }
        onRejected: closeAfterSave = false  // a cancelled Save-As keeps the tab
    }
    Dialog {
        id: unsavedDialog
        title: qsTr("Unsaved changes")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        Label {
            text: qsTr("This document has unsaved changes. Save before closing?")
            color: Theme.textBright
        }
        // The closing tab is active (requestClose activated it), so Save acts on
        // it. A no-path document routes to Save As, which closes on success; a
        // pathed one saves and closes only if the write landed — a failed save
        // never discards the document it could not write.
        onAccepted: {
            if (App.activeNeedsPath()) { saveDialog.closeAfterSave = true; saveDialog.open() }
            else if (App.saveActiveTo(App.activePath())) App.closeWorkspace(root.pendingCloseIndex)
        }
        onDiscarded: { App.closeWorkspace(root.pendingCloseIndex); unsavedDialog.close() }
    }

    // The quit prompt: one honest question for any number of dirty tabs. Discard
    // sets the second-pass flag and closes again; Cancel leaves everything.
    Dialog {
        id: quitDialog
        title: qsTr("Unsaved changes")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label {
            text: qsTr("One or more documents have unsaved changes. Quit and lose them?")
            color: Theme.textBright
        }
        onDiscarded: { root.forceQuit = true; quitDialog.close(); root.close() }
    }

    // Diagnostics are never modal: a refused load surfaces as a toast, per the
    // no-silent-changes-is-no-interruption policy. The full reports panel is a
    // later U0 step; the toast is the notice half.
    Connections {
        target: App
        function onOpenFailed(path, message, line) {
            toast.show(qsTr("Could not open: %1").arg(message) + (line > 0 ? (" (line " + line + ")") : ""))
        }
        function onSaveFailed(path) { toast.show(qsTr("Save failed: %1").arg(path)) }
    }

    // The report-shaped fragments the status string used to carry — deletion
    // counts, structure reports, drops — flash as a toast the moment they land.
    // The reports model is the memory; this is the notice. Re-targets on the
    // active tab, since only the active document is being edited.
    Connections {
        target: App.active
        function onReportPosted(text) { toast.show(text) }
    }

    // ---- Menu bar ----
    // File and View mix app-level chords (new/open/save, zoom, palette) that touch
    // no session with the registry-projected editing menus. The app rows stay
    // shell-handled behind the registry's None tier, exactly today's Ctrl+P
    // mechanism made policy.
    menuBar: MenuBar {
        Menu {
            title: qsTr("File")
            Action { text: qsTr("New Tab"); shortcut: "Ctrl+N"; onTriggered: App.newWorkspace() }
            Action { text: qsTr("Open…"); shortcut: "Ctrl+O"; onTriggered: openDialog.open() }
            MenuSeparator {}
            Action { text: qsTr("Save"); shortcut: "Ctrl+S"; onTriggered: root.doSave() }
            Action { text: qsTr("Save As…"); shortcut: "Ctrl+Shift+S"; onTriggered: { saveDialog.closeAfterSave = false; saveDialog.open() } }
            MenuSeparator {}
            Action { text: qsTr("Close Tab"); shortcut: "Ctrl+W"; onTriggered: root.requestClose(App.activeIndex) }
            Action { text: qsTr("Quit"); shortcut: "Ctrl+Q"; onTriggered: root.close() }
        }
        ProjectedMenu { title: qsTr("Edit"); categories: ["edit", "inference"] }
        ProjectedMenu { title: qsTr("Draw"); categories: ["tool"] }
        ProjectedMenu { title: qsTr("Constrain"); categories: ["relation"] }
        ProjectedMenu { title: qsTr("Arrange"); categories: ["transform", "group", "layer", "tag"] }
        ProjectedMenu { title: qsTr("Region"); categories: ["region", "export"] }
        Menu {
            title: qsTr("View")
            Action { text: qsTr("Zoom In"); shortcut: "Ctrl++"; onTriggered: sketch.zoomIn() }
            Action { text: qsTr("Zoom Out"); shortcut: "Ctrl+-"; onTriggered: sketch.zoomOut() }
            Action { text: qsTr("Reset View / Fit"); shortcut: "Ctrl+0"; onTriggered: sketch.resetView() }
            MenuSeparator {}
            Menu {
                title: qsTr("Panels")
                MenuItem { text: qsTr("Layers"); checkable: true; checked: rightDock.layersVisible; onTriggered: rightDock.layersVisible = !rightDock.layersVisible }
                MenuItem { text: qsTr("Inspector"); checkable: true; checked: rightDock.inspectorVisible; onTriggered: rightDock.inspectorVisible = !rightDock.inspectorVisible }
                MenuItem { text: qsTr("Parameters"); checkable: true; checked: rightDock.parametersVisible; onTriggered: rightDock.parametersVisible = !rightDock.parametersVisible }
                MenuItem { text: qsTr("Reports"); checkable: true; checked: rightDock.reportsVisible; onTriggered: rightDock.reportsVisible = !rightDock.reportsVisible }
                MenuItem { text: qsTr("History"); checkable: true; checked: rightDock.historyVisible; onTriggered: rightDock.historyVisible = !rightDock.historyVisible }
                MenuSeparator {}
                MenuItem { text: qsTr("Bottom Dock"); checkable: true; checked: rightDock.bottomDockVisible; onTriggered: rightDock.bottomDockVisible = !rightDock.bottomDockVisible }
            }
            MenuItem { text: qsTr("Lock Panels"); checkable: true; checked: rightDock.locked; onTriggered: rightDock.locked = !rightDock.locked }
            MenuItem { text: qsTr("Reset Panels"); onTriggered: rightDock.resetPanels() }
            MenuSeparator {}
            Action { text: qsTr("Command Palette…"); shortcut: "Ctrl+P"; onTriggered: commandPalette.visible ? commandPalette.close() : commandPalette.open() }
        }
    }

    // Ctrl+Tab cycles the active tab. Kept as an application shortcut behind the
    // registry's None tier, the same mechanism as the palette chord.
    Shortcut { sequence: "Ctrl+Tab"; onActivated: App.cycleActive(1) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: App.cycleActive(-1) }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Tab bar ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 34
            color: Theme.dockBg
            Row {
                anchors { left: parent.left; leftMargin: 6; verticalCenter: parent.verticalCenter }
                spacing: 2
                Repeater {
                    model: App.tabs
                    delegate: Rectangle {
                        required property int index
                        required property var modelData
                        height: 26
                        width: tabRow.width + 24
                        radius: 4
                        color: index === App.activeIndex ? Theme.surfaceRaised : (tabMouse.containsMouse ? Theme.hover : "transparent")
                        Row {
                            id: tabRow
                            anchors.centerIn: parent
                            spacing: 6
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: index === App.activeIndex ? Theme.textBright : Theme.textSecondary
                                font.pixelSize: 12
                                text: (modelData.dirty ? "• " : "") + modelData.title
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: closeMouse.containsMouse ? Theme.textBright : Theme.textDim
                                font.pixelSize: 13
                                text: "×"
                                MouseArea {
                                    id: closeMouse
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    onClicked: root.requestClose(index)
                                }
                            }
                        }
                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                            onClicked: (mouse) => {
                                if (mouse.button === Qt.MiddleButton) root.requestClose(index)
                                else App.activeIndex = index
                            }
                        }
                    }
                }
                Rectangle {
                    height: 26; width: 26; radius: 4
                    color: newMouse.containsMouse ? Theme.hover : "transparent"
                    Text { anchors.centerIn: parent; color: Theme.textSecondary; font.pixelSize: 16; text: "+" }
                    MouseArea { id: newMouse; anchors.fill: parent; hoverEnabled: true; onClicked: App.newWorkspace() }
                }
            }
        }

        // ---- Toolbar, canvas and its overlays ----
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Left dock: the tools toolbar over the constraints toolbar. Its
            // hover verdict is drawn on the canvas below, bound to constraintHint.
            LeftDock { id: leftDock }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                SketchView {
                    id: sketch
                    anchors.fill: parent
                    workspace: App.active
                    focus: !commandPalette.visible
                }

                // Tool options row: the active tool's name, its live parameters,
                // and the numeric field in flight — the tool fragment the status
                // string used to carry, now a chip near the work. The numeric
                // entry widget proper (scrubbable, Tab-cycling) lands in a later
                // step; this is its read-only home.
                Rectangle {
                    id: toolOptions
                    visible: App.active && App.active.toolName.length > 0
                    anchors { left: parent.left; top: parent.top; margins: 12 }
                    width: toolOptionsRow.width + 20
                    height: 28
                    radius: 5
                    color: Theme.surface
                    border.color: Theme.border
                    Row {
                        id: toolOptionsRow
                        anchors.centerIn: parent
                        spacing: 12
                        Text {
                            color: Theme.textPrimary; font.pixelSize: 12
                            text: App.active ? App.active.toolName : ""
                        }
                        Repeater {
                            model: App.active ? App.active.toolParameters : []
                            delegate: Text {
                                required property var modelData
                                color: Theme.textSecondary; font.pixelSize: 11; font.family: "monospace"
                                text: modelData.name + " " + modelData.value.toFixed(2)
                            }
                        }
                        Text {
                            visible: App.active && App.active.numericEntry.active
                            color: Theme.textBright; font.pixelSize: 12; font.family: "monospace"
                            text: (App.active ? App.active.numericEntry.name : "") + " [" +
                                  (App.active ? App.active.numericEntry.text : "") + "_]"
                        }
                    }
                }

                // The style toolbar, floating at the canvas top, shown only when
                // the selection has something styleable. Writes through the same
                // style actions the inspector's Style section does.
                StyleBar {
                    anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 12 }
                }

                // The transient strip, near the work; hidden while the palette is up.
                StripView { paletteOpen: commandPalette.visible }

                // The constraints toolbar's hover verdict, in words, beside the
                // ghost it arms on the canvas — the same preview path the strip
                // reads, sourced from the left dock's constraintHint.
                Text {
                    id: constraintPreview
                    visible: text.length > 0
                    anchors { left: parent.left; leftMargin: 16; bottom: parent.bottom; bottomMargin: 64 }
                    color: Theme.textMuted
                    font.pixelSize: 11
                    text: leftDock.constraintHint
                }

                // The command palette: everything, in the table's own order,
                // searched by subsequence. Focus returns to the canvas on close.
                CommandPalette {
                    id: commandPalette
                    onClosed: sketch.forceActiveFocus()
                }

                // The latest report as a transient toast near the canvas. The
                // panel is the memory; this is the notice.
                Toast { id: toast }
            }

            // ---- Right dock ----
            // Layers, Inspector and Reports panels, the lock and the layout
            // persistence. Fixed zone for U0; the whole dock hides when every
            // panel is closed. Its lock and bottom-zone visibility are what the
            // View menu and the bottom dock below read.
            RightDock { id: rightDock }
        }

        // ---- Bottom dock ----
        // Reserved from day one and uncluttered by default: where a timeline
        // lands, full-width under the canvas, without evicting established
        // furniture. Empty at U0; its visibility lives with the layout in the
        // right dock.
        Rectangle {
            id: bottomDock
            visible: rightDock.bottomDockVisible
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            color: Theme.dockBg
            Text {
                anchors.centerIn: parent
                color: Theme.borderStrong; font.pixelSize: 11
                text: qsTr("bottom dock (reserved)")
            }
        }

        // ---- Status bar ----
        StatusBarView {}
    }
}
