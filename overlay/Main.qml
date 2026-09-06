import QtQuick
import QtQuick.Window

Window {
    id: root

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    // Every metric below is a multiple of the system font's line height, so the
    // overlay follows the user's font size and display scaling instead of a
    // fixed pixel grid.
    FontMetrics {
        id: baseFont
    }

    readonly property real unit: Math.max(14, baseFont.height)

    // Desktop-specific look lives in one token table rather than in a ternary on
    // every property: Plasma gets Breeze's tight, squared, high-contrast frame,
    // GNOME gets Adwaita's rounder and airier one.
    readonly property var styleTokens: ({
        "kde": {
            "cornerScale": 0.20,
            "chipCornerScale": 0.12,
            "dotCornerScale": 0.20,
            "barCornerScale": 0.06,
            "paddingScale": 0.85,
            "columnSpacingScale": 0.28,
            "rowSpacingScale": 0.70,
            "widthUnits": 27.0,
            "borderAlpha": 0.95,
            "accentBar": true,
            "panelAlpha": 0.97,
            "statusWeight": Font.DemiBold,
            "textWeight": Font.Medium,
            "meterUnits": 2.7,
            "meterBars": 7
        },
        "gnome": {
            "cornerScale": 0.95,
            "chipCornerScale": 0.55,
            "dotCornerScale": 0.50,
            "barCornerScale": 0.50,
            "paddingScale": 1.20,
            "columnSpacingScale": 0.42,
            "rowSpacingScale": 0.85,
            "widthUnits": 26.0,
            "borderAlpha": 0.45,
            "accentBar": false,
            "panelAlpha": 0.99,
            "statusWeight": Font.Bold,
            "textWeight": Font.Normal,
            "meterUnits": 3.0,
            "meterBars": 7
        },
        "generic": {
            "cornerScale": 0.55,
            "chipCornerScale": 0.30,
            "dotCornerScale": 0.35,
            "paddingScale": 1.0,
            "barCornerScale": 0.25,
            "columnSpacingScale": 0.35,
            "rowSpacingScale": 0.75,
            "widthUnits": 26.0,
            "borderAlpha": 0.7,
            "accentBar": false,
            "panelAlpha": 0.98,
            "statusWeight": Font.DemiBold,
            "textWeight": Font.Medium,
            "meterUnits": 2.8,
            "meterBars": 7
        }
    })

    readonly property var style: styleTokens[overlayModel.desktopStyle]
        !== undefined ? styleTokens[overlayModel.desktopStyle]
                      : styleTokens["generic"]

    // Qt reports the portal's org.freedesktop.appearance color-scheme on both
    // Plasma and GNOME and re-emits it on change, so the overlay follows a live
    // light/dark switch. The palette lightness is only a fallback for platforms
    // that report Unknown.
    readonly property int colorScheme: Application.styleHints.colorScheme
    readonly property bool darkMode: colorScheme === Qt.ColorScheme.Dark
        || (colorScheme === Qt.ColorScheme.Unknown
            && systemPalette.window.hslLightness < 0.5)

    readonly property color panelColor: Qt.rgba(
        systemPalette.window.r, systemPalette.window.g, systemPalette.window.b,
        style.panelAlpha)
    readonly property color borderColor: Qt.rgba(
        darkMode ? systemPalette.light.r : systemPalette.mid.r,
        darkMode ? systemPalette.light.g : systemPalette.mid.g,
        darkMode ? systemPalette.light.b : systemPalette.mid.b,
        style.borderAlpha)
    readonly property color primaryText: systemPalette.windowText
    readonly property color secondaryText: Qt.rgba(
        systemPalette.windowText.r, systemPalette.windowText.g,
        systemPalette.windowText.b, darkMode ? 0.72 : 0.66)
    readonly property color accentColor: systemPalette.highlight
    readonly property color dangerColor: darkMode ? "#ff6b7a" : "#c9344d"
    readonly property color successColor: darkMode ? "#63d894" : "#168553"
    readonly property color signalColor: overlayModel.error ? dangerColor : accentColor

    readonly property real padding: Math.round(unit * style.paddingScale)
    readonly property real corner: Math.round(unit * style.cornerScale)

    width: Math.round(Math.min(Screen.width * 0.6, unit * style.widthUnits))
    height: Math.round(content.implicitHeight + padding * 2)
    x: Screen.virtualX + Math.round((Screen.width - width) / 2)
    y: Screen.virtualY + Screen.height - height - Math.round(unit * 3)
    visible: overlayModel.panelVisible
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus

    Rectangle {
        id: panel
        anchors.fill: parent
        radius: root.corner
        color: root.panelColor
        border.width: 1
        border.color: overlayModel.error ? root.dangerColor : root.borderColor
        clip: true

        Rectangle {
            visible: root.style.accentBar
            width: Math.max(2, Math.round(root.unit * 0.16))
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: panel.border.width
            color: root.signalColor
        }

        Row {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: root.padding
            anchors.rightMargin: root.padding
            spacing: Math.round(root.unit * root.style.rowSpacingScale)

            Item {
                id: indicator
                width: Math.round(root.unit * 1.6)
                height: width
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    id: pulse
                    anchors.centerIn: parent
                    width: Math.round(root.unit * 0.75)
                    height: width
                    radius: width * root.style.dotCornerScale
                    color: overlayModel.recording || overlayModel.error
                        ? root.dangerColor : root.successColor

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width * 1.7
                        height: width
                        radius: width * root.style.dotCornerScale
                        color: "transparent"
                        border.width: 1
                        border.color: pulse.color
                        opacity: overlayModel.recording ? 0.42 : 0.0

                        SequentialAnimation on scale {
                            running: overlayModel.recording
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 1.28
                                duration: 520
                                easing.type: Easing.OutCubic
                            }
                            NumberAnimation {
                                to: 1.0
                                duration: 520
                                easing.type: Easing.InCubic
                            }
                        }
                    }
                }
            }

            Column {
                id: labels
                width: parent.width - indicator.width - meter.width
                       - parent.spacing * 2
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(root.unit * root.style.columnSpacingScale)

                Row {
                    width: parent.width
                    spacing: Math.round(root.unit * 0.4)

                    Text {
                        id: statusLabel
                        anchors.verticalCenter: parent.verticalCenter
                        text: overlayModel.status
                        color: overlayModel.error ? root.dangerColor
                                                  : root.secondaryText
                        font.pixelSize: Math.round(root.unit * 0.72)
                        font.weight: root.style.statusWeight
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: overlayModel.source.length > 0
                        width: Math.min(sourceLabel.implicitWidth + root.unit * 0.8,
                                        parent.width - statusLabel.width
                                        - parent.spacing)
                        height: Math.round(root.unit * 1.15)
                        radius: height * root.style.chipCornerScale
                        color: Qt.rgba(root.accentColor.r, root.accentColor.g,
                                       root.accentColor.b,
                                       root.darkMode ? 0.22 : 0.13)

                        Text {
                            id: sourceLabel
                            anchors.fill: parent
                            anchors.leftMargin: Math.round(root.unit * 0.4)
                            anchors.rightMargin: Math.round(root.unit * 0.4)
                            text: overlayModel.source
                            color: root.secondaryText
                            font.pixelSize: Math.round(root.unit * 0.62)
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: overlayModel.text
                    visible: overlayModel.text.length > 0
                    color: root.primaryText
                    font.pixelSize: Math.round(root.unit * 1.05)
                    font.weight: root.style.textWeight
                    elide: Text.ElideRight
                }
            }

            Row {
                id: meter
                width: Math.round(root.unit * root.style.meterUnits)
                height: Math.round(root.unit * 2.6)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(root.unit * 0.22)
                layoutDirection: Qt.RightToLeft

                Repeater {
                    model: root.style.meterBars

                    Rectangle {
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(2, Math.round(root.unit * 0.22))
                        radius: width * root.style.barCornerScale
                        color: root.signalColor
                        opacity: 0.92
                        height: {
                            const gain = Math.min(
                                1, Math.sqrt(overlayModel.level * 8));
                            const shape = 0.42 + 0.58 * Math.sin(
                                (index + 1) * Math.PI / (root.style.meterBars + 1));
                            return Math.round(root.unit * 0.4
                                + meter.height * 0.8 * gain * shape);
                        }
                        Behavior on height {
                            NumberAnimation { duration: 70 }
                        }
                    }
                }
            }
        }
    }
}
