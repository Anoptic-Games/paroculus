import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Paroculus

// The SVG export dialog: destination, margin, precision, and the loss report the
// bake would cost, shown before any write so the lossy step is consented to
// rather than discovered. Export is dimmed until a destination is chosen and a
// document exists to bake; on Export the write is checked end to end in
// exportSvg, and a failure surfaces through the workspace's exportFailed toast.
//
// The loss report is read from App.active.exportReport, which bakes the same
// value the write turns into a file — so what the report describes is exactly
// what lands. Recomputed whenever the dialog opens or the options change; margin
// and precision scale coordinates, not the loss, but reading it live keeps the
// one call answering the whole dialog.
Dialog {
    id: exportDialog
    title: qsTr("Export SVG")
    modal: true
    anchors.centerIn: parent
    width: 460
    standardButtons: Dialog.NoButton

    property string destination: ""
    property real margin: 8.0
    property int precision: 4
    // The loss does not depend on margin or precision — those scale coordinates,
    // not what flattens — so it is read once when the dialog opens rather than
    // re-baked on every field edit. exportReport ignores the arguments; the wording
    // is built in C++ (summarizeBake) so this preview and the after-write log agree.
    property var report: (visible && App.active) ? App.active.exportReport(0, 0) : null

    function openFresh() {
        destination = ""
        margin = 8.0
        precision = 4
        open()
    }

    background: Rectangle { color: Theme.panelBg; border.color: Theme.border; radius: 6 }
    header: Rectangle {
        color: Theme.headerBg; height: 34; radius: 6
        Text {
            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
            color: Theme.textBright; font.pixelSize: 13; text: exportDialog.title
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        // Destination: a read-only path with a Browse button opening the save
        // chooser. The dialog does not write until Export, so choosing a path here
        // only records it.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { color: Theme.textSecondary; font.pixelSize: 12; text: qsTr("Destination") }
            Rectangle {
                Layout.fillWidth: true
                height: 26; radius: 4
                color: Theme.fieldBg; border.color: Theme.border
                Text {
                    anchors { left: parent.left; right: parent.right; margins: 6
                              verticalCenter: parent.verticalCenter }
                    elide: Text.ElideLeft
                    color: exportDialog.destination.length > 0 ? Theme.textPrimary : Theme.textDim
                    font.pixelSize: 11
                    text: exportDialog.destination.length > 0 ? exportDialog.destination
                                                              : qsTr("choose a file…")
                }
            }
            Button {
                text: qsTr("Browse…")
                onClicked: destinationDialog.open()
            }
        }

        // Margin and precision — the SvgOptions the writer takes.
        RowLayout {
            spacing: 16
            RowLayout {
                spacing: 6
                Text { color: Theme.textSecondary; font.pixelSize: 12; text: qsTr("Margin") }
                TextField {
                    id: marginField
                    implicitWidth: 70
                    text: exportDialog.margin.toString()
                    color: Theme.textPrimary
                    background: Rectangle { color: Theme.fieldBg; border.color: Theme.border; radius: 3 }
                    onEditingFinished: {
                        var v = parseFloat(text)
                        if (!isNaN(v) && v >= 0) exportDialog.margin = v
                        else text = exportDialog.margin.toString()
                    }
                }
            }
            RowLayout {
                spacing: 6
                Text { color: Theme.textSecondary; font.pixelSize: 12; text: qsTr("Precision") }
                SpinBox {
                    id: precisionBox
                    from: 0; to: 10
                    value: exportDialog.precision
                    onValueModified: exportDialog.precision = value
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        // The loss report, computed from the bake. What survives, then what the
        // bake cost — a bake trades a program for a picture, and the trade is worth
        // reading in the same breath as the result.
        Text {
            color: Theme.textDim; font.pixelSize: 10; text: qsTr("WHAT WILL BE WRITTEN")
        }
        // Survived and lost, both worded in C++ (summarizeBake) so this preview and
        // the after-write reports entry cannot disagree about the same loss.
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary; font.pixelSize: 11
            text: exportDialog.report ? exportDialog.report.survived : qsTr("nothing to export")
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: exportDialog.report && exportDialog.report.lossy
            color: Theme.warn; font.pixelSize: 11
            text: exportDialog.report ? qsTr("lossy: %1").arg(exportDialog.report.lost) : ""
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim; font.pixelSize: 10
            text: qsTr("The background colour is a viewing aid and is never exported.")
        }

        // Actions. Export is dimmed until a destination is chosen; an empty document
        // still exports (a valid empty SVG), and the survived line above says so.
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: exportDialog.close() }
            Button {
                text: qsTr("Export")
                enabled: exportDialog.destination.length > 0 && App.active
                onClicked: {
                    App.active.exportSvg(exportDialog.destination, exportDialog.margin,
                                         exportDialog.precision)
                    exportDialog.close()
                }
            }
        }
    }

    // The destination chooser. Only records the path; the write is exportSvg's.
    FileDialog {
        id: destinationDialog
        title: qsTr("Export destination")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "svg"
        nameFilters: ["SVG images (*.svg)", "All files (*)"]
        currentFolder: "file://" + App.defaultDirectory()
        onAccepted: exportDialog.destination =
            decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""))
    }
}
