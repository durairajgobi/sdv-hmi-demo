// v1.2.0 — DELIBERATELY BROKEN release used to demonstrate automatic rollback.
// It depends on a shell API (Sdv.Navigation 2.0) that the vehicle's installed
// native shell does not provide, so the QML engine fails to load the bundle.
// The shell reports the failure over SOME/IP and ota-agent rolls back to the
// previous slot without the cluster ever going blank.
import QtQuick
import Sdv.Navigation 2.0

QtObject {
    readonly property string name: "Horizon Nav"
    readonly property string font: "DejaVu Sans"
    readonly property bool showEfficiency: true
    readonly property string speedLabel: "NAV"
    readonly property color background: "#04090a"
    readonly property color backgroundTop: "#08201d"
    readonly property color panel: "#0d1f1d"
    readonly property color dialTop: "#0b2522"
    readonly property color dialBottom: "#050d0c"
    readonly property color track: "#1b3330"
    readonly property color accent: "#9b7bff"
    readonly property color text: "#ecfbf6"
    readonly property color textDim: "#7fa39a"
    readonly property color ok: "#2ee6a8"
    readonly property color regen: "#7cf2ff"
    readonly property color info: "#4c8dff"
    readonly property color caution: "#ffb020"
    readonly property color warn: "#ff5a5f"
}
