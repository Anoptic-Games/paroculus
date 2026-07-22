import QtQuick
import QtQuick.Controls.Basic
import Paroculus

// A value cell that reads like text and edits like a field: click to edit, Enter
// to commit. Every value cell in every panel is a slot editor, not a number
// editor — constant today, an expression the moment the panel passes text — so
// this is the one place the pattern lives. A cell whose value was authored
// elsewhere (an expression) is marked read-only and resists a direct edit, the
// same resistance the rectangle handle carries.
Row {
    id: root
    property string label: ""
    property string value: ""
    property bool editable: true
    // The text the field holds, read by the committed handler.
    readonly property string text: field.text
    signal committed()

    spacing: 3

    Text {
        anchors.verticalCenter: parent.verticalCenter
        visible: root.label.length > 0
        color: Theme.textDim
        font.pixelSize: 10
        text: root.label
    }
    Item {
        width: root.width - (root.label.length > 0 ? 20 : 0)
        height: 18
        anchors.verticalCenter: parent.verticalCenter
        Rectangle {
            anchors.fill: parent
            visible: field.visible
            color: Theme.fieldBg
            border.color: Theme.borderStrong
            radius: 2
        }
        Text {
            visible: !field.visible
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            leftPadding: 2
            color: root.editable ? Theme.textSecondary : Theme.textDim
            font.pixelSize: 11
            font.family: "monospace"
            elide: Text.ElideRight
            text: root.value
            MouseArea {
                anchors.fill: parent
                enabled: root.editable
                onClicked: {
                    field.text = root.value
                    field.visible = true
                    field.forceActiveFocus()
                    field.selectAll()
                }
            }
        }
        TextInput {
            id: field
            visible: false
            anchors.fill: parent
            leftPadding: 2
            verticalAlignment: Text.AlignVCenter
            color: Theme.textBright
            font.pixelSize: 11
            font.family: "monospace"
            onAccepted: {
                root.committed()
                visible = false
            }
            onActiveFocusChanged: if (!activeFocus) visible = false
        }
    }
}
