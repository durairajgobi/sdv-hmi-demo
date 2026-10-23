// OTA status card shown in the centre stack. Driver consent is only accepted
// while parked; download/verify/install run in the background via ota-agent.
import QtQuick
import Sdv 1.0

Rectangle {
    id: card
    required property QtObject theme
    required property QtObject ota
    required property QtObject vehicle
    required property QtObject shell

    readonly property int st: ota.state
    readonly property bool busy: st === OtaClient.Downloading || st === OtaClient.Verifying
                                 || st === OtaClient.Installing || st === OtaClient.Activating
    readonly property bool parked: vehicle.gear === "P"
    property int dismissedState: -1
    readonly property bool wanted: ota.connected && dismissedState !== st && (
                                       st === OtaClient.UpdateAvailable || busy
                                       || st === OtaClient.Success || st === OtaClient.RolledBack
                                       || st === OtaClient.Failed)

    onStChanged: { dismissedState = -1; autoHide.restart() }
    Timer {
        id: autoHide
        interval: card.st === OtaClient.Success ? 7000 : 12000
        running: card.st === OtaClient.Success || card.st === OtaClient.RolledBack || card.st === OtaClient.Failed
        onTriggered: card.dismissedState = card.st
    }

    readonly property color tone: st === OtaClient.RolledBack ? theme.caution
                                : st === OtaClient.Failed ? theme.warn
                                : st === OtaClient.Success ? theme.ok : theme.accent

    width: 560; height: column.implicitHeight + 40
    radius: 18
    color: theme.panel
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.7)
    border.width: 1.5
    opacity: wanted ? 1 : 0
    visible: opacity > 0
    scale: wanted ? 1 : 0.94
    Behavior on opacity { NumberAnimation { duration: 220 } }
    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

    Column {
        id: column
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 22 }
        spacing: 12

        Row {
            spacing: 10
            Rectangle { width: 10; height: 10; radius: 5; color: card.tone; anchors.verticalCenter: parent.verticalCenter
                SequentialAnimation on opacity { running: card.busy; loops: Animation.Infinite
                    NumberAnimation { to: 0.2; duration: 500 } NumberAnimation { to: 1; duration: 500 } }
            }
            Text {
                text: {
                    switch (card.st) {
                    case OtaClient.UpdateAvailable: return "Software update available"
                    case OtaClient.Downloading: return "Downloading update"
                    case OtaClient.Verifying: return "Verifying signature & integrity"
                    case OtaClient.Installing: return "Installing to slot " + (card.ota.activeSlot === "A" ? "B" : "A")
                    case OtaClient.Activating: return "Activating new software"
                    case OtaClient.Success: return "Update complete"
                    case OtaClient.RolledBack: return "Update rolled back"
                    case OtaClient.Failed: return "Update not installed"
                    }
                    return ""
                }
                color: card.theme.text
                font.pixelSize: 24; font.family: card.theme.font
            }
        }

        Text {
            width: parent.width
            // Driver-facing wording; technical detail stays in the agent log / diagnostics
            text: {
                switch (card.st) {
                case OtaClient.Success:
                    return "Cluster software " + card.ota.installedVersion + " is running (slot " + card.ota.activeSlot + ")"
                case OtaClient.RolledBack:
                    return "Version " + card.ota.availableVersion + " did not pass the start-up health check. "
                         + "Your cluster was restored to " + card.ota.installedVersion + " automatically."
                case OtaClient.Failed:
                    return card.ota.message.indexOf("manifest rejected") >= 0
                         ? "The update could not be authenticated and was discarded. Nothing was changed."
                         : card.ota.message.indexOf("Integrity") >= 0
                           ? "The download was corrupted and has been discarded. Nothing was changed."
                           : card.ota.message
                }
                return "Version " + card.ota.installedVersion + "  →  " + card.ota.availableVersion
            }
            color: card.theme.textDim
            wrapMode: Text.WordWrap
            font.pixelSize: 18; font.family: card.theme.font
        }

        Text {
            visible: card.st === OtaClient.UpdateAvailable && card.ota.releaseNotes.length > 0
            width: parent.width
            text: card.ota.releaseNotes
            color: card.theme.text
            wrapMode: Text.WordWrap
            maximumLineCount: 4; elide: Text.ElideRight
            font.pixelSize: 17; font.family: card.theme.font
        }

        // progress
        Rectangle {
            visible: card.busy
            width: parent.width; height: 8; radius: 4
            color: card.theme.track
            Rectangle {
                width: parent.width * (card.st === OtaClient.Activating ? 1 : card.ota.progress / 100)
                height: parent.height; radius: 4; color: card.tone
                Behavior on width { NumberAnimation { duration: 200 } }
            }
        }
        Text {
            visible: card.busy
            text: card.st === OtaClient.Activating ? "Health check in progress…" : card.ota.progress + " %   " + card.ota.message
            color: card.theme.textDim; font.pixelSize: 15; font.family: card.theme.font
        }

        Row {
            visible: card.st === OtaClient.UpdateAvailable
            spacing: 14
            Button2 {
                theme: card.theme
                text: card.parked ? "Install now" : "Install when parked (P)"
                primary: true; enabled: card.parked
                onClicked: card.ota.approveInstall()
            }
            Button2 { theme: card.theme; text: "Later"; onClicked: { card.ota.defer(); card.dismissedState = card.st } }
        }
    }

    component Button2: Rectangle {
        id: b
        required property QtObject theme
        property string text
        property bool primary: false
        signal clicked()
        width: lbl.implicitWidth + 44; height: 44; radius: 22
        color: primary && enabled ? theme.accent : "transparent"
        border.color: enabled ? theme.accent : theme.track; border.width: 1.5
        opacity: enabled ? 1 : 0.6
        Text { id: lbl; anchors.centerIn: parent; text: b.text; font.pixelSize: 17; font.family: b.theme.font
               color: b.primary && b.enabled ? b.theme.background : b.theme.text }
        MouseArea { anchors.fill: parent; enabled: b.enabled; onClicked: b.clicked() }
    }
}
