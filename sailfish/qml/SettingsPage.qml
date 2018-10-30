/* Copyright (C) 2017 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: root

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: root.width
            spacing: Theme.paddingMedium

            PageHeader {
                title: qsTr("Settings")
            }

            TextSwitch {
                automaticCheck: false
                checked: settings.rememberPlaylist
                text: qsTr("Start with last playlist")
                description: qsTr("When Jupii connects to a device, the last " +
                                  "playlist will be automatically loaded.")
                onClicked: {
                    settings.rememberPlaylist = !settings.rememberPlaylist
                }
            }

            Slider {
                width: parent.width
                minimumValue: 1
                maximumValue: 60
                stepSize: 1
                handleVisible: true
                value: settings.forwardTime
                valueText: value + " s"
                label: qsTr("Forward/backward time-step interval")

                onValueChanged: {
                    settings.forwardTime = value
                }
            }

            SectionHeader {
                text: qsTr("Experiments")
            }

            TextSwitch {
                automaticCheck: false
                checked: settings.useDbusVolume
                text: qsTr("Volume control with hardware keys")
                description: qsTr("Change volume level using phone hardware volume keys. " +
                                  "The volume level of the media device will be " +
                                  "set to be the same as the volume level of the ringing alert " +
                                  "on the phone.")
                onClicked: {
                    settings.useDbusVolume = !settings.useDbusVolume
                }
            }

            TextSwitch {
                automaticCheck: false
                checked: settings.imageSupported
                text: qsTr("Image content")
                description: qsTr("Playing images on UPnP devices doesn't work well right now. " +
                                  "There are few minor issues that have not been resolved yet. " +
                                  "This option forces %1 to play images despite the fact " +
                                  "it could cause some issues.").arg(APP_NAME)
                onClicked: {
                    settings.imageSupported = !settings.imageSupported
                }
            }

            TextSwitch {
                automaticCheck: false
                checked: settings.showAllDevices
                text: qsTr("All devices visible")
                description: qsTr("%1 supports only Media Renderer devices. With this option enabled, " +
                                  "all UPnP devices will be shown, including unsupported devices like " +
                                  "home routers or Media Servers. For unsupported devices %1 is able " +
                                  "to show only basic description information. " +
                                  "This option could be useful for auditing UPnP devices " +
                                  "in your local network.").arg(APP_NAME)
                onClicked: {
                    settings.showAllDevices = !settings.showAllDevices
                }
            }

            TextSwitch {
                automaticCheck: false
                checked: settings.ssdpIpEnabled
                text: qsTr("Adding devices manually")
                description: qsTr("If %1 fails to discover a device " +
                             "(e.g. because it is in a different LAN), you can " +
                             "add it manually with IP address. " +
                             "When enabled, pull down menu contains additional " +
                             "option to add device manually. " +
                             "Make sure that your device is not behind a NAT " +
                             "or a firewall.").arg(APP_NAME)
                onClicked: {
                    settings.ssdpIpEnabled = !settings.ssdpIpEnabled
                }
            }

            ComboBox {
                label: qsTr("Internet streaming mode")
                description: qsTr("Streaming from the Internet to UPnP devices can be handled in two modes: Proxy (default) or Redirection. In Proxy mode, %1 relays every packet received from a streaming host (e.g. internet radio server) to a UPnP device located in your home network. This mode is transparent for a UPnP device, so it works in most cases. Because packets goes through your phone/tablet, %1 must be enabled all the time to make a streaming working. In Redirection mode, %1 uses HTTP redirection to instruct UPnP device where internet host is located. The actual streaming goes directly between UPnP device and a streaming server, so %1 in not required to be enabled all the time. The downside of Redirection mode is that not every UPnP device supports redirection. Therefore on some devices this mode will not work properly.").arg(APP_NAME)
                currentIndex: settings.remoteContentMode
                menu: ContextMenu {
                    MenuItem { text: "Proxy" }
                    MenuItem { text: "Redirection" }
                }

                onCurrentIndexChanged: {
                    settings.remoteContentMode = currentIndex
                }
            }

            Spacer {}
        }
    }

    VerticalScrollDecorator {
        flickable: flick
    }
}
