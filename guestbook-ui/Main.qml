// Guestbook — sign a guestbook on the Logos Execution Zone testnet that
// everyone shares. Every copy of this module, on any machine, reads and
// writes the same book; the core (guestbook_core) does the wallet work and
// this file polls its state once a second.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root
    implicitWidth: 900
    implicitHeight: 760

    readonly property string coreId: "guestbook_core"

    property var st: ({})
    property string lastError: ""
    property string copied: ""
    property bool showDetails: false
    property bool nameLoaded: false

    readonly property bool ready: st.phase === "ready"
    readonly property var program: st.program || ({})
    readonly property var me: st.me || ({})
    readonly property var entries: st.entries || []
    readonly property var jobs: st.jobs || []
    readonly property var posting: jobs.filter(function (j) {
        return j.kind === "post" && j.status !== "done" && j.status !== "failed";
    })
    readonly property int textBytes: utf8Length(messageInput.text)

    // ── Logos bridge ─────────────────────────────────────────────────
    //   logos.callModule(id, method, [])            — synchronous, no-arg only
    //   logos.callModuleAsync(id, method, args, cb) — anything with arguments
    function call(method) {
        if (typeof logos === "undefined" || !logos.callModule)
            return null;
        return logos.callModule(coreId, method, []);
    }
    function callArgs(method, args, cb) {
        if (typeof logos === "undefined" || !logos.callModuleAsync) {
            lastError = "The Logos bridge is unavailable.";
            return;
        }
        logos.callModuleAsync(coreId, method, args, function (raw) {
            var r = unwrap(raw, null);
            if (r && r.ok === false)
                lastError = r.error || "Something went wrong.";
            refresh();
            if (cb)
                cb(r);
        });
    }
    // The bridge JSON-encodes the module's return value, so a JSON-returning
    // method arrives double-encoded: parse while it's still a string.
    function unwrap(raw, def) {
        if (raw === null || raw === undefined)
            return def;
        var v = raw;
        for (var i = 0; i < 3 && typeof v === "string"; ++i) {
            try {
                v = JSON.parse(v);
            } catch (e) {
                return (i === 0) ? def : v;
            }
        }
        return v;
    }
    function refresh() {
        var s = unwrap(call("state"), null);
        if (s && typeof s === "object") {
            st = s;
            if (!nameLoaded && ready) {
                nameInput.text = me.name || "";
                nameLoaded = true;
            }
            for (var i = 0; i < jobs.length; ++i)
                if (jobs[i].status === "failed" && Date.now() - jobs[i].finished < 1500)
                    lastError = jobs[i].error;
        }
    }
    function sign() {
        lastError = "";
        callArgs("post", [nameInput.text.trim(), messageInput.text.trim()], function (r) {
            if (r && r.ok)
                messageInput.text = "";
        });
    }

    function utf8Length(s) {
        var n = 0;
        for (var i = 0; i < s.length; ++i) {
            var c = s.charCodeAt(i);
            if (c < 0x80)
                n += 1;
            else if (c < 0x800)
                n += 2;
            else if (c >= 0xD800 && c <= 0xDBFF) {
                n += 4;   // a surrogate pair is one 4-byte character
                ++i;
            } else
                n += 3;
        }
        return n;
    }
    function shortAddr(a) {
        a = a || "";
        return a.length > 12 ? a.substring(0, 4) + "…" + a.substring(a.length - 4) : a;
    }
    function whenOf(sec) {
        if (!sec)
            return "";
        var ms = sec * 1000;
        var s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
        if (s < 60)
            return "just now";
        if (s < 3600)
            return Math.round(s / 60) + " min ago";
        if (s < 86400)
            return Math.round(s / 3600) + " h ago";
        return new Date(ms).toLocaleDateString(Qt.locale("en_US"), "d MMM yyyy");
    }
    function initials(name) {
        var n = (name || "").trim();
        if (!n.length)
            return "?";
        var parts = n.split(/\s+/);
        return (parts.length > 1 ? parts[0].charAt(0) + parts[1].charAt(0) : n.substring(0, 1)).toUpperCase();
    }
    function hueFor(seed) {
        var h = 0;
        var s = seed || "";
        for (var i = 0; i < s.length; i++)
            h = (h * 31 + s.charCodeAt(i)) >>> 0;
        return (h % 360) / 360;
    }

    TextEdit {
        id: clip
        visible: false
    }
    function copy(t, key) {
        clip.text = t;
        clip.selectAll();
        clip.copy();
        copied = key;
        copyTimer.restart();
    }
    Timer {
        id: copyTimer
        interval: 1600
        onTriggered: root.copied = ""
    }
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Gradient {
        id: accentGrad
        GradientStop {
            position: 0.0
            color: "#F28E6B"
        }
        GradientStop {
            position: 1.0
            color: "#E1613A"
        }
    }

    component Card: Rectangle {
        id: card
        default property alias content: inner.data
        property int pad: Theme.spacing.xlarge
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 2 * pad
        color: Theme.palette.backgroundElevated
        radius: Theme.spacing.radiusXlarge
        border.color: Theme.palette.borderSecondary
        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.margins: card.pad
            spacing: Theme.spacing.medium
        }
    }

    component CopyRow: RowLayout {
        id: cr
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        spacing: Theme.spacing.medium
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            LogosText {
                text: cr.label
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
            }
            LogosText {
                Layout.fillWidth: true
                text: cr.value || "—"
                elide: Text.ElideMiddle
                font.family: "Menlo"
                color: Theme.palette.text
                font.pixelSize: Theme.typography.secondaryText
            }
        }
        LogosText {
            text: root.copied === cr.label ? "Copied ✓" : "Copy"
            visible: cr.value.length > 0
            color: Theme.palette.primary
            font.pixelSize: Theme.typography.secondaryText
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.copy(cr.value, cr.label)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ScrollView {
        id: scroller
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: Math.min(scroller.availableWidth - 2 * Theme.spacing.xlarge, 600)
            x: (scroller.availableWidth - width) / 2
            spacing: Theme.spacing.large

            // Header
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing.large
                spacing: Theme.spacing.medium
                ColumnLayout {
                    spacing: 0
                    LogosText {
                        text: "Guestbook"
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.panelTitleText
                        font.weight: Theme.typography.weightBold
                    }
                    LogosText {
                        text: root.st.count === null || root.st.count === undefined ? "Opening the book…" : root.st.count === 0 ? "No one has signed yet — be the first" : root.st.count + (root.st.count === 1 ? " signature" : " signatures") + " · on the Logos Execution Zone"
                        color: Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                Rectangle {
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: !root.ready ? Theme.palette.warning : root.st.network && root.st.network.online ? Theme.palette.success : Theme.palette.error
                }
                LogosText {
                    text: !root.ready ? (root.st.phase === "error" ? "Wallet error" : "Starting") : (root.st.network && root.st.network.online ? "Testnet" : "Offline")
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }

            // Error
            Rectangle {
                Layout.fillWidth: true
                visible: root.lastError.length > 0 || root.st.phase === "error"
                implicitHeight: errText.implicitHeight + 2 * Theme.spacing.medium
                radius: Theme.spacing.radiusLarge
                color: Theme.colors.getColor(Theme.palette.error, 0.14)
                LogosText {
                    id: errText
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.medium
                    text: root.st.phase === "error" ? root.st.error : root.lastError
                    wrapMode: Text.WordWrap
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.secondaryText
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.lastError = ""
                    }
                }
            }

            // Compose
            Card {
                LogosText {
                    text: "Sign the guestbook"
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                }
                LogosTextField {
                    id: nameInput
                    Layout.fillWidth: true
                    implicitHeight: 44
                    placeholderText: "Your name (optional)"
                }
                LogosTextArea {
                    id: messageInput
                    Layout.fillWidth: true
                    Layout.preferredHeight: 100
                    placeholderText: "Leave a message for everyone who comes after you…"
                    wrapMode: TextEdit.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    LogosText {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.posting.length ? (root.posting[0].status === "confirming" ? "Writing it to the chain · usually under a minute" : "Sending…") : root.textBytes > 500 ? "Too long by " + (root.textBytes - 500) + " characters" : "Signed with your wallet and kept forever — it can't be edited or deleted."
                        color: root.posting.length ? Theme.palette.warning : root.textBytes > 500 ? Theme.palette.error : Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                    Rectangle {
                        id: signBtn
                        readonly property bool can: root.ready && !!root.program.configured && messageInput.text.trim().length > 0 && root.textBytes <= 500
                        implicitWidth: signLbl.implicitWidth + 48
                        implicitHeight: 46
                        radius: 23
                        gradient: can ? accentGrad : null
                        color: Theme.palette.backgroundSecondary
                        opacity: can ? 1 : 0.45
                        RowLayout {
                            anchors.centerIn: parent
                            spacing: Theme.spacing.small
                            LogosSpinner {
                                visible: root.posting.length > 0
                                running: visible
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                ringColor: "#ffffff"
                                thickness: 2
                                dotSize: 4
                            }
                            LogosText {
                                id: signLbl
                                text: "Sign"
                                color: "#ffffff"
                                font.pixelSize: Theme.typography.primaryText
                                font.weight: Theme.typography.weightBold
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: signBtn.can
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: root.sign()
                        }
                    }
                }
            }

            // Entries
            Repeater {
                model: root.entries
                delegate: Rectangle {
                    id: entry
                    required property var modelData
                    readonly property string who: modelData.name && modelData.name.length ? modelData.name : "Anonymous"
                    Layout.fillWidth: true
                    implicitHeight: entryCol.implicitHeight + 2 * Theme.spacing.large
                    radius: Theme.spacing.radiusXlarge
                    color: Theme.palette.backgroundElevated
                    border.color: modelData.mine ? Theme.colors.getColor(Theme.palette.primary, 0.6) : Theme.palette.borderSecondary
                    ColumnLayout {
                        id: entryCol
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.small
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            Rectangle {
                                implicitWidth: 36
                                implicitHeight: 36
                                radius: 18
                                color: Qt.hsla(root.hueFor(entry.modelData.author), 0.45, 0.42, 1)
                                LogosText {
                                    anchors.centerIn: parent
                                    text: root.initials(entry.who)
                                    color: "#ffffff"
                                    font.pixelSize: 14
                                    font.weight: Theme.typography.weightBold
                                }
                            }
                            ColumnLayout {
                                spacing: 0
                                RowLayout {
                                    spacing: Theme.spacing.small
                                    LogosText {
                                        text: entry.who
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.primaryText
                                        font.weight: Theme.typography.weightBold
                                    }
                                    LogosBadge {
                                        visible: entry.modelData.mine
                                        text: "You"
                                    }
                                }
                                LogosText {
                                    text: root.shortAddr(entry.modelData.author) + " · " + root.whenOf(entry.modelData.time)
                                    color: Theme.palette.textTertiary
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            LogosText {
                                text: "#" + (entry.modelData.index + 1)
                                color: Theme.palette.textMuted
                                font.pixelSize: Theme.typography.secondaryText
                            }
                        }
                        LogosText {
                            Layout.fillWidth: true
                            text: entry.modelData.text
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.primaryText
                            lineHeight: 1.25
                        }
                    }
                }
            }

            LogosText {
                Layout.alignment: Qt.AlignHCenter
                visible: root.st.hasMore === true
                text: "Show older signatures"
                color: Theme.palette.primary
                font.pixelSize: Theme.typography.secondaryText
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.callArgs("loadMore", [])
                }
            }

            // Details
            LogosText {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.spacing.medium
                text: root.showDetails ? "Hide details" : "Details"
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showDetails = !root.showDetails
                }
            }
            Card {
                visible: root.showDetails
                CopyRow {
                    label: "Guestbook program"
                    value: root.program.address || ""
                }
                CopyRow {
                    label: "Header account (the signature count)"
                    value: root.program.header || ""
                }
                CopyRow {
                    label: "Your wallet (signs your posts)"
                    value: root.me.address || ""
                }
                RowLayout {
                    Layout.fillWidth: true
                    LogosText {
                        Layout.fillWidth: true
                        text: (root.me.lgo || "0") + " LGO for fees · topped up from the testnet faucet automatically"
                        wrapMode: Text.WordWrap
                        color: Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                    LogosText {
                        text: "Get LGO"
                        color: Theme.palette.primary
                        font.pixelSize: Theme.typography.secondaryText
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.callArgs("getGas", [])
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Theme.palette.borderSecondary
                }
                LogosText {
                    Layout.fillWidth: true
                    text: "Use a different deployment (after a testnet reset, or your own book). Leave empty for the built-in one."
                    wrapMode: Text.WordWrap
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    LogosTextField {
                        id: programField
                        Layout.fillWidth: true
                        implicitHeight: 40
                        placeholderText: "Program address"
                        text: root.program.custom ? root.program.address : ""
                    }
                    LogosButton {
                        implicitWidth: 80
                        implicitHeight: 40
                        text: "Use"
                        onClicked: root.callArgs("setProgram", [programField.text.trim()])
                    }
                }
            }

            Item {
                implicitHeight: Theme.spacing.xxlarge
            }
        }
    }
}
