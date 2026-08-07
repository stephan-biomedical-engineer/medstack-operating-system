// SPDX-License-Identifier: MIT
//
// EEG operator display. Deliberately plain: a clinical waveform screen is
// judged on legibility and on never showing something it is not sure of, not
// on decoration.

import QtQuick
import QtQuick.Window

Window {
    id: root
    visible: true
    width: 1024
    height: 600
    color: "#0d1117"
    title: "MedPlatform EEG"

    readonly property color traceColor: "#4ec9b0"
    readonly property color gridColor: "#1f2937"
    readonly property int headerHeight: 56
    readonly property int footerHeight: 32

    // Header ----------------------------------------------------------------
    Rectangle {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: root.headerHeight
        color: "#161b22"

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 16
            spacing: 12

            Rectangle {
                width: 12; height: 12
                radius: 6
                anchors.verticalCenter: parent.verticalCenter
                // Green only while frames are actually arriving; anything else
                // is amber. The operator must never read "connected" as
                // "acquiring".
                color: eeg.connected ? "#3fb950" : "#d29922"
            }

            Text {
                text: "EEG"
                color: "#e6edf3"
                font.pixelSize: 20
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                text: eeg.status
                color: "#8b949e"
                font.pixelSize: 14
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 16
            color: "#8b949e"
            font.pixelSize: 13
            text: eeg.channelCount + " ch @ " + eeg.sampleRateHz.toFixed(0) + " Hz"
        }
    }

    // Waveforms -------------------------------------------------------------
    Canvas {
        id: plot
        anchors {
            top: header.bottom
            left: parent.left
            right: parent.right
            bottom: footer.top
        }
        antialiasing: true

        onPaint: {
            var context = getContext("2d");
            context.reset();
            context.fillStyle = "#0d1117";
            context.fillRect(0, 0, width, height);

            var channels = eeg.channelCount;
            if (channels <= 0) {
                context.fillStyle = "#8b949e";
                context.font = "16px sans-serif";
                context.fillText("no signal", 16, 32);
                return;
            }

            var laneHeight = height / channels;

            // One lane per electrode, with a baseline and a fixed +/-100 uV
            // scale. Fixed, not auto-scaled: an amplitude that silently
            // rescales makes two screens incomparable.
            var fullScaleUv = 100.0;

            for (var channel = 0; channel < channels; ++channel) {
                var baseline = laneHeight * (channel + 0.5);

                context.strokeStyle = root.gridColor;
                context.lineWidth = 1;
                context.beginPath();
                context.moveTo(0, baseline);
                context.lineTo(width, baseline);
                context.stroke();

                context.fillStyle = "#6e7681";
                context.font = "11px sans-serif";
                context.fillText("ch" + channel, 6, baseline - laneHeight * 0.32);

                var samples = eeg.waveform(channel);
                if (samples.length < 2) {
                    continue;
                }

                var step = width / (samples.length - 1);
                var gain = (laneHeight * 0.45) / fullScaleUv;

                context.strokeStyle = root.traceColor;
                context.lineWidth = 1.2;
                context.beginPath();
                for (var i = 0; i < samples.length; ++i) {
                    var y = baseline - samples[i] * gain;
                    if (i === 0) {
                        context.moveTo(0, y);
                    } else {
                        context.lineTo(i * step, y);
                    }
                }
                context.stroke();
            }
        }

        Connections {
            target: eeg
            function onFrameReceived() { plot.requestPaint(); }
        }
    }

    // Footer ----------------------------------------------------------------
    Rectangle {
        id: footer
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: root.footerHeight
        color: "#161b22"

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 16
            color: "#6e7681"
            font.pixelSize: 12
            text: "frames " + eeg.framesReceived + "   rejected " + eeg.framesRejected
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 16
            color: "#6e7681"
            font.pixelSize: 12
            text: "±100 µV per lane"
        }
    }
}
