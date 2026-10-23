// Cluster root. Designed at 1920x720 (12.3" wide cluster) and scaled to window.
import QtQuick
import QtQuick.Window
import Sdv 1.0

Window {
    id: win
    // Injected by the native shell (HmiShell) via setInitialProperties
    required property QtObject vehicle
    required property QtObject ota
    required property QtObject shell
    required property QtObject cockpit   // inter-VM link (available under the hypervisor)

    readonly property bool navShown: cockpit.available && !otaPanel.wanted

    width: 1440; height: 540
    visible: true
    color: theme.background
    title: "SDV Cluster — HMI " + shell.bundleVersion

    Theme { id: theme }

    Item {
        id: stage
        width: 1920; height: 720
        scale: Math.min(win.width / width, win.height / height)
        transformOrigin: Item.TopLeft
        x: (win.width - width * scale) / 2
        y: (win.height - height * scale) / 2

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0; color: theme.backgroundTop }
                GradientStop { position: 1; color: theme.background }
            }
        }

        // ---- top bar -----------------------------------------------------
        Text {
            x: 48; y: 26
            text: win.vehicle.outsideTemp.toFixed(1) + " °C"
            color: theme.textDim; font.pixelSize: 22; font.family: theme.font
        }
        Text {
            id: clock
            anchors.right: parent.right; anchors.rightMargin: 48; y: 26
            color: theme.textDim; font.pixelSize: 22; font.family: theme.font
            Timer { interval: 1000; running: true; repeat: true; triggeredOnStart: true
                onTriggered: clock.text = Qt.formatTime(new Date(), "hh:mm") }
        }
        Telltales {
            theme: theme; vehicle: win.vehicle
            anchors.horizontalCenter: parent.horizontalCenter; y: 20
        }

        // ---- gauges ------------------------------------------------------
        Gauge {
            id: speedo
            theme: theme
            x: 90; y: 90; width: 560; height: 560
            value: win.vehicle.speed
            maxValue: 240; majorStep: 20
            unit: "km/h"; label: theme.speedLabel
        }
        Gauge {
            theme: theme
            x: 1920 - 90 - 560; y: 90; width: 560; height: 560
            value: win.vehicle.power
            minValue: -60; maxValue: 180; zeroValue: 0; majorStep: 30
            unit: "kW"; label: win.vehicle.power < 0 ? "REGEN" : "POWER"
        }

        // ---- centre stack -----------------------------------------------
        Column {
            id: centre
            anchors.horizontalCenter: parent.horizontalCenter
            y: 120
            spacing: 18

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 26
                Repeater {
                    model: ["P", "R", "N", "D"]
                    Text {
                        required property string modelData
                        text: modelData
                        color: win.vehicle.gear === modelData ? theme.accent : theme.textDim
                        opacity: win.vehicle.gear === modelData ? 1 : 0.45
                        font.pixelSize: win.vehicle.gear === modelData ? 64 : 44
                        font.family: theme.font; font.weight: Font.DemiBold
                        anchors.bottom: parent.bottom
                        Behavior on font.pixelSize { NumberAnimation { duration: 150 } }
                    }
                }
            }

            // Drive mode: tap to cycle -> SOME/IP method call SetDriveMode
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 220; height: 46; radius: 23
                color: "transparent"
                border.color: win.vehicle.driveMode === 2 ? theme.warn
                            : win.vehicle.driveMode === 0 ? theme.ok : theme.accent
                border.width: 2
                Text {
                    anchors.centerIn: parent
                    text: win.vehicle.driveModeName
                    color: parent.border.color
                    font.pixelSize: 20; font.letterSpacing: 4; font.family: theme.font
                }
                MouseArea { anchors.fill: parent; onClicked: win.vehicle.setDriveMode((win.vehicle.driveMode + 1) % 3) }
            }

            Item { width: 1; height: 10 }

            Loader {
                id: efficiency
                anchors.horizontalCenter: parent.horizontalCenter
                active: theme.showEfficiency && !otaPanel.wanted && !win.navShown
                source: "EfficiencyPanel.qml"
                onLoaded: { item.theme = theme; item.vehicle = win.vehicle }
            }

            Grid {
                visible: !otaPanel.wanted && !theme.showEfficiency && !win.navShown
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 2; columnSpacing: 60; rowSpacing: 6
                Text { text: "TRIP"; color: theme.textDim; font.pixelSize: 16; font.letterSpacing: 3; font.family: theme.font }
                Text { text: "ODO"; color: theme.textDim; font.pixelSize: 16; font.letterSpacing: 3; font.family: theme.font }
                Text { text: win.vehicle.trip.toFixed(1) + " km"; color: theme.text; font.pixelSize: 30; font.family: theme.font }
                Text { text: Math.floor(win.vehicle.odometer) + " km"; color: theme.text; font.pixelSize: 30; font.family: theme.font }
            }
        }

        // Turn-by-turn projected from the IVI partition (or the cluster-native fallback)
        NavTile {
            theme: theme; cockpit: win.cockpit
            visible: win.navShown
            anchors.horizontalCenter: parent.horizontalCenter
            y: 274
        }

        OtaPanel {
            id: otaPanel
            theme: theme; ota: win.ota; vehicle: win.vehicle; shell: win.shell
            anchors.horizontalCenter: parent.horizontalCenter
            y: 300
        }

        // ---- bottom bar --------------------------------------------------
        Item {
            id: bottom
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 40 }
            height: 56

            Row {
                spacing: 16
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    width: 260; height: 14; radius: 7; color: theme.track
                    anchors.verticalCenter: parent.verticalCenter
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, win.vehicle.soc / 100))
                        height: parent.height; radius: 7
                        color: win.vehicle.soc < 15 ? theme.caution : theme.ok
                    }
                }
                Text { text: win.vehicle.soc.toFixed(0) + " %"; color: theme.text; font.pixelSize: 24; font.family: theme.font }
                Text { text: win.vehicle.range.toFixed(0) + " km range"; color: theme.textDim; font.pixelSize: 22
                       font.family: theme.font; anchors.verticalCenter: parent.verticalCenter }
            }

            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: "HMI " + win.shell.bundleVersion + " · " + theme.name + " · slot " + win.shell.bundleSlot
                      + "   |   shell " + win.shell.shellVersion
                      + (win.vehicle.vin ? "   |   " + win.vehicle.vin : "")
                color: theme.textDim; font.pixelSize: 16; font.family: theme.font
            }
        }

        // ---- signal-loss overlay (SOME/IP subscription lost) -------------
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.86)
            visible: !win.vehicle.connected
            Column {
                anchors.centerIn: parent; spacing: 10
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Vehicle signals unavailable"
                       color: theme.caution; font.pixelSize: 34; font.family: theme.font }
                Text { anchors.horizontalCenter: parent.horizontalCenter
                       text: "Waiting for VehicleSignals (0x1001) over SOME/IP…"
                       color: theme.textDim; font.pixelSize: 20; font.family: theme.font }
            }
        }
    }
}
