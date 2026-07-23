import QtQuick
import Paroculus

// A heading over a wrapping row of colour swatches, with an optional save tile.
// The colour picker uses two — recent and saved — but nothing here is specific to
// either, so a later palette surface can reuse it. Colours arrive as packed ARGB
// integers; a click reports one back, a right-click asks to remove it.
Column {
    id: row
    spacing: 3

    property string heading: ""
    property var colours: []
    // The save tile: a "+" that reports the current colour should be kept.
    property bool showSave: false

    signal picked(int argb)
    signal saveCurrent()
    signal remove(int argb)

    function _colorOf(v) {
        return Qt.rgba(((v >> 16) & 0xff) / 255, ((v >> 8) & 0xff) / 255,
                       (v & 0xff) / 255, ((v >>> 24) & 0xff) / 255)
    }

    Text {
        text: row.heading
        color: Theme.textDim
        font.pixelSize: 10
        visible: row.heading.length > 0
    }

    Flow {
        width: parent.width
        spacing: 4

        Repeater {
            model: row.colours
            delegate: Rectangle {
                required property var modelData
                width: 18; height: 18; radius: 3
                border.color: swatchHover.hovered ? Theme.borderStrong : Theme.border
                color: row._colorOf(modelData)
                HoverHandler { id: swatchHover }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (m) => {
                        if(m.button === Qt.RightButton) row.remove(modelData)
                        else row.picked(modelData)
                    }
                }
            }
        }

        // The save tile, last so it trails the swatches it adds to.
        Rectangle {
            visible: row.showSave
            width: 18; height: 18; radius: 3
            color: saveHover.hovered ? Theme.hoverStrong : Theme.surface
            border.color: Theme.border
            HoverHandler { id: saveHover }
            Text {
                anchors.centerIn: parent; text: "+"
                color: Theme.textSecondary; font.pixelSize: 13
            }
            MouseArea { anchors.fill: parent; onClicked: row.saveCurrent() }
        }

        // A quiet placeholder so an empty row still has height and heading meaning.
        Text {
            visible: row.colours.length === 0 && !row.showSave
            text: qsTr("none yet")
            color: Theme.textFaint; font.pixelSize: 10
        }
    }
}
