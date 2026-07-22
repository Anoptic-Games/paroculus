import QtQuick
import QtQuick.Controls.Basic
import Paroculus

// The command palette overlay: everything in the table's own order, searched by
// subsequence, dimming what does not apply rather than hiding it. open()/close()
// drive visibility; close() emits closed() so the frame can return focus to the
// canvas. Centred on its parent (the canvas).
Rectangle {
    id: commandPalette
    signal closed()
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
    function close() { visible = false; commandPalette.closed() }
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
