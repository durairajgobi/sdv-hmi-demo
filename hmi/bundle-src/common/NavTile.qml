// Navigation tile with graceful degradation across the partition boundary.
//   Healthy     : pixels projected by the IVI partition (validated by the shell)
//   Degraded    : IVI frames stalled/corrupt but guidance data still valid ->
//                 the cluster renders a minimal native guidance itself
//   Starting /
//   Unavailable : no guidance; say so instead of showing stale directions
import QtQuick
import Sdv 1.0

Item {
    id: tile
    required property QtObject theme
    required property QtObject cockpit
    width: 440; height: 260

    readonly property int st: cockpit.iviState

    IviSurface {
        anchors.fill: parent
        link: tile.cockpit
        visible: tile.st === CockpitLink.Healthy
    }

    // ---- cluster-native fallback ------------------------------------------------
    Rectangle {
        anchors.fill: parent
        visible: tile.st === CockpitLink.Degraded
        radius: 22
        color: tile.theme.panel
        border.color: tile.theme.caution; border.width: 1.5

        Row {
            anchors { left: parent.left; top: parent.top; margins: 26 }
            spacing: 24
            Text {
                text: ["↑", "↰", "↱", "↻", "⚑"][Math.max(0, Math.min(4, tile.cockpit.maneuver))]
                color: tile.theme.text
                font.pixelSize: 96; font.family: tile.theme.font
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                Text {
                    text: tile.cockpit.distance >= 1000 ? (tile.cockpit.distance / 1000).toFixed(1) + " km"
                                                       : Math.round(tile.cockpit.distance / 10) * 10 + " m"
                    color: tile.theme.text; font.pixelSize: 44; font.family: tile.theme.font
                }
                Text { text: tile.cockpit.street; color: tile.theme.textDim; font.pixelSize: 20; font.family: tile.theme.font }
            }
        }
        Text {
            anchors { left: parent.left; bottom: parent.bottom; margins: 22 }
            text: "Infotainment display degraded · cluster fallback"
            color: tile.theme.caution; font.pixelSize: 15; font.family: tile.theme.font
        }
    }

    // ---- unavailable ------------------------------------------------------------
    Rectangle {
        anchors.fill: parent
        visible: tile.st === CockpitLink.Unavailable || tile.st === CockpitLink.Starting
        radius: 22
        color: "transparent"
        border.color: tile.theme.track; border.width: 1.5
        Column {
            anchors.centerIn: parent
            spacing: 8
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: tile.st === CockpitLink.Starting ? "Navigation starting…" : "Navigation unavailable"
                color: tile.theme.textDim; font.pixelSize: 24; font.family: tile.theme.font
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Infotainment partition: " + (tile.cockpit.iviPartitionState || "—")
                      + (tile.cockpit.iviRestarts > 0 ? "  ·  restarts " + tile.cockpit.iviRestarts : "")
                color: tile.theme.textDim; opacity: 0.8; font.pixelSize: 16; font.family: tile.theme.font
            }
        }
    }
}
