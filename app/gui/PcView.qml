import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: 310; cellHeight: 330;
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        return model
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        id: pcCardDelegate
        width: 300; height: 320;
        grid: pcGrid
        padding: 12

        property alias pcContextMenu : pcContextMenuLoader.item

        background: Rectangle {
            anchors.fill: parent
            anchors.margins: 8
            radius: 12
            color: pcCardDelegate.hovered || pcCardDelegate.highlighted
                   ? window.theme.bgHover
                   : window.theme.bgSurface
            border.width: pcCardDelegate.highlighted ? 2 : 1
            border.color: pcCardDelegate.highlighted ? window.theme.accent : window.theme.border
        }

        Image {
            id: pcIcon
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 28
            source: window.tintedUrl(model.online ? window.theme.textPrimary : window.theme.textSecondary,
                                     "res/desktop_windows-48px.svg")
            sourceSize {
                width: 164
                height: 164
            }
        }

        Image {
            // TODO: Tooltip
            id: stateIcon
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: !model.online ? -14 : -12
            visible: !model.statusUnknown && (!model.online || !model.paired)
            source: window.tintedUrl(!model.online ? window.theme.warningText : window.theme.accent,
                                     !model.online ? "res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "res/baseline-lock-24px.svg")
            sourceSize {
                width: !model.online ? 60 : 56
                height: !model.online ? 60 : 56
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: -12
            width: 64
            height: 64
            visible: model.statusUnknown
            running: visible
        }

        Label {
            id: pcNameText
            text: model.name

            anchors.top: pcIcon.bottom
            anchors.topMargin: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.right: parent.right
            anchors.rightMargin: 18
            color: model.online ? window.theme.textPrimary : window.theme.textSecondary
            font.pointSize: 20
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }

        // Display a tooltip with the full name if it's truncated
        ToolTip.text: model.name
        ToolTip.delay: 1000
        ToolTip.timeout: 5000
        ToolTip.visible: (hovered || highlighted) && pcNameText.truncated

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
                NavigableMenuItem {
                    visible: model.paired
                    text: model.fileClipboardApproved ? qsTr("Revoke File Clipboard") : qsTr("Approve File Clipboard")
                    onTriggered: {
                        var fingerprint = computerModel.fileClipboardFingerprint(index)
                        if (model.fileClipboardApproved) {
                            computerModel.setFileClipboardApproval(index, fingerprint, false)
                        } else {
                            fileClipboardApprovalDialog.pcIndex = index
                            fileClipboardApprovalDialog.pcName = model.name
                            fileClipboardApprovalDialog.fingerprint = fingerprint
                            fileClipboardApprovalDialog.open()
                        }
                    }
                }
                NavigableMenuItem {
                    visible: model.paired
                    text: qsTr("PC Settings...")
                    onTriggered: {
                        pcSettingsDialog.openForComputer(index, model.name)
                    }
                }
            }
        }

        ToolButton {
            id: pcGearButton
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 16
            z: 2
            width: 32
            height: 32
            padding: 5
            icon.source: "qrc:/res/settings.svg"
            icon.color: pcGearButton.hovered ? window.theme.accent : window.theme.textSecondary
            icon.width: 18
            icon.height: 18
            focusPolicy: Qt.NoFocus
            background: Rectangle {
                radius: 16
                color: pcGearButton.hovered ? window.theme.bgSelected : "transparent"
            }

            onClicked: {
                if (!pcContextMenu) {
                    return
                }
                if (pcContextMenu.popup) {
                    pcContextMenu.popup(pcGearButton, 0, pcGearButton.height)
                }
                else {
                    pcContextMenu.open()
                }
            }

            ToolTip.delay: 600
            ToolTip.timeout: 3000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("%1 options").arg(model.name)
        }

        onClicked: {
            if (model.online) {
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.paired) {
                    // go to game view
                    var component = Qt.createComponent("AppView.qml")
                    var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name})
                    stackView.push(appView)
                }
                else {
                    var pin = computerModel.generatePinString()

                    // Kick off pairing in the background
                    computerModel.pairComputer(index, pin)

                    // Display the pairing dialog
                    pairDialog.pin = pin
                    pairDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    NavigableMessageDialog {
        id: pairDialog
        closePolicy: Popup.CloseOnEscape

        // don't allow edits to the rest of the window while open
        property string pin : "0000"
        text:qsTr("Please enter %1 on your host PC. This dialog will close when pairing is completed.").arg(pin)+"\n\n"+
             qsTr("If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.")
        standardButtons: Dialog.Cancel
        onRejected: {
            // FIXME: We should interrupt pairing here
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: fileClipboardApprovalDialog
        property int pcIndex: -1
        property string pcName: ""
        property string fingerprint: ""
        text: qsTr("Allow file Copy/Paste with '%1'?").arg(pcName) + "\n\n" +
              qsTr("While streaming with clipboard and file transfer enabled, copying files exchanges metadata first and transfers bytes on demand at Paste. Paste chooses the final folder. This approval applies only to its current pairing certificate. Folders and Cut are unsupported.")
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: computerModel.setFileClipboardApproval(pcIndex, fingerprint, true)
    }

    function resolveSelectedAdapterId(adapters, selected) {
        var id = selected || ""
        for (var i = 0; i < adapters.length; i++) {
            if (adapters[i].id === id) {
                return id
            }
        }
        return ""
    }

    NavigableDialog {
        id: pcSettingsDialog
        property int pcIndex: -1
        property string pcName: ""
        property string pcFingerprint: ""
        property bool hostOnline: true
        property bool capabilitiesKnown: true
        property bool supportsAdaptiveRefinement: false
        property bool adaptiveRefinementEnabled: false
        property bool supportsHostEncoder: true
        property int hostEncoderConfig: StreamingPreferences.HEC_AUTO
        property bool hasMultipleAdapters: false
        property bool hasHostCaptureAdapter: false
        property var adapters: []
        property string selectedAdapterId: ""
        property string selectedCaptureAdapterId: ""
        property int matchHostResolutionOverride: -1
        property int revertHostResolutionOverride: -1
        property int muteHostSpeakersOverride: -1
        property int gameOptimizationsOverride: -1
        property int quitAppAfterOverride: -1
        property int openGeneration: 0

        readonly property bool effectiveMatchHostRes: matchHostResolutionOverride < 0
                                                      ? StreamingPreferences.matchHostResolution
                                                      : (matchHostResolutionOverride > 0)

        readonly property bool adaptiveRefinementWillActivate: adaptiveRefinementEnabled &&
                                                               SystemProperties.isDarwin &&
                                                               (!capabilitiesKnown || supportsAdaptiveRefinement) &&
                                                               !StreamingPreferences.enableHdr &&
                                                               !(SystemProperties.isDarwin &&
                                                                 StreamingPreferences.rendererSelection !== StreamingPreferences.RS_AUTO &&
                                                                 StreamingPreferences.rendererSelection !== StreamingPreferences.RS_METAL)

        width: Overlay.overlay ? Math.min(560, Math.max(320, Overlay.overlay.width - 40)) : 560
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            SdlGamepadKeyNavigation.setUiNavMode(true)
            adaptiveRefinementCheck.forceActiveFocus()
        }

        onClosed: {
            SdlGamepadKeyNavigation.setUiNavMode(false)
            adaptiveInfoHoverTimer.stop()
            adaptiveInfoButton.pinned = false
            adaptiveInfoToolTip.close()
            stackView.forceActiveFocus()
        }

        function openForComputer(index, name) {
            var cfg = computerModel.getPcSettings(index)
            pcIndex = index
            pcName = name
            pcFingerprint = cfg.fingerprint || ""
            hostOnline = !!cfg.hostOnline
            capabilitiesKnown = cfg.capabilitiesKnown === undefined ? true : !!cfg.capabilitiesKnown
            supportsAdaptiveRefinement = !!cfg.supportsAdaptiveTileRefinement
            adaptiveRefinementEnabled = !!cfg.adaptiveTileRefinement
            adaptiveRefinementCheck.checked = adaptiveRefinementEnabled
            supportsHostEncoder = cfg.supportsHostEncoder === undefined ? true : !!cfg.supportsHostEncoder
            hostEncoderConfig = cfg.hostEncoderConfig !== undefined ? cfg.hostEncoderConfig : StreamingPreferences.HEC_AUTO
            hasMultipleAdapters = !!cfg.hasMultipleAdapters
            hasHostCaptureAdapter = !!cfg.hasHostCaptureAdapter

            var nextAdapters = cfg.adapters || []
            selectedAdapterId = resolveSelectedAdapterId(nextAdapters, cfg.selectedAdapterId)
            selectedCaptureAdapterId = resolveSelectedAdapterId(nextAdapters, cfg.selectedCaptureAdapterId)
            adapters = nextAdapters

            matchHostResolutionOverride = cfg.matchHostResolutionOverride !== undefined ? cfg.matchHostResolutionOverride : -1
            revertHostResolutionOverride = cfg.revertHostResolutionOverride !== undefined ? cfg.revertHostResolutionOverride : -1
            muteHostSpeakersOverride = cfg.muteHostSpeakersOverride !== undefined ? cfg.muteHostSpeakersOverride : -1
            gameOptimizationsOverride = cfg.gameOptimizationsOverride !== undefined ? cfg.gameOptimizationsOverride : -1
            quitAppAfterOverride = cfg.quitAppAfterOverride !== undefined ? cfg.quitAppAfterOverride : -1

            pcSettingsFlickable.contentY = 0
            openGeneration += 1
            syncHostEncoderIndex()
            syncAdapterCombos()
            open()
        }

        function syncHostEncoderIndex() {
            for (var i = 0; i < pcHostEncoderListModel.count; i++) {
                if (hostEncoderConfig === pcHostEncoderListModel.get(i).val) {
                    pcHostEncoderComboBox.currentIndex = i
                    pcHostEncoderComboBox.recalculateWidth()
                    return
                }
            }
            pcHostEncoderComboBox.currentIndex = 0
            pcHostEncoderComboBox.recalculateWidth()
        }

        function syncAdapterCombos() {
            pcAdapterListModel.clear()
            pcAdapterListModel.append({ "text": qsTr("Automatic (Host default)"), "adapterId": "" })
            var encIdx = 0
            for (var i = 0; i < adapters.length; i++) {
                var a = adapters[i]
                var label = a.isDefault ? qsTr("%1 (Host Default)").arg(a.name) : a.name
                pcAdapterListModel.append({ "text": label, "adapterId": a.id })
                if (a.id === selectedAdapterId) {
                    encIdx = i + 1
                }
            }
            pcAdapterComboBox.currentIndex = encIdx
            pcAdapterComboBox.recalculateWidth()

            pcCaptureAdapterListModel.clear()
            pcCaptureAdapterListModel.append({ "text": qsTr("Automatic (Host current X11)"), "adapterId": "" })
            var capIdx = 0
            for (var j = 0; j < adapters.length; j++) {
                var ca = adapters[j]
                var capLabel = ca.isCaptureDefault ? qsTr("%1 (Active X11)").arg(ca.name) : ca.name
                pcCaptureAdapterListModel.append({ "text": capLabel, "adapterId": ca.id })
                if (ca.id === selectedCaptureAdapterId) {
                    capIdx = j + 1
                }
            }
            pcCaptureAdapterComboBox.currentIndex = capIdx
            pcCaptureAdapterComboBox.recalculateWidth()
        }

        function overrideGlobalDefault(key) {
            switch (key) {
            case "matchHostResolution": return StreamingPreferences.matchHostResolution
            case "revertHostResolution": return StreamingPreferences.revertHostResolution
            case "muteHostSpeakers": return !StreamingPreferences.playAudioOnHost
            case "gameOptimizations": return StreamingPreferences.gameOptimizations
            case "quitAppAfter": return StreamingPreferences.quitAppAfter
            default: return false
            }
        }

        function overrideValue(key) {
            switch (key) {
            case "matchHostResolution": return matchHostResolutionOverride
            case "revertHostResolution": return revertHostResolutionOverride
            case "muteHostSpeakers": return muteHostSpeakersOverride
            case "gameOptimizations": return gameOptimizationsOverride
            case "quitAppAfter": return quitAppAfterOverride
            default: return -1
            }
        }

        function setOverrideValue(key, state) {
            switch (key) {
            case "matchHostResolution":
                matchHostResolutionOverride = state
                break
            case "revertHostResolution":
                revertHostResolutionOverride = state
                break
            case "muteHostSpeakers":
                muteHostSpeakersOverride = state
                break
            case "gameOptimizations":
                gameOptimizationsOverride = state
                break
            case "quitAppAfter":
                quitAppAfterOverride = state
                break
            }
        }

        function overrideRowEnabled(key) {
            if (key === "revertHostResolution") {
                return effectiveMatchHostRes
            }
            return true
        }

        function overrideToolTip(key) {
            switch (key) {
            case "matchHostResolution":
                return qsTr("Switches the host desktop resolution and refresh rate to match your Moonlight stream resolution on launch (when supported by Sunshine).")
            case "revertHostResolution":
                return qsTr("Restores the host desktop to its original resolution when the stream disconnects. Leaving this off keeps the stream resolution on the host.")
            case "muteHostSpeakers":
                return qsTr("You must restart any game currently in progress for this setting to take effect.")
            case "gameOptimizations":
                return qsTr("Lets the host apply optimized game or session display settings for streaming.")
            case "quitAppAfter":
                return qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
            default:
                return ""
            }
        }

        onAccepted: {
            if (pcIndex >= 0 && pcFingerprint.length > 0) {
                var payload = {
                    "adaptiveTileRefinement": adaptiveRefinementEnabled,
                    "hostEncoderConfig": hostEncoderConfig,
                    "matchHostResolutionOverride": matchHostResolutionOverride,
                    "revertHostResolutionOverride": revertHostResolutionOverride,
                    "muteHostSpeakersOverride": muteHostSpeakersOverride,
                    "gameOptimizationsOverride": gameOptimizationsOverride,
                    "quitAppAfterOverride": quitAppAfterOverride
                }
                if (hasMultipleAdapters) {
                    payload["selectedAdapterId"] = selectedAdapterId
                    if (hasHostCaptureAdapter) {
                        payload["selectedCaptureAdapterId"] = selectedCaptureAdapterId
                    }
                }
                computerModel.setPcSettings(pcIndex, pcFingerprint, payload)
            }
        }

        ColumnLayout {
            width: pcSettingsDialog.availableWidth
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("%1 Settings").arg(pcSettingsDialog.pcName)
                font.bold: true
                font.pointSize: 14
            }

            Flickable {
                id: pcSettingsFlickable
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(pcSettingsColumn.implicitHeight, Overlay.overlay ? Math.max(240, Overlay.overlay.height - 170) : 420)
                contentWidth: width
                contentHeight: pcSettingsColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: pcSettingsFlickable.contentHeight > pcSettingsFlickable.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }

                ColumnLayout {
                    id: pcSettingsColumn
                    width: parent.width - 12
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Video & Host Encoder")
                        font.bold: true
                        color: window.theme.accent
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        CheckBox {
                            id: adaptiveRefinementCheck
                            Layout.fillWidth: true
                            text: qsTr("Adaptive dirty-tile lossless refinement")
                            onToggled: pcSettingsDialog.adaptiveRefinementEnabled = checked
                        }

                        Rectangle {
                            id: adaptiveInfoButton
                            width: 18
                            height: 18
                            radius: 9
                            color: adaptiveInfoHover.containsMouse || pinned ? window.theme.accentHoverBg : "transparent"
                            border.width: 1
                            border.color: adaptiveInfoHover.containsMouse || pinned ? window.theme.accent : window.theme.textSecondary

                            property bool pinned: false

                            Label {
                                anchors.centerIn: parent
                                text: "i"
                                color: adaptiveInfoHover.containsMouse || adaptiveInfoButton.pinned ? window.theme.accent : window.theme.textSecondary
                                font.bold: true
                                font.pointSize: 9
                            }

                            Timer {
                                id: adaptiveInfoHoverTimer
                                interval: 150
                                onTriggered: {
                                    if (adaptiveInfoHover.containsMouse && !adaptiveInfoToolTip.opened) {
                                        adaptiveInfoToolTip.open()
                                    }
                                }
                            }

                            MouseArea {
                                id: adaptiveInfoHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: {
                                    if (!adaptiveInfoButton.pinned) {
                                        adaptiveInfoHoverTimer.restart()
                                    }
                                }
                                onExited: {
                                    adaptiveInfoHoverTimer.stop()
                                    if (!adaptiveInfoButton.pinned) {
                                        adaptiveInfoToolTip.close()
                                    }
                                }
                                onClicked: {
                                    adaptiveInfoHoverTimer.stop()
                                    if (adaptiveInfoButton.pinned) {
                                        adaptiveInfoButton.pinned = false
                                        adaptiveInfoToolTip.close()
                                    } else {
                                        adaptiveInfoButton.pinned = true
                                        adaptiveInfoToolTip.open()
                                    }
                                }
                            }

                            Text {
                                id: adaptiveInfoTextHelper
                                visible: false
                                font: adaptiveInfoToolTip.font
                                text: adaptiveInfoToolTip.text
                            }

                            ToolTip {
                                id: adaptiveInfoToolTip
                                parent: adaptiveInfoButton
                                timeout: -1
                                closePolicy: adaptiveInfoButton.pinned
                                             ? (Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent)
                                             : Popup.NoAutoClose
                                contentWidth: Math.min(adaptiveInfoTextHelper.width, 380)
                                text: qsTr("Streams fast native 4:2:0 video for maximum FPS and low latency in games/video, and sends lossless RGB over paired mTLS ONLY for settled 64x64 text/UI tiles with a GPU zero-ghosting check (|Y_video - Y_ref| <= eps).\n\nTurn ON for IDE, terminal, code editor, or desktop work.\nTurn OFF for pure gaming or full-screen video playback.")
                                onClosed: adaptiveInfoButton.pinned = false
                            }
                        }
                    }

                    Label {
                        readonly property bool badRenderer: SystemProperties.isDarwin &&
                                                            StreamingPreferences.rendererSelection !== StreamingPreferences.RS_AUTO &&
                                                            StreamingPreferences.rendererSelection !== StreamingPreferences.RS_METAL
                        readonly property bool hasConflict: pcSettingsDialog.adaptiveRefinementEnabled && (
                            !SystemProperties.isDarwin ||
                            (pcSettingsDialog.capabilitiesKnown && !pcSettingsDialog.supportsAdaptiveRefinement) ||
                            StreamingPreferences.enableHdr ||
                            badRenderer
                        )

                        Layout.fillWidth: true
                        Layout.leftMargin: adaptiveRefinementCheck.indicator ? (adaptiveRefinementCheck.leftPadding + adaptiveRefinementCheck.indicator.width + adaptiveRefinementCheck.spacing) : 32
                        Layout.topMargin: -8
                        wrapMode: Text.WordWrap
                        font.pointSize: 10
                        color: hasConflict ? window.theme.warningText : (pcSettingsDialog.adaptiveRefinementEnabled && (!pcSettingsDialog.capabilitiesKnown || StreamingPreferences.enableYUV444) ? window.theme.infoText : window.theme.textSecondary)
                        text: {
                            if (!pcSettingsDialog.adaptiveRefinementEnabled) {
                                return qsTr("Off by default for pure 4:2:0 gaming speed. Enable per PC for razor-sharp RGB text tiles over Metal.")
                            }
                            if (!SystemProperties.isDarwin) {
                                return qsTr("Conflict: Adaptive dirty-tile lossless refinement requires the macOS Metal renderer.")
                            }
                            if (!pcSettingsDialog.capabilitiesKnown) {
                                return qsTr("Host is offline; adaptive_tile_refinement support will be verified on connect.")
                            }
                            if (!pcSettingsDialog.supportsAdaptiveRefinement) {
                                return qsTr("Conflict: This host does not currently advertise adaptive_tile_refinement.")
                            }
                            if (StreamingPreferences.enableHdr) {
                                return qsTr("Conflict: HDR is enabled in Global Settings. Turn off HDR to use adaptive dirty-tile refinement (HDR takes precedence).")
                            }
                            if (badRenderer) {
                                return qsTr("Conflict: A non-Metal renderer is selected in Global Settings. Switch Video Renderer to Automatic or Metal to activate adaptive tile refinement.")
                            }
                            if (StreamingPreferences.enableYUV444) {
                                return qsTr("Note: Global YUV 4:4:4 is suppressed for this PC so motion uses fast 4:2:0 while static 64x64 text/UI tiles stay lossless RGB.")
                            }
                            return qsTr("Active: Streams native 4:2:0 with mTLS lossless QOI-RGB24 refinement on settled 64x64 text/UI tiles.")
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        enabled: !pcSettingsDialog.capabilitiesKnown || pcSettingsDialog.supportsHostEncoder

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Host video encoder")
                            wrapMode: Text.Wrap
                        }

                        AutoResizingComboBox {
                            id: pcHostEncoderComboBox
                            maximumWidth: 300
                            textRole: "text"
                            model: ListModel {
                                id: pcHostEncoderListModel
                                ListElement {
                                    text: qsTr("Automatic (Host default)")
                                    val: StreamingPreferences.HEC_AUTO
                                }
                                ListElement {
                                    text: qsTr("Hardware (VAAPI / GPU)")
                                    val: StreamingPreferences.HEC_HARDWARE_VAAPI
                                }
                                ListElement {
                                    text: qsTr("Hardware (Vulkan)")
                                    val: StreamingPreferences.HEC_HARDWARE_VULKAN
                                }
                                ListElement {
                                    text: qsTr("Software (CPU — enables YUV 4:4:4)")
                                    val: StreamingPreferences.HEC_SOFTWARE
                                }
                            }
                            Component.onCompleted: recalculateWidth()
                            onActivated: {
                                pcSettingsDialog.hostEncoderConfig = pcHostEncoderListModel.get(currentIndex).val
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pointSize: 10
                        visible: !pcSettingsDialog.capabilitiesKnown ||
                                 !pcSettingsDialog.supportsHostEncoder ||
                                 (StreamingPreferences.enableYUV444 &&
                                  !pcSettingsDialog.adaptiveRefinementWillActivate &&
                                  (pcSettingsDialog.hostEncoderConfig === StreamingPreferences.HEC_SOFTWARE ||
                                   pcSettingsDialog.hostEncoderConfig === StreamingPreferences.HEC_HARDWARE_VULKAN))
                        color: (!pcSettingsDialog.capabilitiesKnown)
                               ? window.theme.infoText
                               : (!pcSettingsDialog.supportsHostEncoder || pcSettingsDialog.hostEncoderConfig === StreamingPreferences.HEC_HARDWARE_VULKAN
                                  ? window.theme.warningText : window.theme.infoText)
                        text: {
                            if (!pcSettingsDialog.capabilitiesKnown) {
                                return qsTr("Host is offline; encoder capabilities will be verified on connect.")
                            }
                            if (!pcSettingsDialog.supportsHostEncoder) {
                                return qsTr("This host does not advertise per-session host encoder selection.")
                            }
                            if (pcSettingsDialog.hostEncoderConfig === StreamingPreferences.HEC_SOFTWARE) {
                                return qsTr("Host Software (CPU) encoder selected: uses native 4:4:4 directly (H.264 / HEVC).")
                            }
                            if (pcSettingsDialog.hostEncoderConfig === StreamingPreferences.HEC_HARDWARE_VULKAN) {
                                return qsTr("Packed hardware 4:4:4 requires Automatic or VAAPI host encoder; Vulkan host encoder falls back to 4:2:0 on hosts without native 4:4:4.")
                            }
                            return ""
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        visible: pcSettingsDialog.hasMultipleAdapters

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Host Encoder GPU")
                            wrapMode: Text.Wrap
                        }

                        AutoResizingComboBox {
                            id: pcAdapterComboBox
                            maximumWidth: 300
                            textRole: "text"
                            model: ListModel {
                                id: pcAdapterListModel
                            }
                            onActivated: {
                                if (currentIndex >= 0 && currentIndex < pcAdapterListModel.count) {
                                    pcSettingsDialog.selectedAdapterId = pcAdapterListModel.get(currentIndex).adapterId
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        visible: pcSettingsDialog.hasMultipleAdapters && pcSettingsDialog.hasHostCaptureAdapter

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Host X11 / Capture GPU")
                            wrapMode: Text.Wrap

                            MouseArea {
                                id: captureGpuTipHover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                            ToolTip.delay: 800
                            ToolTip.timeout: 7000
                            ToolTip.visible: captureGpuTipHover.containsMouse
                            ToolTip.text: qsTr("Controls KMS capture card and per-session app offload (DRI_PRIME / Vulkan) independently of the video encoder GPU. Shared X11 compositing stays on the host's (Active X11) GPU.")
                        }

                        AutoResizingComboBox {
                            id: pcCaptureAdapterComboBox
                            maximumWidth: 300
                            textRole: "text"
                            model: ListModel {
                                id: pcCaptureAdapterListModel
                            }
                            onActivated: {
                                if (currentIndex >= 0 && currentIndex < pcCaptureAdapterListModel.count) {
                                    pcSettingsDialog.selectedCaptureAdapterId = pcCaptureAdapterListModel.get(currentIndex).adapterId
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: window.theme.border
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Host Behavior (Overrides Global Settings)")
                        font.bold: true
                        color: window.theme.accent
                    }

                    Repeater {
                        model: [
                            { key: "matchHostResolution", label: qsTr("Match host resolution while streaming") },
                            { key: "revertHostResolution", label: qsTr("Revert host resolution when stream ends") },
                            { key: "muteHostSpeakers", label: qsTr("Mute host PC speakers while streaming") },
                            { key: "gameOptimizations", label: qsTr("Optimize game settings for streaming") },
                            { key: "quitAppAfter", label: qsTr("Quit app on host PC after ending stream") }
                        ]

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: modelData.key === "revertHostResolution" ? 16 : 0
                            spacing: 10
                            enabled: pcSettingsDialog.overrideRowEnabled(modelData.key)

                            Label {
                                Layout.fillWidth: true
                                text: modelData.label
                                wrapMode: Text.Wrap

                                MouseArea {
                                    id: behaviorTipHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton
                                }
                                ToolTip.delay: 800
                                ToolTip.timeout: 6000
                                ToolTip.visible: behaviorTipHover.containsMouse
                                ToolTip.text: pcSettingsDialog.overrideToolTip(modelData.key)
                            }

                            AutoResizingComboBox {
                                id: overrideCombo
                                maximumWidth: 260
                                readonly property bool globalVal: pcSettingsDialog.overrideGlobalDefault(modelData.key)
                                readonly property int dialogOpenGen: pcSettingsDialog.openGeneration
                                model: [
                                    qsTr("Global Default (%1)").arg(globalVal ? qsTr("On") : qsTr("Off")),
                                    qsTr("Enabled"),
                                    qsTr("Disabled")
                                ]
                                function syncIndex() {
                                    var s = pcSettingsDialog.overrideValue(modelData.key)
                                    currentIndex = s < 0 ? 0 : (s > 0 ? 1 : 2)
                                }
                                Component.onCompleted: {
                                    syncIndex()
                                    recalculateWidth()
                                }
                                onDialogOpenGenChanged: {
                                    syncIndex()
                                    recalculateWidth()
                                }
                                onModelChanged: recalculateWidth()
                                onActivated: {
                                    var nextState = (currentIndex === 0) ? -1 : (currentIndex === 1 ? 1 : 0)
                                    pcSettingsDialog.setOverrideValue(modelData.key, nextState)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails : "";
        text: showPcDetailsDialog.pcDetails
        imageSrc: window.tintedUrl(window.theme.accent, "res/baseline-help_outline-24px.svg")
        standardButtons: Dialog.Ok
    }

    ScrollBar.vertical: ScrollBar {}
}
