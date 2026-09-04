// SPDX-License-Identifier: MIT

import QtQuick
import QtCore

import org.kde.plasma.private.mobileshell as MobileShell
import org.kde.plasma.private.mobileshell.quicksettingsplugin as QS

QS.QuickSetting {
    id: root

    property bool switching: false

    Settings {
        id: settings
        category: "InputModeQuickSetting"
        property bool hardwareMode: false
    }

    text: i18n("Input Mode")
    icon: settings.hardwareMode ? "input-keyboard" : "input-keyboard-virtual"
    status: settings.hardwareMode ? i18n("Hardware · IBus") : i18n("Touch · Plasma")
    enabled: settings.hardwareMode

    Timer {
        id: switchGuard
        interval: 3000
        onTriggered: root.switching = false
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: settings.sync()
    }

    function toggle() {
        if (switching) {
            return;
        }

        switching = true;
        settings.hardwareMode = !settings.hardwareMode;
        MobileShell.ShellUtil.executeCommand(
            "/usr/bin/plasma-input-mode "
            + (settings.hardwareMode ? "hardware" : "touch")
        );
        switchGuard.restart();
    }
}
