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
    visible: root.ap.any === true
    implicitWidth: bar.implicitWidth + 16
    implicitHeight: 28
    radius: 4
    color: Theme.surfaceRaised
    border.color: Theme.border

    function parseColor(s) {
        var t = ("" + s).trim().replace("#", "")
        if (!/^[0-9a-fA-F]{8}$/.test(t)) return -1
        return parseInt(t, 16)
    }

    Row {
        id: bar
        anchors.centerIn: parent
        spacing: 8

        Rectangle {  // stroke swatch — click cycles nothing; the field edits it
            width: 16; height: 16; radius: 2; anchors.verticalCenter: parent.verticalCenter
            border.color: Theme.border
            color: root.ap.strokeMixed ? "transparent" : (root.ap.strokeColor || "#00000000")
        }
        SlotField {
            width: 84; anchors.verticalCenter: parent.verticalCenter
            value: root.ap.strokeMixed ? qsTr("mixed") : (root.ap.strokeColor || "")
            onCommitted: { var v = root.parseColor(text); if (v >= 0) App.active.run("style.set-stroke", { "color": v }) }
        }
        Rectangle {
            width: 16; height: 16; radius: 2; anchors.verticalCenter: parent.verticalCenter
            border.color: Theme.border
            color: root.ap.fillMixed ? "transparent" : (root.ap.fillColor || "#00000000")
        }
        SlotField {
            width: 84; anchors.verticalCenter: parent.verticalCenter
            value: root.ap.fillMixed ? qsTr("mixed") : (root.ap.fillColor || "")
            onCommitted: { var v = root.parseColor(text); if (v >= 0) App.active.run("style.set-fill", { "color": v }) }
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
