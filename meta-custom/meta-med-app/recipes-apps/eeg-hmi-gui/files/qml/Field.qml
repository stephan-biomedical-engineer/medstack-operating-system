// SPDX-License-Identifier: MIT
//
// One line of text the operator types. Short on purpose: what is typed here
// travels in a fixed field of the control message, and a name that would not
// fit is stopped at the keyboard rather than refused after the fact.

import QtQuick

Rectangle {
    id: field

    property alias text: input.text
    // True while this field has the keyboard, so that the screen can keep
    // its single-key shortcuts away from what is being typed.
    readonly property alias typing: input.activeFocus
    // Marked by whoever owns the field as the one its keys are typing into,
    // which a mouse cannot say by focus alone: clicking a key takes the focus
    // away from the field it is for.
    property bool selected: false
    signal picked()
    property string placeholder: ""
    property int maximumLength: 23
    property bool enabled: true
    signal accepted()

    function focusInput() { input.forceActiveFocus() }

    implicitWidth: 150
    implicitHeight: 30
    radius: 6
    opacity: enabled ? 1.0 : 0.4
    color: "#0d131a"
    border.width: 1
    border.color: (input.activeFocus || field.selected) ? "#3fb6ff" : "#263140"

    Text {
        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
        visible: input.text.length === 0 && !input.activeFocus && !field.selected
        text: field.placeholder
        color: "#5d6b7c"
        font.pixelSize: 13
    }

    TextInput {
        id: input
        anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
        verticalAlignment: TextInput.AlignVCenter
        enabled: field.enabled
        maximumLength: field.maximumLength
        color: "#e6edf3"
        selectionColor: "#3fb6ff"
        font.pixelSize: 13
        clip: true
        selectByMouse: true
        // Enter is "done"; where the keyboard goes next is the owner's to say.
        onAccepted: field.accepted()
        onActiveFocusChanged: if (activeFocus) field.picked()
    }
}
