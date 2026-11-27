import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Controls.Material 2.2

ToolTip {
    id: control

    padding: 10
    horizontalPadding: 12
    Material.theme: Material.Dark
    Material.foreground: "#f0f6fc"

    contentItem: Text {
        text: control.text
        font: control.font
        color: "#f0f6fc"
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        radius: 8
        color: (typeof window !== "undefined" && window.theme && window.theme.isDark)
               ? "#2d333b"
               : "#24292f"
        border.width: 1
        border.color: (typeof window !== "undefined" && window.theme && window.theme.isDark)
                      ? "#444c56"
                      : "#343b46"
    }
}
