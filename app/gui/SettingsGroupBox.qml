import QtQuick 2.9
import QtQuick.Controls 2.5

GroupBox {
    id: control
    padding: 14
    font.pointSize: 12

    label: Label {
        x: control.leftPadding
        width: control.availableWidth
        text: control.title
        font.pointSize: control.font.pointSize
        font.weight: Font.DemiBold
        color: (typeof window !== "undefined" && window.theme) ? window.theme.accent : "#0969da"
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        y: control.topPadding - control.bottomPadding
        width: parent.width
        height: parent.height - control.topPadding + control.bottomPadding
        radius: 10
        color: (typeof window !== "undefined" && window.theme) ? window.theme.bgSurface : "#ffffff"
        border.color: (typeof window !== "undefined" && window.theme) ? window.theme.border : "#d0d7de"
        border.width: 1
    }
}
