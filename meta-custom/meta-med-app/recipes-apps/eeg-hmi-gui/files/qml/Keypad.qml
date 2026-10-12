// SPDX-License-Identifier: MIT
//
// Keys to type a short name with a mouse.
//
// The station may have no keyboard. The compositor it first ran on raised an
// on-screen one of its own for any text field, and that one could not be
// relied on: over the display's text fields its backspace did nothing, it
// covered what was being typed, and it had no key to put it away. The
// compositor the station is moving to has none at all. So the display brings
// the few keys it needs, as part of the panel that needs them, and they behave
// the same under any compositor.
//
// It edits the END of the field it is pointed at, which is all a name of
// twenty characters needs. A real keyboard, when there is one, types into the
// same field as before.

import QtQuick

Column {
    id: keypad

    // The Field being typed into.
    property var target: null
    property bool upper: false
    signal done()

    readonly property var rows: [
        ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
        ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
        ["a", "s", "d", "f", "g", "h", "j", "k", "l", "-"],
        ["z", "x", "c", "v", "b", "n", "m", ".", "_"]
    ]

    function type(character) {
        if (target === null || target.text.length >= target.maximumLength)
            return
        target.text = target.text + (upper ? character.toUpperCase() : character)
    }
    function erase() {
        if (target !== null && target.text.length > 0)
            target.text = target.text.substring(0, target.text.length - 1)
    }

    spacing: 6

    Repeater {
        model: keypad.rows
        Row {
            spacing: 6
            anchors.horizontalCenter: parent.horizontalCenter
            Repeater {
                model: modelData
                Chip {
                    implicitWidth: 48
                    implicitHeight: 40
                    text: keypad.upper ? modelData.toUpperCase() : modelData
                    onClicked: keypad.type(modelData)
                }
            }
        }
    }

    Row {
        spacing: 6
        anchors.horizontalCenter: parent.horizontalCenter
        Chip {
            implicitWidth: 102
            implicitHeight: 40
            text: "Shift"
            checked: keypad.upper
            onClicked: keypad.upper = !keypad.upper
        }
        Chip {
            implicitWidth: 264
            implicitHeight: 40
            text: "space"
            onClicked: keypad.type(" ")
        }
        Chip {
            implicitWidth: 102
            implicitHeight: 40
            text: "⌫  erase"
            onClicked: keypad.erase()
        }
        Chip {
            implicitWidth: 48
            implicitHeight: 40
            text: "✓"
            accent: "#3fb950"
            checked: true
            onClicked: keypad.done()
        }
    }
}
