import QtQuick
import Paroculus

// The latest report as a transient toast near the canvas. show(message) flashes
// it and it fades after four seconds. Pure — the reports panel is the memory,
// this is the notice. Anchored to the bottom of its parent (the canvas).
Rectangle {
    id: toast
    function show(message) { toastLabel.text = message; opacity = 1.0; toastTimer.restart() }
    anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 24 }
    width: toastLabel.width + 28
    height: 32
    radius: 6
    color: Theme.headerBg
    border.color: Theme.borderStrong
    opacity: 0.0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 220 } }
    Text { id: toastLabel; anchors.centerIn: parent; color: Theme.textBright; font.pixelSize: 12 }
    Timer { id: toastTimer; interval: 4000; onTriggered: toast.opacity = 0.0 }
}
