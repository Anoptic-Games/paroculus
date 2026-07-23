import QtQuick
import QtQuick.Controls.Basic
import Paroculus

// The style toolbar: the selection's resolved appearance as a compact strip,
// writing through the same style actions the inspector's Style section does — one
// affordance from two directions. Shown only when the selection has something
// styleable, so it never offers controls a construction-only selection forbids.
// Width and opacity resist an expression-driven slot, the rectangle-handle rule.
Rectangle {
    id: root
    readonly property var ap: (App.active && App.active.appearance) ? App.active.appearance : ({})
    // Hidden in inspect mode, where the canvas presents the document as output
    // and the style toolbar is editing chrome.
    visible: root.ap.any === true && !(App.active && App.active.inspectMode)
    implicitWidth: bar.implicitWidth + 16
    implicitHeight: 28
    radius: 4
    color: Theme.surfaceRaised
    border.color: Theme.border

    Row {
        id: bar
        anchors.centerIn: parent
        spacing: 8

        ColorField {  // stroke — the swatch opens the picker, the field edits hex
            anchors.verticalCenter: parent.verticalCenter
            argb: root.ap.strokeColorValue === undefined ? 0 : root.ap.strokeColorValue
            mixed: root.ap.strokeMixed || false
            onCommitted: (v) => App.active.run("style.set-stroke", { "color": v })
        }
        ColorField {  // fill
            anchors.verticalCenter: parent.verticalCenter
            argb: root.ap.fillColorValue === undefined ? 0 : root.ap.fillColorValue
            mixed: root.ap.fillMixed || false
            onCommitted: (v) => App.active.run("style.set-fill", { "color": v })
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textDim; font.pixelSize: 10; text: qsTr("w")
        }
        SlotField {
            width: 48; anchors.verticalCenter: parent.verticalCenter
            editable: !root.ap.strokeWidthExpr
            value: root.ap.strokeWidthExpr ? qsTr("expr")
                 : (root.ap.strokeWidthMixed ? "—" : (root.ap.strokeWidth === undefined ? 1 : root.ap.strokeWidth).toFixed(2))
            onCommitted: App.active.run("style.set-stroke-width", { "value": parseFloat(text) })
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textDim; font.pixelSize: 10; text: qsTr("op")
        }
        SlotField {
            width: 48; anchors.verticalCenter: parent.verticalCenter
            editable: !root.ap.opacityExpr
            value: root.ap.opacityExpr ? qsTr("expr")
                 : (root.ap.opacityMixed ? "—" : (root.ap.opacity === undefined ? 1 : root.ap.opacity).toFixed(2))
            onCommitted: App.active.run("style.set-opacity", { "value": parseFloat(text) })
        }
        Text {
            visible: root.ap.regions > 0
            anchors.verticalCenter: parent.verticalCenter
            color: root.ap.filled ? Theme.info : Theme.textDim
            font.pixelSize: 11
            text: root.ap.filledMixed ? qsTr("fill?") : (root.ap.filled ? qsTr("filled") : qsTr("unfilled"))
            MouseArea {
                anchors.fill: parent; anchors.margins: -3
                onClicked: App.active.run("style.set-filled", { "flag": root.ap.filled ? 0 : 1 })
            }
        }
    }
}
