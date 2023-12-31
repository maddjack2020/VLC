/*****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * Author: Prince Gupta <guptaprince8832@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * ( at your option ) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

import QtQuick 2.12
import QtQuick.Templates 2.12 as T
import QtQml.Models 2.12

import QtGraphicalEffects 1.12

import org.videolan.vlc 0.1

import "qrc:///style/"
import "qrc:///playlist/" as Playlist
import "qrc:///util/Helpers.js" as Helpers
import "qrc:///util/" as Util

Item {
    id: dragItem

    //---------------------------------------------------------------------------------------------
    // Properties
    //---------------------------------------------------------------------------------------------

    readonly property int coverSize: VLCStyle.icon_normal

    property var indexes: []

    property string defaultCover: VLCStyle.noArtAlbumCover

    property string defaultText: I18n.qtr("Unknown")

    // function(index, data) - returns cover for the index in the model in the form {artwork: <string> (file-name), cover: <component>}
    property var coverProvider: null

    // string => role
    property string coverRole: "cover"

    // function(index, data) - returns title text for the index in the model i.e <string> title
    property var titleProvider: null

    // string => role
    property string titleRole: "title"

    readonly property ColorContext colorContext: ColorContext {
        id: theme
        colorSet: ColorContext.Window
    }

    signal requestData(var indexes, var resolve, var reject)
    signal requestInputItems(var indexes, var data, var resolve, var reject)

    function coversXPos(index) {
        return VLCStyle.margin_small + (coverSize / 3) * index;
    }

    /**
      * @return {Promise} Promise object of the input items
      */
    function getSelectedInputItem() {
        if (_inputItems)
            return Promise.resolve(dragItem._inputItems)
        else if (dragItem._dropPromise)
            return dragItem._dropPromise
        else
            dragItem._dropPromise = new Promise((resolve, reject) => {
                dragItem._dropCallback = resolve
                dragItem._dropFailedCallback = reject
            })
            return dragItem._dropPromise
    }

    //---------------------------------------------------------------------------------------------
    // Private

    readonly property int _maxCovers: 3

    readonly property int _indexesSize: !!indexes ? indexes.length : 0

    readonly property int _displayedCoversCount: Math.min(_indexesSize, _maxCovers + 1)

    property var _inputItems: []

    property var _data: []

    property var _covers: []

    property string _title: ""

    property int _currentRequest: 0

    property var _dropPromise: null
    property var _dropCallback: null
    property var _dropFailedCallback: null

    //---------------------------------------------------------------------------------------------
    // Implementation
    //---------------------------------------------------------------------------------------------

    parent: g_mainDisplay

    width: VLCStyle.colWidth(2)

    height: coverSize + VLCStyle.margin_small * 2

    opacity: visible ? 0.90 : 0

    visible: Drag.active
    enabled: visible

    function _setData(data) {
        console.assert(data.length === indexes.length)
        _data = data

        const covers = []
        const titleList = []

        for (let i in indexes) {
            if (covers.length === _maxCovers)
                break

            const cover = _getCover(indexes[i], data[i])
            const itemTitle = _getTitle(indexes[i], data[i])
            if (!cover || !itemTitle)
                continue

            covers.push(cover)
            titleList.push(itemTitle)
        }

        if (covers.length === 0)
            covers.push({artwork: dragItem.defaultCover})

        if (titleList.length === 0)
            titleList.push(defaultText)

        _covers = covers
        _title = titleList.join(",") + (indexes.length > _maxCovers ? "..." : "")
    }

    function _setInputItems(inputItems) {
        if (!Array.isArray(inputItems) || inputItems.length === 0) {
            console.warn("can't convert items to input items");
            dragItem._inputItems = null
            return
        }

        dragItem._inputItems = inputItems
    }

    function _getCover(index, data) {
        console.assert(dragItem.coverRole)
        if (!!dragItem.coverProvider)
            return dragItem.coverProvider(index, data)
        else
            return {artwork: data[dragItem.coverRole] || dragItem.defaultCover}
    }

    function _getTitle(index, data) {
        console.assert(dragItem.titleRole)
        if (!!dragItem.titleProvider)
            return dragItem.titleProvider(index, data)
        else
            return data[dragItem.titleRole] || dragItem.defaultText
    }

    //NoRole because I'm not sure we need this to be accessible
    //can drag items be considered Tooltip ? or is another class better suited
    Accessible.role: Accessible.NoRole
    Accessible.name: I18n.qtr("drag item")

    Drag.onActiveChanged: {
        if (Drag.active) {
            fsm.startDrag()
        } else {
            fsm.stopDrag()
        }
    }

    Behavior on opacity {
        NumberAnimation {
            easing.type: Easing.InOutSine
            duration: VLCStyle.duration_short
        }
    }

    Util.FSM {
        id: fsm

        signal startDrag()
        signal stopDrag()

        //internal signals
        signal resolveData(var requestId, var indexes)
        signal resolveInputItems(var requestId, var indexes)
        signal resolveFailed()

        signalMap: ({
            startDrag: startDrag,
            stopDrag: stopDrag,
            resolveData: resolveData,
            resolveInputItems: resolveInputItems,
            resolveFailed: resolveFailed
        })

        initialState: fsmDragInactive

        Util.FSMState {
            id: fsmDragInactive

            function enter() {
                _title = ""
                _covers = []
                _data = []
            }

            transitions: ({
                startDrag: fsmDragActive
            })
        }

        Util.FSMState {
            id: fsmDragActive

            initialState: fsmRequestData

            function enter() {
                MainCtx.setCursor(Qt.DragMoveCursor)
            }

            function exit() {
                MainCtx.restoreCursor()
                if (dragItem._dropFailedCallback) {
                    dragItem._dropFailedCallback()
                }
                dragItem._dropPromise = null
                dragItem._dropFailedCallback = null
                dragItem._dropCallback = null
            }

            transitions: ({
                stopDrag: fsmDragInactive
            })

            Util.FSMState {
                id: fsmRequestData

                function enter() {
                    const requestId = ++dragItem._currentRequest
                    dragItem.requestData(
                        dragItem.indexes,
                        (data) => fsm.resolveData(requestId, data),
                        fsm.resolveFailed)
                }

                transitions: ({
                    resolveData: {
                        guard: (requestId, data) => requestId === dragItem._currentRequest,
                        action: (requestId, data) => {
                            dragItem._setData(data)
                        },
                        target: fsmRequestInputItem
                    },
                    resolveFailed: fsmLoadingFailed
                })
            }

            Util.FSMState {
                id: fsmRequestInputItem

                function enter() {
                    const requestId = ++dragItem._currentRequest
                    dragItem.requestInputItems(
                        dragItem.indexes, _data,
                        (items) => { fsm.resolveInputItems(requestId, items) },
                        fsm.resolveFailed)
                }

                transitions: ({
                    resolveInputItems: {
                        guard: (requestId, items) => requestId === dragItem._currentRequest,
                        action: (requestId, items) => {
                            dragItem._setInputItems(items)
                        },
                        target: fsmLoadingDone,
                    },
                    resolveFailed: fsmLoadingFailed
                })
            }

            Util.FSMState {
                id: fsmLoadingDone

                function enter() {
                    if (dragItem._dropCallback) {
                        dragItem._dropCallback(dragItem._inputItems)
                    }
                    dragItem._dropPromise = null
                    dragItem._dropCallback = null
                    dragItem._dropFailedCallback = null
                }
            }

            Util.FSMState {
                id: fsmLoadingFailed
                function enter() {
                    if (dragItem._dropFailedCallback) {
                        dragItem._dropFailedCallback()
                    }
                    dragItem._dropPromise = null
                    dragItem._dropCallback = null
                    dragItem._dropFailedCallback = null
                }
            }
        }
    }

    Rectangle {
        /* background */
        anchors.fill: parent
        color: fsmLoadingFailed.active ? theme.bg.negative : theme.bg.primary
        border.color: theme.border
        border.width: VLCStyle.dp(1, VLCStyle.scale)
        radius: VLCStyle.dp(6, VLCStyle.scale)
    }

    RectangularGlow {
        anchors.fill: parent
        glowRadius: VLCStyle.dp(8, VLCStyle.scale)
        color: theme.shadow
        spread: 0.2
        z: -1
    }

    Repeater {
        id: coverRepeater

        model: dragItem._covers

        Item {
            x: dragItem.coversXPos(index)
            y: (dragItem.height - height) / 2
            width: dragItem.coverSize
            height: dragItem.coverSize

            Rectangle {
                id: bg

                radius: coverRepeater.count > 1 ? dragItem.coverSize : VLCStyle.dp(2, VLCStyle.scale)
                anchors.fill: parent
                color: theme.bg.primary

                DoubleShadow {
                    anchors.fill: parent

                    z: -1

                    xRadius: bg.radius
                    yRadius: bg.radius

                    primaryBlurRadius: VLCStyle.dp(3)
                    primaryVerticalOffset: VLCStyle.dp(1, VLCStyle.scale)
                    primaryHorizontalOffset: 0

                    secondaryBlurRadius: VLCStyle.dp(14)
                    secondaryVerticalOffset: VLCStyle.dp(6, VLCStyle.scale)
                    secondaryHorizontalOffset: 0
                }
            }


            Loader {
                // parent may provide extra data with covers
                property var model: modelData

                anchors.centerIn: parent
                sourceComponent: (!modelData.artwork || modelData.artwork.toString() === "") ? modelData.cover : artworkLoader
                layer.enabled: true
                layer.effect: OpacityMask {
                    maskSource: Rectangle {
                        width: bg.width
                        height: bg.height
                        radius: bg.radius
                        visible: false
                    }
                }

                onItemChanged: {
                    if (modelData.artwork && modelData.artwork.toString() !== "")
                        item.source = modelData.artwork
                }
            }

            Rectangle {
                // for cover border
                color: "transparent"
                border.width: VLCStyle.dp(1, VLCStyle.scale)
                border.color: theme.border
                anchors.fill: parent
                radius: bg.radius
            }
        }
    }

    Rectangle {
        id: extraCovers

        x: dragItem.coversXPos(_maxCovers)
        y: (dragItem.height - height) / 2
        width: dragItem.coverSize
        height: dragItem.coverSize
        radius: dragItem.coverSize
        visible: dragItem._indexesSize > dragItem._maxCovers
        color: theme.bg.secondary
        border.width: VLCStyle.dp(1, VLCStyle.scale)
        border.color: theme.border

        MenuLabel {
            anchors.centerIn: parent
            color: theme.accent
            text: "+" + (dragItem._indexesSize - dragItem._maxCovers)
        }

        DoubleShadow {
            z: -1
            anchors.fill: parent
            xRadius: extraCovers.radius
            yRadius: extraCovers.radius

            primaryBlurRadius: VLCStyle.dp(3)
            primaryVerticalOffset: VLCStyle.dp(1, VLCStyle.scale)
            primaryHorizontalOffset: 0

            secondaryBlurRadius: VLCStyle.dp(14)
            secondaryVerticalOffset: VLCStyle.dp(6, VLCStyle.scale)
            secondaryHorizontalOffset: 0
        }
    }


    Column {
        id: labelColumn

        anchors.verticalCenter: parent.verticalCenter
        x: dragItem.coversXPos(_displayedCoversCount - 1) + dragItem.coverSize + VLCStyle.margin_small
        width: parent.width - x - VLCStyle.margin_small
        spacing: VLCStyle.margin_xxxsmall

        ScrollingText {
            label: titleLabel
            height: VLCStyle.fontHeight_large
            width: parent.width

            clip: scrolling
            forceScroll: dragItem.visible
            hoverScroll: false

            T.Label {
                id: titleLabel

                text: dragItem._title
                visible: text && text !== ""
                font.pixelSize: VLCStyle.fontSize_large
                color: theme.fg.primary
            }
        }

        MenuCaption {
            id: subtitleLabel

            visible: text && text !== ""
            width: parent.width
            text: I18n.qtr("%1 selected").arg(dragItem._indexesSize)
            color: theme.fg.secondary
        }
    }

    Component {
        id: artworkLoader

        ScaledImage {
            fillMode: Image.PreserveAspectCrop
            width: coverSize
            height: coverSize
            asynchronous: true
            cache: false
        }
    }
}
