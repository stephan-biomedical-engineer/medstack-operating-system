// SPDX-License-Identifier: MIT
//
// A button that shows whether it is on. Used for every choice on the toolbar,
// so that the state of the display can be read off the screen without
// remembering what was pressed.

import QtQuick

Rectangle {
    id: chip

    property string text: ""
    property bool checked: false
    property bool enabled: true
    property color accent: "#3fb6ff"
    signal clicked()

    implicitWidth: label.implicitWidth + 24
    implicitHeight: 30
    radius: 6
    opacity: enabled ? 1.0 : 0.4
    color: checked ? Qt.rgba(accent.r, accent.g, accent.b, 0.18)
                   : (area.containsMouse ? "#1b2430" : "#151c26")
    border.width: 1
    border.color: checked ? accent : (area.containsMouse ? "#3a4758" : "#263140")

    Text {
        id: label
        anchors.centerIn: parent
        text: chip.text
        color: chip.checked ? "#e6edf3" : "#aab6c5"
        font.pixelSize: 13
        font.weight: chip.checked ? Font.DemiBold : Font.Normal
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        enabled: chip.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: chip.clicked()
    }
}
