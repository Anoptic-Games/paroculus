import QtQuick
import Paroculus

// A generalized value slider: a horizontal track the caller fills with whatever
// belongs behind it (a plain fill, a hue gradient, an alpha checker), a handle
// that rides the value, and press/drag/keyboard input mapping a position to a
// value in [from, to].
//
// Color-agnostic on purpose. The colour picker composes four of these with
// different tracks, and later surfaces — a density scrubber, an opacity row —
// get the same control rather than a second implementation. The caller owns the
// track visual and reads `moved`; nothing here knows what the value means.
Item {
    id: control
    implicitWidth: 140
    implicitHeight: 16

    property real from: 0
    property real to: 1
    property real value: 0
    // Keyboard and click-step increment; 0 means a hundredth of the range.
    property real stepSize: 0
    property real trackHeight: 8
    property color handleColor: Theme.textBright
    // The track visual. A caller writes `ValueSlider { Rectangle { ... } }` and
    // its children fill the track area behind the handle.
    default property alias trackData: track.data

    // Emitted whenever the user moves the handle, with the new value. Not emitted
    // for a programmatic `value` write, so a two-way binding cannot loop.
    signal moved(real value)

    readonly property real _span: to - from
    readonly property real _step: stepSize > 0 ? stepSize : (_span !== 0 ? _span / 100 : 0)
    readonly property real position:
        _span !== 0 ? Math.max(0, Math.min(1, (value - from) / _span)) : 0
    readonly property real _travel: width - handle.width

    // Sets the value from a desired handle-centre x, clamped, and reports it. The
    // handle centre tracks the cursor rather than its left edge, so a press lands
    // the value where the eye is.
    function _applyAt(centreX) {
        if(_travel <= 0) return
        var p = Math.max(0, Math.min(1, (centreX - handle.width / 2) / _travel))
        value = from + p * _span
        moved(value)
    }
    function _nudge(delta) {
        value = Math.max(from, Math.min(to, value + delta))
        moved(value)
    }

    Item {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: control.trackHeight
        clip: true
    }
    Rectangle {  // track outline, over the caller's fill
        anchors.fill: track
        color: "transparent"
        radius: height / 2
        border.color: Theme.border
    }

    Rectangle {
        id: handle
        width: 12
        height: control.height
        radius: 3
        x: control.position * control._travel
        color: control.handleColor
        border.color: Theme.windowBg
        border.width: 2
    }

    MouseArea {
        anchors.fill: parent
        onPressed: (m) => { control.forceActiveFocus(); control._applyAt(m.x) }
        onPositionChanged: (m) => { if(pressed) control._applyAt(m.x) }
    }

    Keys.onLeftPressed: control._nudge(-control._step)
    Keys.onRightPressed: control._nudge(control._step)
    activeFocusOnTab: true
}
