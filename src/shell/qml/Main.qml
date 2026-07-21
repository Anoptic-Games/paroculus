import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Paroculus

// The production window frame: a menu bar, a tab bar over the open set, the
// canvas bound to the active workspace, and the surfaces that project it. Every
// mutation still dispatches through the workspace's run()/invokeAction entrance,
// and every read binds a projection — no QML calls a Session directly. The
// registry-projected menus and toolbars, the dockable panel host and the theme
// tokens land on top of this frame in the following U0 steps; what is here is the
// scaffolding they hang from and the file lifecycle they need.
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
    // The window lock: freezes panel geometry by hiding the collapse/close
    // affordances, which is the whole of the feature.
    property bool panelsLocked: false
    function anyDirty() {
        for (var i = 0; i < App.tabs.length; i++) if (App.tabs[i].dirty) return true
        return false
    }
    onClosing: function(close) {
        if (!forceQuit && anyDirty()) { close.accepted = false; quitDialog.open() }
    }

    // The registry rows in the given categories, live. Every menu and toolbar is
    // a filter of this one projection — the registry table — so no surface holds
    // a second list to drift, and applicability re-dims as the selection changes.
    function actionsIn(cats) {
        if (!App.active) return []
        return App.active.actions.filter(function(a) { return cats.indexOf(a.category) >= 0 })
    }

    // The default layout, in code, so reset works with no settings file at all.
    function resetPanels() {
        layersPanel.visible = true
        layersPanel.collapsed = false
        inspectorPanel.visible = false
        inspectorPanel.collapsed = false
        reportsPanel.visible = true
        reportsPanel.collapsed = false
        bottomDock.visible = false
        root.panelsLocked = false
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
            bottom: bottomDock.visible,
            lock: root.panelsLocked
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
                bottomDock.visible = o.bottom
                root.panelsLocked = o.lock
            } catch (e) { /* a malformed layout costs the arrangement, nothing more */ }
        }
        layoutReady = true
    }
    Component.onCompleted: loadPanelLayout()
    onPanelsLockedChanged: savePanelLayout()

    // The driving-strength imposition rows in a taxonomy family, for the
    // constraints toolbar. strength 1 is Impose; the other two strengths reach
    // the surface through the right-click flyout, not a row each.
    function imposeActionsInFamily(fam) {
        if (!App.active) return []
        return App.active.actions.filter(function(a) {
            return a.generated && a.strength === 1 && a.family === fam
        })
    }

    // The driving/reference toggle and the conflict walker, which the constraints
    // toolbar also carries beneath the imposition families, dimmed by their own
    // predicates. Named rather than category-filtered so distribute and mirror
    // (also "relation") stay in the Arrange menu where they belong.
    function toolbarRelationActions() {
        if (!App.active) return []
        return App.active.actions.filter(function(a) {
            return a.name === "relation.toggle-driving" || a.name === "relation.walk-conflicts"
        })
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

    // A dockable panel: a title bar with a collapse chevron and a close, and a
    // content body. The panel host is a contract, not a library commitment — the
    // zones, order, collapse, close and lock are fixed here, so a docking
    // dependency could replace this implementation without touching any panel.
    // Fixed zones for U0 (drag-reorder is the deferred half); the window lock
    // freezes the affordances, which is the whole of the lock feature.
    component Panel: Rectangle {
        id: panelRoot
        property string title: ""
        property bool collapsed: false
        property bool locked: false
        default property alias body: bodyContainer.data
        Layout.fillWidth: true
        Layout.preferredHeight: header.height + (collapsed ? 0 : bodyContainer.childrenRect.height + 14)
        color: Theme.panelBg
        border.color: Theme.surfaceRaised
        radius: 4
        clip: true

        Rectangle {
            id: header
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 26
            color: Theme.headerBg
            Text {
                // Frozen with the close under the window lock: collapse changes
                // panel geometry, and the lock freezes all of it, which is the
                // whole of the feature.
                visible: !panelRoot.locked
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                color: Theme.textMuted
                font.pixelSize: 10
                text: panelRoot.collapsed ? ">" : "v"
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: panelRoot.collapsed = !panelRoot.collapsed
                }
            }
            Text {
                anchors { left: parent.left; leftMargin: 24; verticalCenter: parent.verticalCenter }
                text: panelRoot.title
                color: Theme.textPrimary
                font.pixelSize: 12
            }
            Text {
                // No close affordance while the window lock is on.
                visible: !panelRoot.locked
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                text: "×"
                color: closeArea.containsMouse ? Theme.textBright : Theme.textDim
                font.pixelSize: 13
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    onClicked: panelRoot.visible = false
                }
            }
        }
        Item {
            id: bodyContainer
            visible: !panelRoot.collapsed
            anchors { left: parent.left; right: parent.right; top: header.bottom }
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: 7
            height: childrenRect.height
        }
    }

    // A registry-projected submenu: one MenuItem per row in the given categories,
    // titled, bindings shown, dimmed where inapplicable. The menu is a view of the
    // registry table, never a second list.
    component ProjectedMenu: Menu {
        id: projected
        property var categories: []
        Repeater {
            // `categories` resolves up the scope chain to the ProjectedMenu; the
            // Repeater's `parent` is the Menu's internal content view, which has
            // no such property — reading it there leaves every menu empty.
            model: root.actionsIn(projected.categories)
            delegate: MenuItem {
                required property var modelData
                text: modelData.title + (modelData.binding.length > 0 ? "   " + modelData.binding : "")
                // A parameterized row is dimmed rather than run with no value: the
                // numeric-entry-pending flow that supplies it lands with the
                // numeric-entry widget in a later step, and a menu row that runs
                // and silently no-ops breaks the property that an applicable
                // action runs — the one that makes the whole table trustworthy.
                enabled: modelData.applicable && !modelData.needsValue
                onTriggered: App.active.run(modelData.name, {})
            }
        }
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
                MenuItem { text: qsTr("Layers"); checkable: true; checked: layersPanel.visible; onTriggered: layersPanel.visible = !layersPanel.visible }
                MenuItem { text: qsTr("Inspector"); checkable: true; checked: inspectorPanel.visible; onTriggered: inspectorPanel.visible = !inspectorPanel.visible }
                MenuItem { text: qsTr("Reports"); checkable: true; checked: reportsPanel.visible; onTriggered: reportsPanel.visible = !reportsPanel.visible }
                MenuSeparator {}
                MenuItem { text: qsTr("Bottom Dock"); checkable: true; checked: bottomDock.visible; onTriggered: bottomDock.visible = !bottomDock.visible }
            }
            MenuItem { text: qsTr("Lock Panels"); checkable: true; checked: root.panelsLocked; onTriggered: root.panelsLocked = !root.panelsLocked }
            MenuItem { text: qsTr("Reset Panels"); onTriggered: root.resetPanels() }
            MenuSeparator {}
            Action { text: qsTr("Command Palette…"); shortcut: "Ctrl+P"; onTriggered: commandPalette.visible ? commandPalette.close() : commandPalette.open() }
        }
    }

    // Ctrl+Tab cycles the active tab. Kept as an application shortcut behind the
    // registry's None tier, the same mechanism as the palette chord.
    Shortcut { sequence: "Ctrl+Tab"; onActivated: App.cycleActive(1) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: App.cycleActive(-1) }

    // The three-strengths flyout for a constraints-toolbar button, opened on
    // right-click. The impose row's name plus the strength suffix reaches the
    // reference and measure-once variants; the flyout order never changes, since
    // strength is a property of the click and never a sticky mode.
    Menu {
        id: strengthMenu
        property string actionBase: ""
        property string actionTitle: ""
        MenuItem {
            text: qsTr("Impose (driving)")
            onTriggered: App.active.run(strengthMenu.actionBase, {})
        }
        MenuItem {
            text: qsTr("Add as reference measurement")
            onTriggered: App.active.run(strengthMenu.actionBase + ".reference", {})
        }
        MenuItem {
            text: qsTr("Measure once")
            onTriggered: App.active.run(strengthMenu.actionBase + ".measure", {})
        }
    }

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

            // Left dock: the tools toolbar over the constraints toolbar, both
            // projections of the one registry table — the tools by category, the
            // constraints as the imposition rows grouped by taxonomy family.
            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 64
                color: Theme.dockBg
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    // Tools: the five tool actions, the active tool held down,
                    // tooltips carrying name, binding and description.
                    Column {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 8
                        spacing: 6
                        Repeater {
                            model: root.actionsIn(["tool"])
                            delegate: Rectangle {
                                required property var modelData
                                width: 34
                                height: 34
                                radius: 5
                                property bool active: App.active && App.active.activeToolAction === modelData.name
                                color: active ? Theme.activeBg : (toolBtnMouse.containsMouse ? Theme.surfaceRaised : "transparent")
                                Text {
                                    anchors.centerIn: parent
                                    color: modelData.applicable ? (active ? Theme.textBright : Theme.textSecondary) : Theme.textFaint
                                    font.pixelSize: 13
                                    text: modelData.binding.length > 0 ? modelData.binding.toUpperCase() : modelData.title.charAt(0)
                                }
                                MouseArea {
                                    id: toolBtnMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: modelData.applicable
                                    onClicked: App.active.run(modelData.name, {})
                                    ToolTip.visible: containsMouse
                                    ToolTip.text: modelData.title +
                                        (modelData.binding.length > 0 ? " (" + modelData.binding + ")" : "") +
                                        "\n" + modelData.description
                                }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.preferredHeight: 1; color: Theme.surfaceRaised }

                    // Constraints toolbar: the imposition rows grouped by family in
                    // fixed order. A click imposes at driving strength; hover ghosts
                    // the imposition through the same preview path the strip uses,
                    // with its verdict; right-click opens the three-strengths
                    // flyout. Each button's enablement is canImpose for its kind,
                    // the same predicate the strip and palette read, so the three
                    // surfaces can never disagree.
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        Column {
                            width: 60
                            spacing: 5
                            Repeater {
                                model: ["placement", "direction", "size", "symmetry", "curve", "anchor"]
                                delegate: Column {
                                    required property string modelData
                                    width: 60
                                    spacing: 1
                                    Text {
                                        text: modelData
                                        color: Theme.textDim
                                        font.pixelSize: 8
                                        font.capitalization: Font.AllUppercase
                                        leftPadding: 5
                                    }
                                    Repeater {
                                        model: root.imposeActionsInFamily(modelData)
                                        delegate: Rectangle {
                                            required property var modelData
                                            width: 60
                                            height: 19
                                            radius: 3
                                            property bool on: modelData.applicable
                                            color: cMouse.containsMouse && on ? Theme.hoverStrong : "transparent"
                                            Text {
                                                anchors.fill: parent
                                                anchors.leftMargin: 6
                                                anchors.rightMargin: 3
                                                verticalAlignment: Text.AlignVCenter
                                                color: on ? Theme.textPrimary : Theme.textFaint
                                                font.pixelSize: 9
                                                elide: Text.ElideRight
                                                text: modelData.title
                                            }
                                            MouseArea {
                                                id: cMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                                enabled: on
                                                onEntered: constraintPreview.text = App.active.previewOf(modelData.name, 0)
                                                onExited: { constraintPreview.text = ""; App.active.clearPreview() }
                                                onClicked: (mouse) => {
                                                    if (mouse.button === Qt.RightButton) {
                                                        strengthMenu.actionBase = modelData.name
                                                        strengthMenu.actionTitle = modelData.title
                                                        strengthMenu.popup()
                                                    } else {
                                                        App.active.run(modelData.name, {})
                                                        constraintPreview.text = ""
                                                        App.active.clearPreview()
                                                    }
                                                }
                                                ToolTip.visible: containsMouse && on
                                                ToolTip.text: modelData.description
                                            }
                                        }
                                    }
                                }
                            }
                            // The driving/reference toggle and the conflict walker,
                            // beneath the imposition families, dimmed by predicate.
                            Repeater {
                                model: root.toolbarRelationActions()
                                delegate: Rectangle {
                                    required property var modelData
                                    width: 60
                                    height: 19
                                    radius: 3
                                    property bool on: modelData.applicable
                                    color: rMouse.containsMouse && on ? Theme.hoverStrong : "transparent"
                                    Text {
                                        anchors.fill: parent
                                        anchors.leftMargin: 6
                                        anchors.rightMargin: 3
                                        verticalAlignment: Text.AlignVCenter
                                        color: on ? Theme.textPrimary : Theme.textFaint
                                        font.pixelSize: 9
                                        elide: Text.ElideRight
                                        text: modelData.title
                                    }
                                    MouseArea {
                                        id: rMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: on
                                        onClicked: App.active.run(modelData.name, {})
                                        ToolTip.visible: containsMouse && on
                                        ToolTip.text: modelData.description
                                    }
                                }
                            }
                        }
                    }
                }
            }

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

            // The transient strip, near the work.
            Rectangle {
                id: strip
                visible: App.active && App.active.strip.length > 0 && !commandPalette.visible
                anchors { left: parent.left; leftMargin: 16; bottom: parent.bottom; bottomMargin: 16 }
                width: stripRow.width + 20
                height: 34
                radius: 5
                color: Theme.surface
                border.color: Theme.border
                Row {
                    id: stripRow
                    anchors.centerIn: parent
                    spacing: 6
                    Repeater {
                        model: App.active ? App.active.strip : []
                        delegate: Rectangle {
                            required property var modelData
                            width: entryLabel.width + 16
                            height: 24
                            radius: 3
                            color: entryMouse.containsMouse ? Theme.hoverStrong : "transparent"
                            Text {
                                id: entryLabel
                                anchors.centerIn: parent
                                color: entryMouse.containsMouse ? Theme.textBright : Theme.textSecondary
                                font.pixelSize: 12
                                text: modelData.title
                            }
                            MouseArea {
                                id: entryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onEntered: preview.text = App.active.previewOf(
                                    modelData.name,
                                    modelData.arguments.assignment !== undefined ? modelData.arguments.assignment : 0)
                                onExited: { preview.text = ""; App.active.clearPreview() }
                                onClicked: {
                                    App.active.run(modelData.name, modelData.arguments)
                                    preview.text = ""
                                    App.active.clearPreview()
                                }
                            }
                        }
                    }
                }
            }
            Text {
                id: preview
                visible: text.length > 0 && strip.visible
                anchors { left: strip.right; leftMargin: 12; verticalCenter: strip.verticalCenter }
                color: Theme.textMuted
                font.pixelSize: 11
            }

            // The constraints toolbar's hover verdict, in words, beside the ghost
            // it arms on the canvas — the same preview path the strip reads.
            Text {
                id: constraintPreview
                visible: text.length > 0
                anchors { left: parent.left; leftMargin: 16; bottom: parent.bottom; bottomMargin: 64 }
                color: Theme.textMuted
                font.pixelSize: 11
            }

            // The layers list moved into the Layers panel in the right dock.

            // The command palette: everything, in the table's own order, searched
            // by subsequence. Dims what does not apply rather than hiding it.
            Rectangle {
                id: commandPalette
                visible: false
                anchors.centerIn: parent
                width: 460
                height: 360
                radius: 6
                color: Theme.panelBg
                border.color: Theme.border
                function open() {
                    visible = true
                    query.text = ""
                    results.model = App.active ? App.active.palette("") : []
                    query.forceActiveFocus()
                }
                function close() { visible = false; sketch.forceActiveFocus() }
                TextField {
                    id: query
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
                    placeholderText: qsTr("search commands")
                    color: Theme.textBright
                    background: Rectangle { color: Theme.fieldBg; radius: 4 }
                    onTextChanged: results.model = App.active ? App.active.palette(text) : []
                    Keys.onEscapePressed: commandPalette.close()
                    Keys.onReturnPressed: {
                        if (results.count > 0) {
                            const first = results.model[0]
                            if (first.applicable) App.active.run(first.name, first.arguments)
                        }
                        commandPalette.close()
                    }
                }
                ListView {
                    id: results
                    anchors {
                        left: parent.left; right: parent.right
                        top: query.bottom; bottom: parent.bottom; margins: 10
                    }
                    clip: true
                    delegate: Item {
                        required property var modelData
                        width: results.width
                        height: 24
                        Text {
                            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                            color: modelData.applicable ? Theme.textBright : Theme.textDim
                            font.pixelSize: 12
                            text: modelData.title
                        }
                        Text {
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            color: Theme.textFaint
                            font.pixelSize: 10
                            font.family: "monospace"
                            text: modelData.name
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: modelData.applicable
                            onClicked: { App.active.run(modelData.name, modelData.arguments); commandPalette.close() }
                        }
                    }
                }
            }

            // The latest report as a transient toast near the canvas. The panel
            // is the memory; this is the notice.
            Rectangle {
                id: toast
                function show(message) { toastLabel.text = message; opacity = 1.0; toastTimer.restart() }
                anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 24 }
                width: toastLabel.width + 28
                height: 32
                radius: 6
                color: Theme.headerBg
                border.color: Theme.borderStrong
                opacity: 0.0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 220 } }
                Text { id: toastLabel; anchors.centerIn: parent; color: Theme.textBright; font.pixelSize: 12 }
                Timer { id: toastTimer; interval: 4000; onTriggered: toast.opacity = 0.0 }
            }
            }  // canvas Item

            // ---- Right dock ----
            // Fixed zone for U0; each panel collapses and closes, View ▸ Panels
            // re-opens, and the window lock freezes the affordances. The whole
            // dock hides when every panel is closed, so the canvas is the largest
            // thing on screen at every default.
            ColumnLayout {
                id: rightDock
                Layout.fillHeight: true
                Layout.preferredWidth: 224
                Layout.margins: 8
                spacing: 8
                visible: layersPanel.visible || inspectorPanel.visible || reportsPanel.visible

                Panel {
                    id: layersPanel
                    title: qsTr("Layers")
                    locked: root.panelsLocked
                    onVisibleChanged: root.savePanelLayout()
                    onCollapsedChanged: root.savePanelLayout()
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
                    locked: root.panelsLocked
                    onVisibleChanged: root.savePanelLayout()
                    onCollapsedChanged: root.savePanelLayout()
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
                    locked: root.panelsLocked
                    onVisibleChanged: root.savePanelLayout()
                    onCollapsedChanged: root.savePanelLayout()
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
        }  // toolbar+canvas RowLayout

        // ---- Bottom dock ----
        // Reserved from day one and uncluttered by default: where a timeline
        // lands, full-width under the canvas, without evicting established
        // furniture. Empty at U0.
        Rectangle {
            id: bottomDock
            visible: false
            onVisibleChanged: root.savePanelLayout()
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
        // The status mega-string is gone: each readout is its own typed
        // projection, the report-shaped fragments went to the reports model and
        // its toast, and the tool fragment to the tool options row.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 30
            color: Theme.panelBg

            // Left: the selection ladder, the dof, and the calm drag notes.
            Row {
                anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                spacing: 18
                Text {
                    color: Theme.textPrimary; font.pixelSize: 12
                    text: App.active ? App.active.selectionText : ""
                }
                Text {
                    // Displayed calmly: under-constraint is the normal state.
                    color: App.active && App.active.dof > 0 ? Theme.info : Theme.textMuted
                    font.pixelSize: 12; font.family: "monospace"
                    text: App.active ? (App.active.dof >= 0 ? qsTr("%1 dof").arg(App.active.dof) : qsTr("unsolved")) : ""
                }
                Text {
                    visible: App.active && App.active.saturated
                    color: Theme.warnAlt; font.pixelSize: 12
                    text: qsTr("resisting %1").arg(App.active ? App.active.resisting : 0)
                }
                Text {
                    visible: App.active && App.active.rippledOffScreen
                    color: Theme.warnAlt; font.pixelSize: 12
                    text: qsTr("moved off screen")
                }
                Text {
                    // The calm busy glyph while a component solves off-thread.
                    visible: App.active && App.active.busy
                    color: Theme.warn; font.pixelSize: 12
                    text: qsTr("solving…")
                }
            }

            // Right: the numeric readouts, calm and continuous.
            Row {
                anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                spacing: 18
                Text {
                    color: Theme.textMuted; font.pixelSize: 11; font.family: "monospace"
                    text: App.active ? App.active.solveStatus : ""
                }
                Text {
                    color: Theme.textMuted; font.pixelSize: 11; font.family: "monospace"
                    text: App.active ? qsTr("%1 ms").arg(App.active.solveMilliseconds.toFixed(2)) : ""
                }
                Text {
                    color: Theme.textMuted; font.pixelSize: 11; font.family: "monospace"
                    text: App.active ? qsTr("%1×").arg(App.active.zoom.toFixed(2)) : ""
                }
            }
        }
    }
}
