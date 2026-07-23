import QtQuick
import QtQuick.Controls.Basic
import Paroculus

// A colour input: a swatch that opens the visual picker and a hex field beside
// it, the two directions on one value. Replaces the ad-hoc swatch-plus-hex the
// style rows and the background carried, so every colour in the app gets the
// picker rather than one surface being special.
//
// The edit commits once, when the popup closes on a changed colour or the hex
// field is entered — not on every slider drag, so a colour is one undo step and
// not a hundred. Each commit records the colour as recently used.
Row {
    id: field
    spacing: 6

    // The packed ARGB word the actions speak, carried as a real because a full
    // 0xAARRGGBB with alpha 0xff overflows a 32-bit int. `mixed` shows a
    // multi-selection whose colours disagree, as the field it replaces did.
    property real argb: 0xff000000
    property bool mixed: false

    signal committed(real argb)

    function _colorOf(v) {
        return Qt.rgba(((v >> 16) & 0xff) / 255, ((v >> 8) & 0xff) / 255,
                       (v & 0xff) / 255, ((v >>> 24) & 0xff) / 255)
    }
    function _argbOf(c) {
        return (((Math.round(c.a * 255) & 0xff) << 24) | ((Math.round(c.r * 255) & 0xff) << 16)
             | ((Math.round(c.g * 255) & 0xff) << 8) | (Math.round(c.b * 255) & 0xff)) >>> 0
    }
    function _commit(v) {
        field.argb = v
        if(AppSettings) AppSettings.addRecentColor(v)
        field.committed(v)
    }

    Rectangle {
        id: swatch
        width: 20; height: 20; radius: 3
        anchors.verticalCenter: parent.verticalCenter
        border.color: popup.opened ? Theme.borderStrong : Theme.border
        color: field.mixed ? "transparent" : field._colorOf(field.argb)
        // A hint of the multi-selection state and of being openable.
        Text {
            anchors.centerIn: parent; visible: field.mixed
            text: "?"; color: Theme.textMuted; font.pixelSize: 12
        }
        MouseArea { anchors.fill: parent; onClicked: popup.openAt() }
    }

    SlotField {
        width: 92; anchors.verticalCenter: parent.verticalCenter
        value: field.mixed ? qsTr("mixed")
                           : "#" + ("00000000" + (field.argb >>> 0).toString(16)).slice(-8)
        onCommitted: {
            var t = ("" + text).trim().replace("#", "")
            if(/^[0-9a-fA-F]{8}$/.test(t)) field._commit(parseInt(t, 16))
        }
    }

    Popup {
        id: popup
        // Below the swatch, floating over everything.
        parent: swatch
        y: swatch.height + 4
        padding: 10
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle {
            color: Theme.panelBg
            border.color: Theme.borderStrong
            radius: 6
        }

        property int openingArgb: 0

        function openAt() {
            var c = field.mixed ? Qt.rgba(0, 0, 0, 1) : field._colorOf(field.argb)
            picker.setColor(c)
            openingArgb = field._argbOf(picker.selected)
            open()
        }
        // Commit on close, but only a genuine change — opening and dismissing the
        // picker must not journal a no-op edit.
        onClosed: {
            var v = field._argbOf(picker.selected)
            if(v !== openingArgb) field._commit(v)
        }

        contentItem: Column {
            spacing: 8
            ColorPicker { id: picker; width: 234 }
            Row {
                spacing: 8
                anchors.right: parent.right
                Text {
                    text: qsTr("Done")
                    color: Theme.info; font.pixelSize: 12
                    MouseArea { anchors.fill: parent; anchors.margins: -4; onClicked: popup.close() }
                }
            }
        }
    }
}
