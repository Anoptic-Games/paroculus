import QtQuick
import Paroculus

// The visual colour picker: a hue ring with a saturation/value triangle rotating
// inside it, four ValueSliders for hue, saturation, brightness and opacity, a hex
// field, and two swatch rows — the last few colours used and the ones saved.
//
// Hue, saturation, value and alpha are the source of truth; `selected` is derived
// from them through Qt.hsva. Keeping hue a stored number rather than reading it
// back from the colour is deliberate: a grey has no hue to recover, so dragging
// saturation to zero would otherwise lose which hue to return to.
Item {
    id: picker
    implicitWidth: 234
    implicitHeight: contentColumn.implicitHeight

    // 0..360, 0..1, 0..1, 0..1. The controls write these; everything else reads
    // `selected`.
    property real hue: 0
    property real sat: 1
    property real val: 1
    property real alpha: 1
    readonly property color selected: Qt.hsva(hue / 360, sat, val, alpha)

    // Emitted on every change, for a live preview. Committing (recording a recent
    // colour, closing the popup) is the caller's, since only it knows the target.
    signal edited(color colour)

    // Seeds the controls from a colour. Hue is left where it is when the colour is
    // an achromatic grey, so the ring does not jump to red on a black.
    function setColor(c) {
        alpha = c.a
        val = c.hsvValue
        sat = c.hsvSaturation
        if(c.hsvSaturation > 0.0001 && c.hsvValue > 0.0001) hue = c.hsvHue * 360
    }

    // Packed ARGB (0xAARRGGBB) <-> QML colour, the words the actions and settings
    // speak. Split out so the swatch rows and the hex field share one conversion.
    function argbOf(c) {
        // >>> 0 keeps it a positive 0..0xffffffff word: the action casts to
        // uint32 and a negative double would be undefined there.
        return (((Math.round(c.a * 255) & 0xff) << 24) | ((Math.round(c.r * 255) & 0xff) << 16)
             | ((Math.round(c.g * 255) & 0xff) << 8) | (Math.round(c.b * 255) & 0xff)) >>> 0
    }
    function colorOfArgb(v) {
        return Qt.rgba(((v >> 16) & 0xff) / 255, ((v >> 8) & 0xff) / 255,
                       (v & 0xff) / 255, ((v >>> 24) & 0xff) / 255)
    }

    onSelectedChanged: picker.edited(selected)

    // The swatch rows, read from settings at build and refreshed on a save so the
    // Flow updates without a notifying property.
    property var recentList: AppSettings ? AppSettings.recentColors() : []
    property var savedList: AppSettings ? AppSettings.savedColors() : []
    function refreshSaved() { savedList = AppSettings ? AppSettings.savedColors() : [] }

    Column {
        id: contentColumn
        width: parent.width
        spacing: 8

        // ---- The wheel: hue ring plus a rotating S/V triangle ----
        Item {
            id: wheel
            width: parent.width
            height: width
            readonly property real cx: width / 2
            readonly property real cy: height / 2
            readonly property real rOuter: width / 2 - 1
            readonly property real rInner: rOuter - 18
            readonly property real rMid: (rOuter + rInner) / 2
            readonly property real triR: rInner - 3
            readonly property real hueAngle: picker.hue / 360 * 2 * Math.PI
            // The three triangle vertices: pure hue, white, black, in that order.
            readonly property var triV: [
                {x: cx + triR * Math.cos(hueAngle),                 y: cy + triR * Math.sin(hueAngle)},
                {x: cx + triR * Math.cos(hueAngle + 2.0943951),     y: cy + triR * Math.sin(hueAngle + 2.0943951)},
                {x: cx + triR * Math.cos(hueAngle + 4.1887902),     y: cy + triR * Math.sin(hueAngle + 4.1887902)}
            ]

            // The hue ring is static — the full spectrum never changes — so it
            // paints once and is never touched by a hue change. Keeping it off the
            // per-frame path is half of why dragging stays cheap.
            Canvas {
                id: ringCanvas
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    var segs = 180
                    for(var i = 0; i < segs; i++) {
                        var a0 = i / segs * 2 * Math.PI
                        var a1 = (i + 1) / segs * 2 * Math.PI + 0.02
                        ctx.beginPath()
                        ctx.arc(wheel.cx, wheel.cy, wheel.rOuter, a0, a1, false)
                        ctx.arc(wheel.cx, wheel.cy, wheel.rInner, a1, a0, true)
                        ctx.closePath()
                        ctx.fillStyle = Qt.hsva(i / segs, 1, 1, 1)
                        ctx.fill()
                    }
                }
            }
            // The S/V triangle, three gradient fills clipped to the triangle rather
            // than a per-pixel sweep. The earlier per-pixel version cost ~20ms and
            // ran on every hue change, so a hue drag queued repaints faster than
            // they drained and starved the event loop — the freeze. Each vertex is
            // exact (its gradient starts there); the interior is the standard
            // vertex-to-opposite-edge-midpoint approximation, imperceptible here.
            Canvas {
                id: triCanvas
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    var A = wheel.triV[0], B = wheel.triV[1], C = wheel.triV[2]
                    ctx.save()
                    ctx.beginPath()
                    ctx.moveTo(A.x, A.y); ctx.lineTo(B.x, B.y); ctx.lineTo(C.x, C.y)
                    ctx.closePath()
                    ctx.clip()
                    // Pure hue everywhere, then black toward the black vertex and
                    // white toward the white vertex composited over it.
                    ctx.fillStyle = Qt.hsva(picker.hue / 360, 1, 1, 1)
                    ctx.fillRect(0, 0, width, height)
                    var mAB = {x: (A.x + B.x) / 2, y: (A.y + B.y) / 2}
                    var gBlack = ctx.createLinearGradient(C.x, C.y, mAB.x, mAB.y)
                    gBlack.addColorStop(0, "rgba(0,0,0,1)")
                    gBlack.addColorStop(1, "rgba(0,0,0,0)")
                    ctx.fillStyle = gBlack
                    ctx.fillRect(0, 0, width, height)
                    var mAC = {x: (A.x + C.x) / 2, y: (A.y + C.y) / 2}
                    var gWhite = ctx.createLinearGradient(B.x, B.y, mAC.x, mAC.y)
                    gWhite.addColorStop(0, "rgba(255,255,255,1)")
                    gWhite.addColorStop(1, "rgba(255,255,255,0)")
                    ctx.fillStyle = gWhite
                    ctx.fillRect(0, 0, width, height)
                    ctx.restore()
                }
            }
            // Only the triangle follows a hue change; the ring is fixed.
            Connections {
                target: picker
                function onHueChanged() { triCanvas.requestPaint() }
            }
            onWidthChanged: { ringCanvas.requestPaint(); triCanvas.requestPaint() }

            // Ring handle.
            Rectangle {
                width: 10; height: 10; radius: 5
                border.color: "#000000"; border.width: 1
                color: Qt.hsva(picker.hue / 360, 1, 1, 1)
                x: wheel.cx + wheel.rMid * Math.cos(wheel.hueAngle) - width / 2
                y: wheel.cy + wheel.rMid * Math.sin(wheel.hueAngle) - height / 2
            }
            // Triangle handle at the barycentric point of (sat, val).
            Rectangle {
                readonly property real w0: picker.sat * picker.val
                readonly property real w1: (1 - picker.sat) * picker.val
                readonly property real w2: 1 - picker.val
                width: 11; height: 11; radius: 6
                border.color: picker.val > 0.5 ? "#000000" : "#ffffff"; border.width: 1
                color: picker.selected
                x: w0 * wheel.triV[0].x + w1 * wheel.triV[1].x + w2 * wheel.triV[2].x - width / 2
                y: w0 * wheel.triV[0].y + w1 * wheel.triV[1].y + w2 * wheel.triV[2].y - height / 2
            }

            MouseArea {
                anchors.fill: parent
                onPressed: (m) => wheel.pick(m.x, m.y)
                onPositionChanged: (m) => { if(pressed) wheel.pick(m.x, m.y) }
            }
            // A press on the ring sets hue; anywhere else is read as the triangle,
            // its weights clamped so a pick outside still lands on the nearest
            // colour rather than nothing.
            function pick(px, py) {
                var dx = px - cx, dy = py - cy
                var dist = Math.hypot(dx, dy)
                if(dist >= rInner - 1 && dist <= rOuter + 4) {
                    var a = Math.atan2(dy, dx) / (2 * Math.PI) * 360
                    picker.hue = a < 0 ? a + 360 : a
                    return
                }
                var A = triV[0], B = triV[1], C = triV[2]
                var denom = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y)
                if(Math.abs(denom) < 1e-6) return
                var w0 = ((B.y - C.y) * (px - C.x) + (C.x - B.x) * (py - C.y)) / denom
                var w1 = ((C.y - A.y) * (px - C.x) + (A.x - C.x) * (py - C.y)) / denom
                var w2 = 1 - w0 - w1
                w0 = Math.max(0, w0); w1 = Math.max(0, w1); w2 = Math.max(0, w2)
                var s = w0 + w1 + w2
                if(s <= 0) return
                w0 /= s; w1 /= s
                picker.val = w0 + w1
                picker.sat = (w0 + w1) > 0 ? w0 / (w0 + w1) : 0
            }
        }

        // ---- The four sliders, each with the readout of its channel ----
        Grid {
            width: parent.width
            columns: 3
            rowSpacing: 5
            columnSpacing: 6
            verticalItemAlignment: Grid.AlignVCenter

            // Hue
            Text { text: qsTr("H"); color: Theme.textSecondary; font.pixelSize: 11; width: 12 }
            ValueSlider {
                width: picker.width - 64; from: 0; to: 360; value: picker.hue
                onMoved: (v) => picker.hue = v
                Rectangle {
                    anchors.fill: parent; radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0;   color: "#ff0000" }
                        GradientStop { position: 0.166; color: "#ffff00" }
                        GradientStop { position: 0.333; color: "#00ff00" }
                        GradientStop { position: 0.5;   color: "#00ffff" }
                        GradientStop { position: 0.666; color: "#0000ff" }
                        GradientStop { position: 0.833; color: "#ff00ff" }
                        GradientStop { position: 1.0;   color: "#ff0000" }
                    }
                }
            }
            Text {
                text: Math.round(picker.hue) + "°"; color: Theme.textMuted
                font.pixelSize: 10; width: 34; horizontalAlignment: Text.AlignRight
            }

            // Saturation
            Text { text: qsTr("S"); color: Theme.textSecondary; font.pixelSize: 11; width: 12 }
            ValueSlider {
                width: picker.width - 64; from: 0; to: 1; value: picker.sat
                onMoved: (v) => picker.sat = v
                Rectangle {
                    anchors.fill: parent; radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Qt.hsva(picker.hue / 360, 0, picker.val, 1) }
                        GradientStop { position: 1.0; color: Qt.hsva(picker.hue / 360, 1, picker.val, 1) }
                    }
                }
            }
            Text {
                text: Math.round(picker.sat * 100) + "%"; color: Theme.textMuted
                font.pixelSize: 10; width: 34; horizontalAlignment: Text.AlignRight
            }

            // Brightness (value)
            Text { text: qsTr("B"); color: Theme.textSecondary; font.pixelSize: 11; width: 12 }
            ValueSlider {
                width: picker.width - 64; from: 0; to: 1; value: picker.val
                onMoved: (v) => picker.val = v
                Rectangle {
                    anchors.fill: parent; radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#000000" }
                        GradientStop { position: 1.0; color: Qt.hsva(picker.hue / 360, picker.sat, 1, 1) }
                    }
                }
            }
            Text {
                text: Math.round(picker.val * 100) + "%"; color: Theme.textMuted
                font.pixelSize: 10; width: 34; horizontalAlignment: Text.AlignRight
            }

            // Opacity, over a checker so transparency reads as transparency.
            Text { text: qsTr("A"); color: Theme.textSecondary; font.pixelSize: 11; width: 12 }
            ValueSlider {
                width: picker.width - 64; from: 0; to: 1; value: picker.alpha
                onMoved: (v) => picker.alpha = v
                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d"); var s = 4
                        for(var y = 0; y < height; y += s)
                            for(var x = 0; x < width; x += s) {
                                ctx.fillStyle = ((x / s + y / s) % 2 === 0) ? "#5a5f68" : "#3a3e45"
                                ctx.fillRect(x, y, s, s)
                            }
                    }
                }
                Rectangle {
                    anchors.fill: parent; radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Qt.rgba(picker.selected.r, picker.selected.g, picker.selected.b, 0) }
                        GradientStop { position: 1.0; color: Qt.rgba(picker.selected.r, picker.selected.g, picker.selected.b, 1) }
                    }
                }
            }
            Text {
                text: Math.round(picker.alpha * 100) + "%"; color: Theme.textMuted
                font.pixelSize: 10; width: 34; horizontalAlignment: Text.AlignRight
            }
        }

        // ---- Hex entry, mirroring the field it replaces (#AARRGGBB) ----
        Row {
            spacing: 6
            Rectangle {
                width: 20; height: 20; radius: 3; anchors.verticalCenter: parent.verticalCenter
                border.color: Theme.border; color: picker.selected
            }
            SlotField {
                width: picker.width - 32; anchors.verticalCenter: parent.verticalCenter
                value: "#" + ("00000000" + (picker.argbOf(picker.selected) >>> 0).toString(16)).slice(-8)
                onCommitted: {
                    var t = ("" + text).trim().replace("#", "")
                    if(/^[0-9a-fA-F]{8}$/.test(t)) picker.setColor(picker.colorOfArgb(parseInt(t, 16)))
                }
            }
        }

        // ---- Recent and saved swatch rows ----
        SwatchRow {
            width: parent.width
            heading: qsTr("RECENT")
            colours: picker.recentList
            onPicked: (argb) => picker.setColor(picker.colorOfArgb(argb))
        }
        SwatchRow {
            width: parent.width
            heading: qsTr("SAVED")
            colours: picker.savedList
            showSave: true
            onPicked: (argb) => picker.setColor(picker.colorOfArgb(argb))
            onSaveCurrent: {
                if(AppSettings) AppSettings.addSavedColor(picker.argbOf(picker.selected))
                picker.refreshSaved()
            }
            onRemove: (argb) => { if(AppSettings) AppSettings.removeSavedColor(argb); picker.refreshSaved() }
        }
    }
}
