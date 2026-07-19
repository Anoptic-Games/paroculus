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
        ratio: ratioSlider.value
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
            // Proportion is the control surface, not a derived readout.
            text: qsTr("len(A) / len(B)")
        }

        Slider {
            id: ratioSlider
            anchors { left: parent.left; leftMargin: 20; right: parent.right; rightMargin: 20
                      top: label.bottom; topMargin: 6 }
            from: 0.4
            to: 3.0
            value: 1.618
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
