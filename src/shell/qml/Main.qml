import QtQuick
import QtQuick.Controls.Basic
import Paroculus

ApplicationWindow {
    id: root
    width: 900
    height: 620
    visible: true
    title: qsTr("paroculus")
    color: "#0e1013"

    SketchView {
        id: sketch
        anchors.fill: parent
        anchors.bottomMargin: 78
        focus: !palette.visible
    }

    // The transient strip, near the work.
    //
    // Context sensitivity arrives additively: this appears when the selection
    // admits something and is otherwise not there at all. The permanent
    // furniture below never reshuffles, which is the other half of the same
    // discipline — ranking within the strip is contextual, placement of the
    // rest is not.
    Rectangle {
        id: strip
        visible: sketch.strip.length > 0 && !palette.visible
        anchors { left: parent.left; leftMargin: 16; bottom: sketch.bottom; bottomMargin: 16 }
        width: stripRow.width + 20
        height: 34
        radius: 5
        color: "#22262d"
        border.color: "#333944"

        Row {
            id: stripRow
            anchors.centerIn: parent
            spacing: 6

            Repeater {
                model: sketch.strip
                delegate: Rectangle {
                    required property var modelData
                    width: entryLabel.width + 16
                    height: 24
                    radius: 3
                    color: entryMouse.containsMouse ? "#313947" : "transparent"

                    Text {
                        id: entryLabel
                        anchors.centerIn: parent
                        color: entryMouse.containsMouse ? "#e6e9ee" : "#aab1bd"
                        font.pixelSize: 12
                        text: modelData.title
                    }

                    MouseArea {
                        id: entryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        // Hovering previews. Speculative solves make the
                        // catalogue learnable by looking rather than by
                        // reading, and the document is untouched throughout.
                        onEntered: {
                            preview.text = sketch.previewOf(
                                modelData.name,
                                modelData.arguments.assignment !== undefined
                                    ? modelData.arguments.assignment : 0)
                        }
                        onExited: preview.text = ""
                        onClicked: {
                            sketch.run(modelData.name, modelData.arguments)
                            preview.text = ""
                        }
                    }
                }
            }
        }
    }

    Text {
        id: preview
        visible: text.length > 0 && strip.visible
        anchors { left: strip.right; leftMargin: 12; verticalCenter: strip.verticalCenter }
        color: "#8f97a4"
        font.pixelSize: 11
    }

    // The command palette: everything, in the table's own order, searched by
    // subsequence. Permanent furniture, so it does not reshuffle with context
    // and inapplicable entries dim rather than vanish.
    Rectangle {
        id: palette
        visible: false
        anchors.centerIn: parent
        width: 460
        height: 360
        radius: 6
        color: "#1b1f25"
        border.color: "#39404c"

        function open() {
            visible = true
            query.text = ""
            results.model = sketch.palette("")
            query.forceActiveFocus()
        }
        function close() {
            visible = false
            sketch.forceActiveFocus()
        }

        TextField {
            id: query
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
            placeholderText: qsTr("search commands")
            color: "#e6e9ee"
            background: Rectangle { color: "#141820"; radius: 4 }
            onTextChanged: results.model = sketch.palette(text)
            Keys.onEscapePressed: palette.close()
            Keys.onReturnPressed: {
                if (results.count > 0) {
                    const first = results.model[0]
                    if (first.applicable) sketch.run(first.name, first.arguments)
                }
                palette.close()
            }
        }

        ListView {
            id: results
            anchors {
                left: parent.left; right: parent.right
                top: query.bottom; bottom: parent.bottom; margins: 10
            }
            clip: true
            delegate: Item {
                required property var modelData
                width: results.width
                height: 24

                Text {
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    // Dimmed rather than hidden: a command that vanishes is a
                    // command the user cannot learn.
                    color: modelData.applicable ? "#d6dae1" : "#5a616c"
                    font.pixelSize: 12
                    text: modelData.title
                }
                Text {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    color: "#4e5560"
                    font.pixelSize: 10
                    font.family: "monospace"
                    text: modelData.name
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: modelData.applicable
                    onClicked: {
                        sketch.run(modelData.name, modelData.arguments)
                        palette.close()
                    }
                }
            }
        }
    }

    Shortcut {
        sequences: ["Ctrl+P", "Ctrl+Shift+P"]
        onActivated: palette.visible ? palette.close() : palette.open()
    }

    // The layers, back to front. Permanent furniture, so it is spatially stable:
    // a layer does not move in the list because the selection changed, and a
    // hidden or locked one dims rather than vanishes — the same discipline the
    // palette follows and the transient strip deliberately does not.
    Column {
        anchors { right: parent.right; rightMargin: 20; top: parent.top; topMargin: 56 }
        spacing: 4
        visible: sketch.layers.length > 0

        Repeater {
            model: sketch.layers
            delegate: Row {
                spacing: 8
                Text {
                    color: modelData.visible ? "#c8d2e0" : "#5c646f"
                    font.pixelSize: 12
                    text: modelData.name
                }
                Text {
                    color: "#7f8794"
                    font.pixelSize: 12
                    font.family: "monospace"
                    text: (modelData.visible ? "" : "hidden ") +
                          (modelData.locked ? "locked" : "")
                }
            }
        }
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

        // Displayed calmly, never as a progress bar or a warning: under-
        // constraint is the normal state, and a free degree of freedom is a
        // thing the user can still push by hand rather than an item of debt.
        Text {
            id: dofLabel
            anchors { left: label.right; leftMargin: 18; baseline: label.baseline }
            color: sketch.dof > 0 ? "#7cc4e0" : "#7f8794"
            font.pixelSize: 13
            font.family: "monospace"
            text: sketch.dof >= 0 ? qsTr("%1 dof").arg(sketch.dof) : qsTr("unsolved")
        }

        // Nothing the system does on its own initiative is invisible. These are
        // two faces of that one policy: something the user cannot see moved
        // something they can, and a deletion left a fill unable to enclose what
        // it still says it encloses. Both are diagnostics beside the work, never
        // dialogs, because there is no error state that suspends editing.
        Text {
            id: influence
            anchors { left: dofLabel.right; leftMargin: 18; baseline: label.baseline }
            visible: sketch.hiddenInfluences > 0
            color: "#e0c07c"
            font.pixelSize: 13
            font.family: "monospace"
            text: qsTr("%1 hidden influencing").arg(sketch.hiddenInfluences)
        }

        Text {
            anchors {
                left: influence.visible ? influence.right : dofLabel.right
                leftMargin: 18
                baseline: label.baseline
            }
            visible: sketch.brokenRegions > 0
            color: "#ff8c42"
            font.pixelSize: 13
            font.family: "monospace"
            text: qsTr("%1 broken").arg(sketch.brokenRegions)
        }

        Text {
            anchors { left: parent.left; leftMargin: 20; top: label.bottom; topMargin: 8 }
            color: "#7f8794"
            font.pixelSize: 11
            // Drag is a solve; everything else is selection. Esc lands home.
            text: qsTr("drag a point · shift-click to extend · marquee on empty space · " +
                       "del to delete · z / shift-z to undo · g / shift-g to group · " +
                       "h / k to hide or lock a layer · ctrl-p for commands · " +
                       "wheel to zoom · esc to clear")
        }

        Text {
            // Bounded on the left by the readouts, because the status grows: a
            // tool's live parameters, its offers, what a placement declared,
            // whether a loop closed. Unbounded it would overrun them rather
            // than give way.
            anchors {
                left: dofLabel.right; leftMargin: 20
                right: parent.right; rightMargin: 20
                top: parent.top; topMargin: 14
            }
            color: "#7f8794"
            font.pixelSize: 12
            font.family: "monospace"
            horizontalAlignment: Text.AlignRight
            // Cut from the head when it will not fit: the tail carries dof and
            // solve time, which are the numbers being watched continuously.
            elide: Text.ElideLeft
            text: sketch.status
        }
    }
}
