import QtQuick 2.0
import QtQuick.Controls 2.5
import QtQuick.Controls.Material 2.2

Dialog {
    modal: true
    anchors.centerIn: Overlay.overlay
    Material.background: (typeof window !== "undefined" && window.theme) ? window.theme.bgSurface : "#ffffff"

    onClosed: {
        // We must force focus back to the last item. If we don't,
        // gamepad and keyboard navigation will break after a
        // dialog appears.
        stackView.forceActiveFocus()
    }
}
