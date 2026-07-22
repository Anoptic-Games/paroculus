import QtQuick
import Paroculus

// The transient context strip near the work, with its hover-verdict readout.
// Input: paletteOpen (bool) — the strip hides while the command palette is up.
// Fills the canvas as an input-transparent overlay; reads App.active.strip and
// ghosts each entry through the same preview path the constraints toolbar uses.
Item {
    id: stripView
    property bool paletteOpen: false
    anchors.fill: parent

    Rectangle {
        id: strip
        visible: App.active && App.active.strip.length > 0 && !stripView.paletteOpen
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
}
