// Telltale row. Colours follow ISO 2575 conventions: green = active function,
// blue = high beam, amber = caution, red = warning.
import QtQuick
import Sdv 1.0

Row {
    id: row
    required property QtObject theme
    required property QtObject vehicle
    spacing: 14

    component Lamp: Rectangle {
        id: lamp
        property string glyph
        property bool on: false
        property color lit: row.theme.ok
        width: Math.max(54, t.implicitWidth + 22); height: 34; radius: 8
        color: on ? Qt.rgba(lit.r, lit.g, lit.b, 0.16) : "transparent"
        border.color: on ? lit : row.theme.track
        border.width: 1.5
        opacity: on ? 1 : 0.35
        Text {
            id: t
            anchors.centerIn: parent
            text: lamp.glyph
            color: lamp.on ? lamp.lit : row.theme.textDim
            font.pixelSize: 16; font.bold: true; font.family: row.theme.font
        }
    }

    readonly property int tt: vehicle.telltales
    function has(bit) { return (tt & bit) !== 0 }

    Lamp { glyph: "◀"; on: row.has(VehicleState.LeftIndicator); lit: row.theme.ok; width: 54 }
    Lamp { glyph: "READY"; on: row.has(VehicleState.Ready); lit: row.theme.ok }
    Lamp { glyph: "LOW"; on: row.has(VehicleState.LowBeam); lit: row.theme.ok }
    Lamp { glyph: "HIGH"; on: row.has(VehicleState.HighBeam); lit: row.theme.info }
    Lamp { glyph: "(P)"; on: row.has(VehicleState.ParkingBrake); lit: row.theme.warn }
    Lamp { glyph: "BELT"; on: row.has(VehicleState.SeatBelt); lit: row.theme.warn }
    Lamp { glyph: "TPMS"; on: row.has(VehicleState.TyrePressure); lit: row.theme.caution }
    Lamp { glyph: "BATT"; on: row.has(VehicleState.BatteryLow); lit: row.theme.caution }
    Lamp { glyph: "▶"; on: row.has(VehicleState.RightIndicator); lit: row.theme.ok; width: 54 }
}
