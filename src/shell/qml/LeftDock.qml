import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Paroculus

// The left dock: the tools toolbar over the constraints toolbar, both
// projections of the one registry table — the tools by category, the
// constraints as the imposition rows grouped by taxonomy family. Exposes
// constraintHint (string): the hover verdict the frame draws on the canvas,
// written as the constraint buttons are hovered. Owns the three-strengths
// flyout it pops on right-click.
Rectangle {
    id: leftDock
    property string constraintHint: ""
    Layout.fillHeight: true
    implicitWidth: 64
    color: Theme.dockBg

    // The registry rows in the given categories, live — the tools are the
    // "tool" category of the one projection.
    function actionsIn(cats) {
        if (!App.active) return []
        return App.active.actions.filter(function(a) { return cats.indexOf(a.category) >= 0 })
    }

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
        spacing: 6

        // Tools: the five tool actions, the active tool held down,
        // tooltips carrying name, binding and description.
        Column {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            spacing: 6
            Repeater {
                model: leftDock.actionsIn(["tool"])
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
                            model: leftDock.imposeActionsInFamily(modelData)
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
                                    onEntered: leftDock.constraintHint = App.active.previewOf(modelData.name, 0)
                                    onExited: { leftDock.constraintHint = ""; App.active.clearPreview() }
                                    onClicked: (mouse) => {
                                        if (mouse.button === Qt.RightButton) {
                                            strengthMenu.actionBase = modelData.name
                                            strengthMenu.actionTitle = modelData.title
                                            strengthMenu.popup()
                                        } else {
                                            App.active.run(modelData.name, {})
                                            leftDock.constraintHint = ""
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
                    model: leftDock.toolbarRelationActions()
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
