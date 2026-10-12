// SPDX-License-Identifier: MIT
//
// The front-end's settings, chosen by the operator.
//
// Everything on this panel comes from the sealed table of choices: which
// settings exist, which values each can take, and whether the combination
// picked is one this link offers. The panel knows the names it shows them
// under and nothing else about them. "Apply" asks the acquisition service,
// which may refuse - and nothing here changes until it has answered.

import QtQuick

Rectangle {
    id: panel

    // What the service says is in force, and what the operator has picked
    // here and not yet asked for.
    // The row in force. Read again whenever the service is asked, which is
    // every second; what the operator has picked must survive an answer that
    // changed nothing, so the picks are dropped only when the row is another.
    property int seenChoice: -2
    property var current: prescriptions.choice(sessions.choice)
    property var pending: ({})
    property string affirmed: ""

    signal closeRequested()

    readonly property var names: ({
        "afe.gain": "Gain",
        "afe.lead_off_detection": "Electrode contact (lead-off) detection",
        "afe.montage": "Montage",
        "afe.test_signal": "Internal test signal"
    })
    readonly property var words: ({
        "true": "on", "false": "off",
        "off": "off", "internal": "on",
        "bipolar": "bipolar", "referential": "referential"
    })

    function picked(key) {
        return pending[key] !== undefined ? pending[key] : current[key]
    }
    function wanted() {
        var out = {}
        for (var i = 0; i < prescriptions.keys.length; ++i)
            out[prescriptions.keys[i]] = picked(prescriptions.keys[i])
        return out
    }
    function pick(key, value) {
        var next = {}
        for (var k in pending)
            next[k] = pending[k]
        next[key] = value
        pending = next
    }
    function label(key, value) {
        if (key === "afe.gain")
            return "×" + value
        return words[value] !== undefined ? words[value] : value
    }
    // The word the table says must be stated with this change, if any.
    function owed() {
        for (var i = 0; i < prescriptions.keys.length; ++i) {
            var key = prescriptions.keys[i]
            if (picked(key) !== current[key] && prescriptions.affirmation(key).length > 0)
                return prescriptions.affirmation(key)
        }
        return ""
    }

    readonly property bool differs: {
        for (var i = 0; i < prescriptions.keys.length; ++i)
            if (picked(prescriptions.keys[i]) !== current[prescriptions.keys[i]])
                return true
        return false
    }
    readonly property bool offered: prescriptions.indexOf(wanted()) >= 0
    readonly property string owedWord: owed()

    // Dropped from a signal handler and not from a property's own change
    // handler: the first version reset the picks while the bindings that read
    // them were still being evaluated, and the engine reported a binding loop
    // on the board's first start.
    Connections {
        target: sessions
        function onChanged() {
            if (sessions.choice !== panel.seenChoice) {
                panel.seenChoice = sessions.choice
                panel.pending = ({})
                panel.affirmed = ""
            }
        }
    }

    width: 560
    height: body.implicitHeight + 40
    radius: 10
    color: "#111821"
    border.width: 1
    border.color: "#263140"

    // A click on the panel is not a click on what is under it.
    MouseArea { anchors.fill: parent }

    Column {
        id: body
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
        spacing: 16

        Item {
            width: parent.width
            height: 26
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Front-end settings"
                color: "#e6edf3"
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            Chip {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: "Close"
                onClicked: panel.closeRequested()
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            visible: sessions.recording
            text: "A session is being recorded. Its record states one set of settings, so they "
                + "cannot change until it is closed."
            color: "#e0af68"
            font.pixelSize: 13
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            visible: sessions.choice < 0
            text: "The service is running a prescription that is not in this device's table "
                + "of choices. Choosing one below replaces it."
            color: "#8b98a9"
            font.pixelSize: 13
        }

        Repeater {
            model: prescriptions.keys
            Column {
                id: setting
                readonly property string key: modelData
                width: body.width
                spacing: 6
                Text {
                    text: panel.names[setting.key] !== undefined ? panel.names[setting.key]
                                                                 : setting.key
                    color: "#8b98a9"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                Flow {
                    width: parent.width
                    spacing: 6
                    Repeater {
                        model: prescriptions.values(setting.key)
                        Chip {
                            text: panel.label(setting.key, modelData)
                            checked: panel.picked(setting.key) === modelData
                            enabled: !sessions.recording
                            onClicked: panel.pick(setting.key, modelData)
                        }
                    }
                }
            }
        }

        // The other half of a setting that software cannot see. The words
        // are the request's; what they mean on this board is said beside them.
        Column {
            width: parent.width
            spacing: 6
            visible: panel.owedWord.length > 0
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "This change has a manual half: the front-end board's reference switch. "
                    + "Set it, then say where it is. It goes into the record as you state it - "
                    + "nothing can check it."
                color: "#e0af68"
                font.pixelSize: 13
            }
            Row {
                spacing: 6
                Chip {
                    text: "switch at ground (GNDA) - bipolar"
                    checked: panel.affirmed === "ground"
                    accent: "#e0af68"
                    onClicked: panel.affirmed = "ground"
                }
                Chip {
                    text: "switch at the reference electrode - referential"
                    checked: panel.affirmed === "electrode"
                    accent: "#e0af68"
                    onClicked: panel.affirmed = "electrode"
                }
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            visible: panel.differs && !panel.offered
            text: "This combination is not offered on this link."
            color: "#f85149"
            font.pixelSize: 13
        }

        Item {
            width: parent.width
            height: 30
            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                width: parent.width - 260
                elide: Text.ElideRight
                text: sessions.message
                color: sessions.refused ? "#f85149" : "#8b98a9"
                font.pixelSize: 12
            }
            Row {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                spacing: 6
                Chip {
                    text: "Factory settings"
                    enabled: !sessions.recording && prescriptions.factory >= 0
                             && sessions.choice !== prescriptions.factory
                    onClicked: { panel.pending = prescriptions.choice(prescriptions.factory); panel.affirmed = "" }
                }
                Chip {
                    text: "Apply"
                    checked: enabled
                    enabled: !sessions.recording && panel.differs && panel.offered
                             && (panel.owedWord.length === 0 || panel.affirmed.length > 0)
                    onClicked: {
                        var request = {}
                        for (var i = 0; i < prescriptions.keys.length; ++i) {
                            var key = prescriptions.keys[i]
                            if (panel.picked(key) !== panel.current[key])
                                request[key] = panel.picked(key)
                        }
                        if (panel.owedWord.length > 0)
                            request[panel.owedWord] = panel.affirmed
                        sessions.configure(request)
                    }
                }
            }
        }
    }
}
