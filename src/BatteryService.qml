import QtQuick
import Quickshell.Io

// Service singletons are not re-created on hot reload, so edits here only take
// effect after `omarchy-restart-shell`.
//
// Headless singleton: one instance per shell however many monitors have a bar,
// so the helper runs once per poll rather than once per screen. Widget.qml
// binds to `device`.
Item {
    id: svc

    property var device: null          // {pct, state, charging, model} or null
    property int lowThreshold: 20
    property bool notifyOnLow: true
    property int pollSeconds: 30
    property int presenceSeconds: 15

    // Failed polls tolerated before the widget goes away. Applies only to an
    // asleep keyboard; an absent receiver clears immediately.
    property int missesBeforeClearing: 3
    property int _misses: 0
    property bool _warnedNoAccess: false

    property string _helperPath: ""
    readonly property string _buildScriptPath: localPath("build_aula_f75_battery.sh")

    function localPath(relativePath) {
        var url = Qt.resolvedUrl(relativePath).toString();
        return url.startsWith("file://") ? decodeURIComponent(url.substring(7)) : url;
    }

    // Compiles on first run, no-op afterwards: the script caches the binary
    // under a source-checksum directory.
    Process {
        id: build
        command: ["sh", svc._buildScriptPath]
        stdout: StdioCollector { id: buildOutput }
        stderr: StdioCollector { id: buildError }
        onExited: function (code, status) {
            if (code !== 0) {
                console.warn("AULA F75 battery: helper build failed:", code,
                    buildError.text);
                return;
            }
            svc._helperPath = buildOutput.text.trim();
            if (svc._helperPath) probe.running = true;
        }
    }

    // The helper prints one line and exits immediately, so stdout is collected
    // rather than stream-parsed. With SplitParser the exit signal can arrive
    // before the line does, and this handler would blank a good reading.
    Process {
        id: probe
        command: svc._helperPath ? [svc._helperPath] : []
        stdout: StdioCollector { id: probeOutput }
        stderr: StdioCollector { id: probeError }
        onExited: function (code, status) {
            // Arm the access warning again as soon as anything but 4 comes
            // back, so a problem that recurs is reported rather than swallowed.
            if (code !== 4) svc._warnedNoAccess = false;

            if (svc._apply(probeOutput.text)) {
                svc._misses = 0;
                return;
            }

            // 3 means the receiver is gone, 4 that it is here but unopenable.
            // Neither is worth waiting on, so clear at once. 4 is a setup
            // problem the user has to fix, so say so - once, not every poll.
            if (code === 3 || code === 4) {
                svc._misses = 0;
                svc.device = null;
                if (code === 4 && !svc._warnedNoAccess) {
                    svc._warnedNoAccess = true;
                    console.warn("AULA F75 battery: receiver present but not "
                        + "readable. Install the udev rule: sudo ./install.sh");
                }
                return;
            }

            // 1 means present but silent, which is just a sleeping keyboard.
            // Blanking on the first miss would flicker the widget off an idle
            // desk, so a known reading is held for a few polls.
            svc._misses += 1;
            if (svc._misses >= svc.missesBeforeClearing) svc.device = null;

            if (code > 1)
                console.warn("AULA F75 battery helper failed:", code,
                    probeError.text);
        }
    }

    // Last non-empty line, so this holds whether the collector is cleared
    // between runs or accumulates across them.
    function _apply(text) {
        var lines = String(text || "").split("\n");

        for (var i = lines.length - 1; i >= 0; i--) {
            var line = lines[i].trim();

            if (line === "") continue;

            try {
                var update = JSON.parse(line);

                if (typeof update.pct !== "number") return false;

                svc.device = {
                    pct: update.pct,
                    state: update.state,
                    charging: update.charging === true,
                    model: update.model || "AULA F75"
                };
                svc._maybeNotify();
                return true;
            } catch (error) {
                console.warn("AULA F75 battery: invalid helper output:", error);
                return false;
            }
        }

        return false;
    }

    function refresh() {
        if (!svc._helperPath) {
            if (!build.running) build.running = true;
            return;
        }
        if (!probe.running) probe.running = true;
    }

    // Presence costs an open and an ioctl and sends the keyboard nothing, so it
    // runs far more often than the battery read. Without it an unplugged dongle
    // would sit on the bar until the next poll.
    Process {
        id: presence
        command: svc._helperPath ? [svc._helperPath, "--present"] : []
        onExited: function (code, status) {
            if (code === 3 || code === 4) {
                svc._misses = 0;
                svc.device = null;
            } else if (code === 0 && svc.device === null) {
                svc.refresh();
            }
        }
    }

    Timer {
        interval: svc.presenceSeconds * 1000
        running: true
        repeat: true
        onTriggered: {
            if (svc._helperPath && !presence.running && !probe.running)
                presence.running = true;
        }
    }

    // Charge state can only be learned by asking: with the mode switch on 2.4G
    // the USB-C cable supplies power without enumerating anything, so plugging
    // it in raises no event to react to. Hence seconds, not minutes.
    Timer {
        interval: Math.max(5, svc.pollSeconds) * 1000
        running: true
        repeat: true
        onTriggered: svc.refresh()
    }

    // Once per dip: charging or climbing back over the threshold re-arms it, so
    // a keyboard sitting at 18% does not notify on every poll.
    property bool _notified: false

    Process { id: notifier }

    function _maybeNotify() {
        if (!svc.device) return;

        if (svc.device.charging || svc.device.pct > svc.lowThreshold) {
            svc._notified = false;
            return;
        }

        if (!svc.notifyOnLow || svc._notified) return;

        svc._notified = true;
        notifier.command = ["notify-send", "--app-name=AULA F75",
            "--urgency=critical", "--icon=battery-caution",
            "Keyboard battery low", svc.device.pct + "% remaining"];
        notifier.running = true;
    }

    Component.onCompleted: build.running = true
}
