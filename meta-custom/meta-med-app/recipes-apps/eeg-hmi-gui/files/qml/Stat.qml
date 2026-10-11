// SPDX-License-Identifier: MIT
//
// One number with its name above it. The value has a fixed width so that a
// changing digit does not shift everything to its right four times a second.

import QtQuick

Column {
    id: stat

    property string label: ""
    property string value: ""
    property string note: ""
    property color valueColor: "#e6edf3"
    property int valueWidth: 96

    spacing: 1

    Text {
        text: stat.label
        color: "#6f7d8f"
        font.pixelSize: 10
        font.letterSpacing: 1.2
        font.weight: Font.DemiBold
    }
    Row {
        spacing: 6
        Text {
            id: number
            width: stat.valueWidth
            text: stat.value
            color: stat.valueColor
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
        Text {
            visible: stat.note.length > 0
            anchors.baseline: number.baseline
            text: stat.note
            color: "#6f7d8f"
            font.pixelSize: 11
        }
    }
}
