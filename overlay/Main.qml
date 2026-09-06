import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 560
    height: 112
    x: Screen.virtualX + Math.round((Screen.width - width) / 2)
    y: Screen.virtualY + Screen.height - height - 72
    visible: overlayModel.panelVisible
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus

    Rectangle {
        anchors.fill: parent
        radius: 24
        color: "#ed17191f"
        border.width: 1
        border.color: overlayModel.error ? "#ff6b6b" : "#424650"

        Row {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            spacing: 18

            Item {
                width: 28
                height: parent.height

                Rectangle {
                    id: pulse
                    anchors.centerIn: parent
                    width: 15
                    height: 15
                    radius: width / 2
                    color: overlayModel.error ? "#ff6b6b"
                         : overlayModel.recording ? "#ff4d67" : "#64d98b"
                    opacity: 0.95

                    SequentialAnimation on scale {
                        running: overlayModel.recording
                        loops: Animation.Infinite
                        NumberAnimation { to: 1.34; duration: 520; easing.type: Easing.OutCubic }
                        NumberAnimation { to: 1.0; duration: 520; easing.type: Easing.InCubic }
                    }
                }
            }

            Column {
                width: parent.width - 28 - meter.width - parent.spacing * 2
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Text {
                    width: parent.width
                    text: overlayModel.status
                    color: overlayModel.error ? "#ff8585" : "#aeb4bf"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: overlayModel.text.length > 0
                          ? overlayModel.text
                          : (overlayModel.recording ? "请开始说话" : "")
                    color: "#f4f5f7"
                    font.pixelSize: 20
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }

            Row {
                id: meter
                width: 64
                height: parent.height
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5

                Repeater {
                    model: 7
                    Rectangle {
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: 5
                        radius: 3
                        color: overlayModel.error ? "#ff6b6b" : "#7b8cff"
                        height: {
                            const gain = Math.min(1, Math.sqrt(overlayModel.level * 8))
                            const shape = 0.42 + 0.58 * Math.sin((index + 1) * Math.PI / 8)
                            return 8 + 50 * gain * shape
                        }
                        Behavior on height { NumberAnimation { duration: 70 } }
                    }
                }
            }
        }
    }
}
