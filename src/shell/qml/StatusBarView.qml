import QtQuick
import QtQuick.Layouts
import Paroculus

// The status bar: each readout its own typed projection over App.active. The
// status mega-string is gone — the selection ladder, dof and calm drag notes at
// left; solve status, timing and zoom at right. Pure; reads no frame-local
// state. The report-shaped fragments went to the reports model and its toast,
// the tool fragment to the tool options row.
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
