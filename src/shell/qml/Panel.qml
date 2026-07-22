import QtQuick
import QtQuick.Layouts
import Paroculus

// A dockable panel: a title bar with a collapse chevron and a close, and a
// content body. Inputs: title (string); locked (bool, injected) — the window
// lock that freezes the collapse and close affordances, the whole of the lock
// feature. collapsed is internal state, body the default content slot. Depends
// only on Theme and its injected props, so a docking dependency could replace
// this implementation without touching any panel.
Rectangle {
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
