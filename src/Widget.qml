import QtQuick
import qs.Ui       // BarWidget, WidgetButton, PopupCard, PanelActionButton
import qs.Commons  // Color, Style tokens

// Hot-reloads on save only when the plugin is a real directory under
// ~/.config/omarchy/plugins; a symlinked checkout needs `omarchy-restart-shell`.
//
// Host injects: bar, moduleName, settings. Read config via setting().
BarWidget {
    id: root
    moduleName: "io.github.azapc.aula-f75-battery"  // must match manifest id

    readonly property bool showPercentage: setting("showPercentage", true)
    readonly property int lowThreshold: setting("lowThreshold", 20)
    readonly property bool notifyOnLow: setting("notifyOnLow", true)
    readonly property int pollSeconds: setting("pollSeconds", 30)

    // Shell-managed singleton, so every monitor's bar shares one reading rather
    // than each spawning the HID helper on its own timer.
    readonly property var service: root.bar && root.bar.shell
        ? root.bar.shell.serviceFor(root.moduleName) : null
    readonly property var device: service ? service.device : null

    function syncServiceSettings() {
        if (!service) return;
        service.lowThreshold = lowThreshold;
        service.notifyOnLow = notifyOnLow;
        service.pollSeconds = pollSeconds;
    }
    onServiceChanged: syncServiceSettings()
    onLowThresholdChanged: syncServiceSettings()
    onNotifyOnLowChanged: syncServiceSettings()
    onPollSecondsChanged: syncServiceSettings()
    Component.onCompleted: syncServiceSettings()

    readonly property int pct: device ? device.pct : 0
    readonly property bool charging: device ? device.charging === true : false
    readonly property bool low: device !== null && !charging && pct <= lowThreshold

    // A fixed keyboard icon rather than a battery-fill ramp, so this stays
    // readable as "the keyboard" in a bar that may already show a battery.
    readonly property string keyboardGlyph: String.fromCodePoint(0xF030C)
    readonly property string boltGlyph: String.fromCodePoint(0xF140B)
    readonly property string glyph: charging
        ? keyboardGlyph + boltGlyph
        : keyboardGlyph
    readonly property string label: showPercentage
        ? glyph + " " + pct + "%"
        : glyph

    // Collapse when there is no reading, so the bar keeps no dead gap.
    visible: device !== null
    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    WidgetButton {
        id: button
        anchors.fill: parent
        bar: root.bar
        text: root.label
        fontSize: Style.font.caption
        horizontalMargin: 6
        useActiveColor: false
        foreground: root.low
            ? (root.bar ? root.bar.urgent : Color.urgent)
            : (root.bar ? root.bar.barForeground : Color.foreground)
        tooltipText: root.device
            ? root.device.model + " " + root.pct + "%"
              + (root.charging ? " (charging)" : "")
            : "AULA F75"

        onPressed: function (b) { card.open = !card.open }
    }

    PopupCard {
        id: card
        anchorItem: button
        bar: root.bar
        contentWidth: Style.space(220)
        contentHeight: panel.implicitHeight + card.verticalContentInset

        Column {
            id: panel
            width: parent.width
            spacing: Style.spacing.md

            Text {
                text: root.device ? root.device.model : "AULA F75"
                color: Color.popups.text
                font.family: Style.font.family
                font.pixelSize: Style.font.body
            }

            Text {
                text: root.pct + "%"
                color: root.low ? Color.urgent : Color.popups.text
                font.family: Style.font.family
                font.pixelSize: Style.font.body * 2
            }

            Rectangle {
                width: parent.width
                height: Style.space(6)
                radius: height / 2
                color: Qt.rgba(Color.popups.text.r, Color.popups.text.g,
                    Color.popups.text.b, 0.15)

                Rectangle {
                    width: parent.width * Math.max(0, Math.min(100, root.pct)) / 100
                    height: parent.height
                    radius: parent.radius
                    color: root.low ? Color.urgent : Color.popups.text
                }
            }

            // Only charging earns a word. Discharging is the resting state of a
            // wireless keyboard, so naming it says nothing.
            Text {
                visible: root.charging
                text: "Charging"
                color: Color.muted
                font.family: Style.font.family
                font.pixelSize: Style.font.caption
            }

            PanelActionButton {
                iconText: String.fromCodePoint(0xF0450)  // nf-md-refresh
                tooltipText: "Refresh"
                onClicked: if (root.service) root.service.refresh()
            }
        }
    }
}
