// New in v1.1.0: live consumption + eco score, computed from SOME/IP power/speed.
import QtQuick
import QtQuick.Shapes

Item {
    id: eff
    property QtObject theme
    property QtObject vehicle
    width: 520; height: 230

    readonly property real kwh100: vehicle ? vehicle.avgConsumption : 0
    // 15 kWh/100 km or better = 100, 30 or worse = 0
    readonly property int ecoScore: Math.round(Math.max(0, Math.min(100, (30 - kwh100) / 15 * 100)))

    Row {
        anchors.centerIn: parent
        spacing: 50

        Column {
            spacing: 4
            Text { text: "AVG CONSUMPTION"; color: eff.theme.textDim; font.pixelSize: 15; font.letterSpacing: 3; font.family: eff.theme.font }
            Text { text: eff.kwh100 > 0 ? eff.kwh100.toFixed(1) : "—"; color: eff.theme.text; font.pixelSize: 56; font.weight: Font.Light; font.family: eff.theme.font }
            Text { text: "kWh / 100 km"; color: eff.theme.textDim; font.pixelSize: 16; font.family: eff.theme.font }
            Item { width: 1; height: 8 }
            Text { text: "TRIP  " + eff.vehicle.trip.toFixed(1) + " km"; color: eff.theme.text; font.pixelSize: 20; font.family: eff.theme.font }
        }

        Item {
            width: 170; height: 170
            Rectangle {
                anchors.fill: parent; radius: width / 2
                color: "transparent"; border.width: 10; border.color: eff.theme.track
            }
            Shape {
                id: ring
                anchors.fill: parent
                layer.enabled: true; layer.samples: 4
                property real shown: eff.ecoScore
                Behavior on shown { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }
                ShapePath {
                    strokeWidth: 10; fillColor: "transparent"; capStyle: ShapePath.RoundCap
                    strokeColor: eff.ecoScore > 66 ? eff.theme.ok : eff.ecoScore > 33 ? eff.theme.caution : eff.theme.warn
                    PathAngleArc {
                        centerX: ring.width / 2; centerY: ring.height / 2
                        radiusX: ring.width / 2 - 5; radiusY: ring.height / 2 - 5
                        startAngle: -90; sweepAngle: 360 * ring.shown / 100
                    }
                }
            }
            Column {
                anchors.centerIn: parent
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: eff.ecoScore; color: eff.theme.text
                       font.pixelSize: 54; font.family: eff.theme.font }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "ECO SCORE"; color: eff.theme.textDim
                       font.pixelSize: 13; font.letterSpacing: 2; font.family: eff.theme.font }
            }
        }
    }
}
