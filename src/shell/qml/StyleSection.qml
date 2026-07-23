import QtQuick
import QtQuick.Controls.Basic
import Paroculus

// The appearance of the selection, and the named-styles list. Reads the resolved
// appearance and writes through the style actions, which fork a style shared
// beyond the selection and mutate an exclusive one — the toolbar and this section
// are one affordance from two directions. A width or opacity holding an
// expression shows read-only and resists, the same resistance the rectangle
// handle carries. Construction geometry contributes nothing here, so a selection
// of it alone leaves the section empty rather than offering controls its role
// forbids.
Column {
    id: root
    width: parent.width
    spacing: 4
    readonly property var ap: (App.active && App.active.appearance) ? App.active.appearance : ({})

    Text { color: Theme.textDim; font.pixelSize: 10; text: qsTr("STYLE") }

    Row {
        spacing: 6
        Text {
            width: 44; anchors.verticalCenter: parent.verticalCenter
            color: Theme.textSecondary; font.pixelSize: 11; text: qsTr("stroke")
        }
        ColorField {
            anchors.verticalCenter: parent.verticalCenter
            argb: root.ap.strokeColorValue === undefined ? 0 : root.ap.strokeColorValue
            mixed: root.ap.strokeMixed || false
            onCommitted: (v) => App.active.run("style.set-stroke", { "color": v })
        }
    }

    Row {
        spacing: 6
        Text {
            width: 44; anchors.verticalCenter: parent.verticalCenter
            color: Theme.textSecondary; font.pixelSize: 11; text: qsTr("fill")
        }
        ColorField {
            anchors.verticalCenter: parent.verticalCenter
            argb: root.ap.fillColorValue === undefined ? 0 : root.ap.fillColorValue
            mixed: root.ap.fillMixed || false
            onCommitted: (v) => App.active.run("style.set-fill", { "color": v })
        }
    }

    Row {
        spacing: 6
        Text {
            width: 44; anchors.verticalCenter: parent.verticalCenter
            color: Theme.textSecondary; font.pixelSize: 11; text: qsTr("width")
        }
        SlotField {
            width: 114; anchors.verticalCenter: parent.verticalCenter
            editable: !root.ap.strokeWidthExpr
            value: root.ap.strokeWidthExpr ? qsTr("expression")
                 : (root.ap.strokeWidthMixed ? qsTr("mixed") : (root.ap.strokeWidth === undefined ? 1 : root.ap.strokeWidth).toFixed(2))
            onCommitted: App.active.run("style.set-stroke-width", { "value": parseFloat(text) })
        }
    }

    Row {
        spacing: 6
        Text {
            width: 44; anchors.verticalCenter: parent.verticalCenter
            color: Theme.textSecondary; font.pixelSize: 11; text: qsTr("opacity")
        }
        SlotField {
            width: 114; anchors.verticalCenter: parent.verticalCenter
            editable: !root.ap.opacityExpr
            value: root.ap.opacityExpr ? qsTr("expression")
                 : (root.ap.opacityMixed ? qsTr("mixed") : (root.ap.opacity === undefined ? 1 : root.ap.opacity).toFixed(2))
            onCommitted: App.active.run("style.set-opacity", { "value": parseFloat(text) })
        }
    }

    Row {
        spacing: 6
        visible: root.ap.regions > 0
        Text {
            width: 44; anchors.verticalCenter: parent.verticalCenter
            color: Theme.textSecondary; font.pixelSize: 11; text: qsTr("filled")
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: root.ap.filled ? Theme.info : Theme.textDim
            font.pixelSize: 11
            text: root.ap.filledMixed ? qsTr("mixed") : (root.ap.filled ? qsTr("yes") : qsTr("no"))
            MouseArea {
                anchors.fill: parent; anchors.margins: -3
                onClicked: App.active.run("style.set-filled", { "flag": root.ap.filled ? 0 : 1 })
            }
        }
    }

    // Named styles: create from the selection, apply, and "used by N" before a
    // shared edit.
    Text {
        color: Theme.info; font.pixelSize: 11; topPadding: 4
        text: qsTr("+ create style from selection")
        MouseArea {
            anchors.fill: parent; anchors.margins: -3
            onClicked: App.active.run("style.create", {})
        }
    }
    Repeater {
        model: App.active ? App.active.namedStyles : []
        delegate: Row {
            required property var modelData
            spacing: 6
            Rectangle {
                width: 12; height: 12; radius: 2; anchors.verticalCenter: parent.verticalCenter
                border.color: Theme.border; color: modelData.strokeColor
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textSecondary; font.pixelSize: 11
                text: modelData.name + "  (used by " + modelData.usage + ")"
                MouseArea {
                    anchors.fill: parent
                    onClicked: App.active.run("style.apply", { "style": modelData.id })
                }
            }
        }
    }
}
