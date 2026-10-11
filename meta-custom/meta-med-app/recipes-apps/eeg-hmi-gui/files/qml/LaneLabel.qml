// SPDX-License-Identifier: MIT
//
// What the operator needs to know about one lane without reading the trace:
// which channel, at what scale, how large the signal is, and whether its
// electrodes are on.

import QtQuick

Item {
    id: lane

    property int channel: 0
    property color traceColor: "#4ec9b0"
    property real scaleUv: 100
    property real rmsUv: 0
    // 0 not monitored, 1 attached, 2 detached - "unknown" is a state of its own.
    property int contactPositive: 0
    property int contactNegative: 0
    property bool individualScale: false
    // Level of what arrived, before the display filters, and whether it moved
    // at all. A channel at a rail filters down to a calm line; these two are
    // what keep that from reading as a good electrode.
    property real levelUv: 0
    property bool flat: false

    signal scaleStep(int direction)
    signal scaleReset()

    function amplitude(uv) {
        if (uv >= 1000)
            return (uv / 1000).toFixed(uv >= 10000 ? 0 : 1) + " mV"
        return uv.toFixed(0) + " µV"
    }
    function measured(uv) {
        if (uv >= 1000)
            return (uv / 1000).toFixed(2) + " mV"
        return uv.toFixed(1) + " µV"
    }
    function signedLevel(uv) {
        var sign = uv < 0 ? "−" : "+"
        var size = Math.abs(uv)
        if (size >= 1000)
            return sign + (size / 1000).toFixed(size >= 100000 ? 1 : 2) + " mV"
        return sign + size.toFixed(0) + " µV"
    }
    function pillColor(state) {
        return state === 2 ? "#f85149" : (state === 1 ? "#2ea043" : "#2a3442")
    }

    readonly property bool detached: contactPositive === 2 || contactNegative === 2

    Rectangle {
        anchors.fill: parent
        color: lane.detached ? "#2a1215" : (lane.flat ? "#2a2110" : "transparent")
    }

    Rectangle {
        id: stripe
        width: 4
        radius: 2
        color: lane.traceColor
        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
        height: Math.min(parent.height - 14, 54)
    }

    Column {
        anchors { left: stripe.right; leftMargin: 10; verticalCenter: parent.verticalCenter }
        spacing: 2

        Row {
            spacing: 8
            Text {
                text: "CH" + (lane.channel + 1)
                color: "#e6edf3"
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Row {
                spacing: 3
                anchors.verticalCenter: parent.verticalCenter
                Repeater {
                    model: [{ t: "+", s: lane.contactPositive }, { t: "−", s: lane.contactNegative }]
                    Rectangle {
                        width: 16; height: 16; radius: 4
                        color: lane.pillColor(modelData.s)
                        Text {
                            anchors.centerIn: parent
                            text: modelData.t
                            color: modelData.s === 0 ? "#5d6b7c" : "#ffffff"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                    }
                }
            }
        }
        Text {
            visible: lane.height >= 44
            text: "±" + lane.amplitude(lane.scaleUv)
            color: lane.individualScale ? "#e0af68" : "#8b98a9"
            font.pixelSize: 12
        }
        Text {
            visible: lane.height >= 62
            text: lane.detached ? "ELECTRODE OFF"
                : lane.flat ? "NOT VARYING" : lane.measured(lane.rmsUv) + " rms"
            color: lane.detached ? "#f85149" : (lane.flat ? "#e0af68" : "#6f7d8f")
            font.pixelSize: 11
            font.weight: (lane.detached || lane.flat) ? Font.Bold : Font.Normal
        }
        Text {
            visible: lane.height >= 80
            text: "input at " + lane.signedLevel(lane.levelUv)
            color: lane.flat ? "#e0af68" : "#6f7d8f"
            font.pixelSize: 11
        }
    }

    // Wheel: this lane's scale alone. Double click: every lane the same again.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onWheel: (wheel) => lane.scaleStep(wheel.angleDelta.y > 0 ? -1 : 1)
        onDoubleClicked: lane.scaleReset()
    }
}
