// Circular arc gauge drawn with Qt Quick Shapes (GPU path rendering).
import QtQuick
import QtQuick.Shapes

Item {
    id: g
    required property QtObject theme
    property real value: 0
    property real minValue: 0
    property real maxValue: 240
    property real zeroValue: 0          // arc grows from here (supports regen < 0)
    property real majorStep: 20
    property string unit: "km/h"
    property string label: ""
    property int decimals: 0
    property color arcColor: theme.accent
    property color negativeColor: theme.regen

    readonly property real startAngle: 135
    readonly property real sweep: 270
    readonly property real radius: Math.min(width, height) / 2 - 18
    function angleOf(v) {
        const f = (Math.max(minValue, Math.min(maxValue, v)) - minValue) / (maxValue - minValue)
        return startAngle + sweep * f
    }

    Behavior on value { NumberAnimation { duration: 80 } }

    // soft glow disc
    Rectangle {
        anchors.centerIn: parent
        width: g.radius * 2 - 40; height: width; radius: width / 2
        gradient: Gradient {
            GradientStop { position: 0; color: g.theme.dialTop }
            GradientStop { position: 1; color: g.theme.dialBottom }
        }
    }

    Shape {
        anchors.fill: parent
        layer.enabled: true
        layer.samples: 4    // MSAA for smooth arcs (ignored by the software backend)
        // track
        ShapePath {
            strokeColor: g.theme.track; strokeWidth: 12; fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: g.width / 2; centerY: g.height / 2
                radiusX: g.radius; radiusY: g.radius
                startAngle: g.startAngle; sweepAngle: g.sweep
            }
        }
        // value
        ShapePath {
            strokeColor: g.value < g.zeroValue ? g.negativeColor : g.arcColor
            strokeWidth: 12; fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: g.width / 2; centerY: g.height / 2
                radiusX: g.radius; radiusY: g.radius
                startAngle: g.angleOf(g.zeroValue)
                sweepAngle: g.angleOf(g.value) - g.angleOf(g.zeroValue)
            }
        }
    }

    // ticks + labels
    Repeater {
        model: Math.round((g.maxValue - g.minValue) / g.majorStep) + 1
        Item {
            required property int index
            readonly property real v: g.minValue + index * g.majorStep
            readonly property real a: g.angleOf(v) * Math.PI / 180
            x: g.width / 2 + Math.cos(a) * (g.radius - 34) - width / 2
            y: g.height / 2 + Math.sin(a) * (g.radius - 34) - height / 2
            width: 44; height: 24
            Text {
                anchors.centerIn: parent
                text: Math.abs(parent.v) < 1e-6 ? "0" : parent.v
                color: parent.v <= g.value && parent.v >= g.zeroValue ? g.theme.text : g.theme.textDim
                font.pixelSize: 17; font.family: g.theme.font
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: -4
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: g.value.toFixed(g.decimals)
            color: g.theme.text
            font.pixelSize: g.radius * 0.52; font.family: g.theme.font; font.weight: Font.Light
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: g.unit
            color: g.theme.textDim
            font.pixelSize: 22; font.family: g.theme.font; font.letterSpacing: 2
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            topPadding: 14
            text: g.label
            color: g.theme.accent
            font.pixelSize: 16; font.family: g.theme.font; font.letterSpacing: 4
        }
    }
}
