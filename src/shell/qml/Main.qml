import QtQuick
import QtQuick.Controls.Basic
import Paroculus

ApplicationWindow {
    width: 900
    height: 620
    visible: true
    title: qsTr("paroculus")
    color: "#0e1013"

    SketchView {
        id: sketch
        anchors.fill: parent
        anchors.bottomMargin: 78
        focus: true
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 78
        color: "#191c21"

        Text {
            id: label
            anchors { left: parent.left; leftMargin: 20; top: parent.top; topMargin: 14 }
            color: "#c8ccd4"
            font.pixelSize: 13
            text: sketch.selectionText
        }

        Text {
            anchors { left: parent.left; leftMargin: 20; top: label.bottom; topMargin: 8 }
            color: "#7f8794"
            font.pixelSize: 11
            // Drag is a solve; everything else is selection. Esc lands home.
            text: qsTr("drag a point · shift-click to extend · marquee on empty space · " +
                       "del to delete · z / shift-z to undo · wheel to zoom · esc to clear")
        }

        Text {
            anchors { right: parent.right; rightMargin: 20; top: parent.top; topMargin: 14 }
            color: "#7f8794"
            font.pixelSize: 12
            font.family: "monospace"
            text: sketch.status
        }
    }
}
