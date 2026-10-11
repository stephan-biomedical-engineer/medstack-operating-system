// SPDX-License-Identifier: MIT
//
// EEG operator display.
//
// Judged on legibility and on never showing something it is not sure of. Every
// control on this screen changes how the stream is DRAWN - the window, the
// scale, the display filters, a frozen picture. None of them reaches the
// acquisition service, and the record is the same whatever is pressed here.

import QtQuick
import QtQuick.Window
import MedPlatform.Eeg

Window {
    id: root
    visible: true
    visibility: Window.Maximized
    width: 1280
    height: 720
    minimumWidth: 900
    minimumHeight: 520
    color: theme.background
    title: "MedPlatform EEG"

    QtObject {
        id: theme
        readonly property color background: "#0b0f14"
        readonly property color panel: "#111821"
        readonly property color line: "#1e2733"
        readonly property color text: "#e6edf3"
        readonly property color dim: "#8b98a9"
        readonly property color faint: "#6f7d8f"
        readonly property color accent: "#3fb6ff"
        readonly property color good: "#3fb950"
        readonly property color warn: "#d29922"
        readonly property color bad: "#f85149"
        readonly property var traces: ["#4ec9b0", "#7aa2f7", "#e0af68", "#bb9af7",
                                       "#9ece6a", "#f7768e", "#7dcfff", "#ff9e64"]
    }

    // --- scale ---------------------------------------------------------------
    // Fixed steps the operator chooses, never a trace that rescales itself: an
    // amplitude that silently changes makes two screens incomparable.
    readonly property var scaleSteps: [10, 20, 50, 100, 200, 500, 1000, 2000, 5000,
                                       10000, 20000, 50000, 100000, 200000]
    property real commonScale: 100
    // Per channel; 0 means "the common one".
    property var individual: []

    function scaleOf(channel) {
        return individual[channel] > 0 ? individual[channel] : commonScale
    }
    function effectiveScales() {
        var out = []
        for (var c = 0; c < eeg.channelCount; ++c)
            out.push(scaleOf(c))
        return out
    }
    function stepped(value, direction) {
        var index = 0
        for (var i = 0; i < scaleSteps.length; ++i)
            if (scaleSteps[i] <= value)
                index = i
        index = Math.max(0, Math.min(scaleSteps.length - 1, index + direction))
        return scaleSteps[index]
    }
    function stepCommon(direction) {
        commonScale = stepped(commonScale, direction)
    }
    function stepChannel(channel, direction) {
        var next = individual.slice()
        next[channel] = stepped(scaleOf(channel), direction)
        individual = next
    }
    function resetChannel(channel) {
        var next = individual.slice()
        next[channel] = 0
        individual = next
    }
    // One shot, on request: each lane gets the smallest step that holds what is
    // in its window now. It does not keep following the signal afterwards.
    function fitAll() {
        var next = []
        for (var c = 0; c < eeg.channelCount; ++c) {
            var wanted = eeg.peakUv(c) * 1.25
            var chosen = scaleSteps[scaleSteps.length - 1]
            for (var i = 0; i < scaleSteps.length; ++i) {
                if (scaleSteps[i] >= wanted) {
                    chosen = scaleSteps[i]
                    break
                }
            }
            next.push(chosen)
        }
        individual = next
    }
    function amplitude(uv) {
        if (uv >= 1000)
            return (uv / 1000).toFixed(uv >= 10000 ? 0 : 1) + " mV"
        return uv.toFixed(0) + " µV"
    }
    function contact(monitored, detached, channel) {
        var bit = 1 << channel
        if ((monitored & bit) === 0)
            return 0
        return (detached & bit) !== 0 ? 2 : 1
    }

    readonly property bool rateWrong: eeg.connected && eeg.rateMeasured && eeg.sampleRateHz > 0
        && Math.abs(eeg.deliveredRateHz - eeg.sampleRateHz) > 0.02 * eeg.sampleRateHz

    Connections {
        target: eeg
        function onLayoutChanged() { root.individual = [] }
    }

    Shortcut { sequence: "Space"; onActivated: eeg.frozen = !eeg.frozen }
    Shortcut { sequences: ["+", "="]; onActivated: root.stepCommon(-1) }
    Shortcut { sequence: "-"; onActivated: root.stepCommon(1) }
    Shortcut { sequence: "A"; onActivated: root.fitAll() }
    Shortcut {
        sequence: "F11"
        onActivated: root.visibility = root.visibility === Window.FullScreen ? Window.Maximized
                                                                              : Window.FullScreen
    }

    // --- header --------------------------------------------------------------
    Rectangle {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 64
        color: theme.panel

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: theme.line
        }

        Row {
            anchors { left: parent.left; leftMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 14

            Rectangle {
                width: 12; height: 12; radius: 6
                anchors.verticalCenter: parent.verticalCenter
                // Green only while a stream is attached and nothing is wrong
                // with it. The operator must never read "connected" as "fine".
                color: !eeg.connected ? theme.warn
                     : (eeg.electrodeOff || root.rateWrong) ? theme.bad : theme.good
            }
            Text {
                text: "EEG"
                color: theme.text
                font.pixelSize: 22
                font.weight: Font.Bold
                font.letterSpacing: 1.5
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                width: 1; height: 26; color: theme.line
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: eeg.status
                color: eeg.connected ? theme.dim : theme.warn
                font.pixelSize: 14
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                visible: eeg.electrodeOff
                anchors.verticalCenter: parent.verticalCenter
                radius: 4
                color: theme.bad
                width: offText.implicitWidth + 16
                height: 24
                Text {
                    id: offText
                    anchors.centerIn: parent
                    text: "ELECTRODE OFF"
                    color: "#ffffff"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                }
            }
        }

        Row {
            anchors { right: parent.right; rightMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 28

            Stat {
                label: "CHANNELS"
                value: eeg.channelCount > 0 ? eeg.channelCount : "—"
                valueWidth: 40
            }
            Stat {
                // The delivered rate, large, and the declared one beside it. A
                // display that printed only the header's number would repeat
                // on screen the claim the stream may not be keeping.
                label: "SAMPLES / S DELIVERED"
                value: !eeg.connected ? "—"
                     : eeg.rateMeasured ? eeg.deliveredRateHz.toFixed(1) : "measuring"
                note: eeg.sampleRateHz > 0 ? "of " + eeg.sampleRateHz.toFixed(0) + " declared" : ""
                valueColor: root.rateWrong ? theme.bad : theme.text
                valueWidth: 92
            }
            Stat {
                label: "FRAMES MISSED"
                value: eeg.framesMissed
                valueColor: eeg.framesMissed > 0 ? theme.warn : theme.text
                valueWidth: 70
            }
            Stat {
                label: "REJECTED"
                value: eeg.framesRejected
                valueColor: eeg.framesRejected > 0 ? theme.bad : theme.text
                valueWidth: 56
            }
        }
    }

    // --- lanes ---------------------------------------------------------------
    Rectangle {
        id: gutter
        anchors { top: header.bottom; bottom: ruler.top; left: parent.left }
        width: 168
        color: theme.panel

        Rectangle {
            anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
            width: 1
            color: theme.line
        }

        Column {
            anchors.fill: parent
            Repeater {
                model: eeg.channelCount
                LaneLabel {
                    width: gutter.width - 1
                    height: gutter.height / Math.max(1, eeg.channelCount)
                    channel: index
                    traceColor: theme.traces[index % theme.traces.length]
                    scaleUv: root.scaleOf(index)
                    individualScale: root.individual[index] > 0
                    rmsUv: index < eeg.rms.length ? eeg.rms[index] : 0
                    levelUv: index < eeg.level.length ? eeg.level[index] : 0
                    flat: index < eeg.flat.length ? eeg.flat[index] : false
                    contactPositive: root.contact(eeg.monitoredPositive, eeg.detachedPositive, index)
                    contactNegative: root.contact(eeg.monitoredNegative, eeg.detachedNegative, index)
                    onScaleStep: (direction) => root.stepChannel(index, direction)
                    onScaleReset: root.resetChannel(index)
                }
            }
        }
    }

    WaveformItem {
        id: plot
        anchors { top: header.bottom; bottom: ruler.top; left: gutter.right; right: parent.right }
        client: eeg
        scales: root.effectiveScales()
        colors: theme.traces
        gridColor: theme.line
        cursorColor: eeg.frozen ? theme.warn : theme.accent
        clip: true

        // Wheel over a lane: that lane's scale.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            onWheel: (wheel) => {
                if (eeg.channelCount <= 0)
                    return
                var lane = Math.floor(wheel.y / (height / eeg.channelCount))
                lane = Math.max(0, Math.min(eeg.channelCount - 1, lane))
                root.stepChannel(lane, wheel.angleDelta.y > 0 ? -1 : 1)
            }
        }

        Text {
            anchors.centerIn: parent
            visible: eeg.channelCount <= 0
            text: "no signal"
            color: theme.faint
            font.pixelSize: 26
        }

        Rectangle {
            visible: eeg.frozen
            anchors { top: parent.top; topMargin: 14; horizontalCenter: parent.horizontalCenter }
            radius: 6
            color: "#3a2c0c"
            border.color: theme.warn
            border.width: 1
            width: frozenText.implicitWidth + 28
            height: 34
            Text {
                id: frozenText
                anchors.centerIn: parent
                text: "PICTURE FROZEN  ·  acquisition and recording continue"
                color: "#f0c674"
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
        }
    }

    // --- time ruler ----------------------------------------------------------
    Item {
        id: ruler
        anchors { bottom: toolbar.top; left: gutter.right; right: parent.right }
        height: 20

        Rectangle { anchors.fill: parent; color: theme.background }
        Repeater {
            model: eeg.windowSeconds > 30 ? Math.floor(eeg.windowSeconds / 5) : Math.floor(eeg.windowSeconds)
            Text {
                readonly property real seconds: (index + 1) * (eeg.windowSeconds > 30 ? 5 : 1)
                visible: seconds < eeg.windowSeconds
                x: seconds / eeg.windowSeconds * ruler.width - width - 4
                anchors.verticalCenter: parent.verticalCenter
                text: seconds + " s"
                color: theme.faint
                font.pixelSize: 10
            }
        }
    }
    Rectangle {
        anchors { bottom: toolbar.top; left: parent.left; right: gutter.right }
        height: ruler.height
        color: theme.panel
        Rectangle {
            anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
            width: 1
            color: theme.line
        }
    }

    // --- toolbar -------------------------------------------------------------
    Rectangle {
        id: toolbar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: 58
        color: theme.panel

        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 1
            color: theme.line
        }

        Row {
            anchors { left: parent.left; leftMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 26

            Row {
                spacing: 6
                Text {
                    text: "WINDOW"; color: theme.faint; font.pixelSize: 10
                    font.letterSpacing: 1.2; font.weight: Font.DemiBold
                    anchors.verticalCenter: parent.verticalCenter
                    rightPadding: 4
                }
                Repeater {
                    model: [5, 10, 20, 30]
                    Chip {
                        text: modelData + " s"
                        checked: eeg.windowSeconds === modelData
                        onClicked: eeg.windowSeconds = modelData
                    }
                }
            }

            Row {
                spacing: 6
                Text {
                    text: "SCALE"; color: theme.faint; font.pixelSize: 10
                    font.letterSpacing: 1.2; font.weight: Font.DemiBold
                    anchors.verticalCenter: parent.verticalCenter
                    rightPadding: 4
                }
                Chip { text: "−"; onClicked: root.stepCommon(1) }
                Rectangle {
                    width: 92; height: 30; radius: 6
                    color: "#0d131a"
                    border.width: 1; border.color: theme.line
                    Text {
                        anchors.centerIn: parent
                        text: "±" + root.amplitude(root.commonScale)
                        color: theme.text
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                }
                Chip { text: "+"; onClicked: root.stepCommon(-1) }
                Chip { text: "Fit"; enabled: eeg.channelCount > 0; onClicked: root.fitAll() }
                Chip {
                    text: "Same for all"
                    enabled: root.individual.some(function (v) { return v > 0 })
                    onClicked: root.individual = []
                }
            }

            Row {
                spacing: 6
                Text {
                    text: "DISPLAY FILTERS"; color: theme.faint; font.pixelSize: 10
                    font.letterSpacing: 1.2; font.weight: Font.DemiBold
                    anchors.verticalCenter: parent.verticalCenter
                    rightPadding: 4
                }
                Chip {
                    text: "High-pass 0.5 Hz"
                    checked: eeg.highPassHz > 0
                    onClicked: eeg.highPassHz = eeg.highPassHz > 0 ? 0 : 0.5
                }
                Chip {
                    text: "Notch 50 Hz"
                    checked: eeg.notchHz === 50
                    enabled: eeg.sampleRateHz === 0 || eeg.sampleRateHz > 100
                    onClicked: eeg.notchHz = eeg.notchHz === 50 ? 0 : 50
                }
                Chip {
                    text: "Notch 60 Hz"
                    checked: eeg.notchHz === 60
                    enabled: eeg.sampleRateHz === 0 || eeg.sampleRateHz > 120
                    onClicked: eeg.notchHz = eeg.notchHz === 60 ? 0 : 60
                }
            }
        }

        Row {
            anchors { right: parent.right; rightMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 16

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "filters and scale change the picture, never the record"
                color: theme.faint
                font.pixelSize: 11
                visible: toolbar.width > 1500
            }
            Chip {
                text: eeg.frozen ? "Resume" : "Freeze"
                checked: eeg.frozen
                accent: theme.warn
                onClicked: eeg.frozen = !eeg.frozen
            }
        }
    }
}
