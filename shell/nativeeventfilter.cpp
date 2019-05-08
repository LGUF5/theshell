/****************************************
 *
 *   theShell - Desktop Environment
 *   Copyright (C) 2019 Victor Tran
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * *************************************/

/****************************************
 *
 *   Parts of this file are adapted from source code of KWin, whose code
 *   is licensed under the GPL (version 2 or later). The copyright message
 *   can be found below:
 *
 *   KWin - the KDE window manager
 *
 *   Copyright (C) 2016 Martin Gräßlin <mgraesslin@kde.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * *************************************/

#include "nativeeventfilter.h"

#include "mainwindow.h"
#include "menu.h"

#include "soundengine.h"

#include <X11/XF86keysym.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>

#define Bool int
#define Status int
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XI2proto.h>

extern void EndSession(EndSessionWait::shutdownType type);
extern DbusEvents* DBusEvents;
extern MainWindow* MainWin;
extern AudioManager* AudioMan;
extern ScreenRecorder* screenRecorder;


class GeEventMemMover
{
public:
    GeEventMemMover(xcb_generic_event_t *event)
        : m_event(reinterpret_cast<xcb_ge_generic_event_t *>(event))
    {
        // xcb event structs contain stuff that wasn't on the wire, the full_sequence field
        // adds an extra 4 bytes and generic events cookie data is on the wire right after the standard 32 bytes.
        // Move this data back to have the same layout in memory as it was on the wire
        // and allow casting, overwriting the full_sequence field.
        memmove((char*) m_event + 32, (char*) m_event + 36, m_event->length * 4);
    }
    ~GeEventMemMover()
    {
        // move memory layout back, so that Qt can do the same without breaking
        memmove((char*) m_event + 36, (char *) m_event + 32, m_event->length * 4);
    }

    xcb_ge_generic_event_t *operator->() const {
        return m_event;
    }

private:
    xcb_ge_generic_event_t *m_event;
};

static inline qreal fixed1616ToReal(FP1616 val)
{
    return (val) * 1.0 / (1 << 16);
}

struct NativeEventFilterPrivate {
    enum TouchTrackingType {
        None,
        GatewayOpen
    };

    QTime lastPress;
    HotkeyHud* Hotkeys;

    bool isEndSessionBoxShowing = false;
    bool ignoreSuper = false;

    QSettings settings;
    QSettings* themeSettings = new QSettings("theSuite", "ts-qtplatform");

    QTimer* powerButtonTimer = nullptr;
    bool powerPressed = false;

    int systrayOpcode, xiOpcode;
    int touchTracking = 0;
    TouchTrackingType touchTrackingType;

    QHash<uint32_t, QPointF> firstTouchPoints;
    QVector<QPointF> touchPoints;
};

NativeEventFilter::NativeEventFilter(QObject* parent) : QObject(parent)
{
    d = new NativeEventFilterPrivate();

    //Create the Hotkey window and set appropriate flags
    d->Hotkeys = new HotkeyHud();
    d->Hotkeys->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    d->Hotkeys->setAttribute(Qt::WA_ShowWithoutActivating, true);

    d->powerButtonTimer = new QTimer();
    d->powerButtonTimer->setInterval(500);
    d->powerButtonTimer->setSingleShot(true);
    connect(d->powerButtonTimer, SIGNAL(timeout()), this, SLOT(handlePowerButton()));

    //Capture required keys
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_MonBrightnessUp), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_MonBrightnessDown), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_KbdBrightnessUp), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_KbdBrightnessDown), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_AudioLowerVolume), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_AudioRaiseVolume), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_AudioMute), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_Eject), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_PowerOff), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_Sleep), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Print), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Delete), ControlMask | Mod1Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_L), Mod4Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F2), Mod1Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_P), Mod4Mask | Mod1Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_O), Mod4Mask | Mod1Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F1), Mod4Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F2), Mod4Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F3), Mod4Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Num_Lock), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Caps_Lock), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Return), Mod4Mask, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);

    //Check if the user wants to capture the super key
    if (d->settings.value("input/superkeyGateway", true).toBool()) {
        XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Super_L), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
        XGrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Super_R), AnyModifier, RootWindow(QX11Info::display(), 0), true, GrabModeAsync, GrabModeAsync);
    }

    //Start the Last Pressed timer to ignore repeated keys
    d->lastPress.start();

    //Get all opcodes needed
    //Get the System Tray opcode
    std::string name = "_NET_SYSTEM_TRAY_OPCODE";
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(QX11Info::connection(), xcb_intern_atom(QX11Info::connection(), 1, name.size(), name.c_str()), nullptr);
    d->systrayOpcode = reply ? reply->atom : XCB_NONE;
    free(reply);

    //Get the XInput opcode
    bool initXinput = true;
    int event, error;
    if (!XQueryExtension(QX11Info::display(), "XInputExtension", &d->xiOpcode, &event, &error)) {
        initXinput = false;
    }

    if (initXinput) {
        //Capture the touch screen
        XIEventMask masks[1];
        unsigned char mask1[XIMaskLen(XI_LASTEVENT)];

        memset(mask1, 0, sizeof(mask1));

        XISetMask(mask1, XI_TouchBegin);
        XISetMask(mask1, XI_TouchUpdate);
        XISetMask(mask1, XI_TouchEnd);
        XISetMask(mask1, XI_TouchOwnership);

        masks[0].deviceid = XIAllDevices;
        masks[0].mask_len = sizeof(mask1);
        masks[0].mask = mask1;

        XSetErrorHandler([](Display* d, XErrorEvent* e) {
            qDebug() << "X11 error:" << e->error_code;
            return 0;
        });
        XISelectEvents(QX11Info::display(), QX11Info::appRootWindow(), masks, 1);
    }
}

NativeEventFilter::~NativeEventFilter() {
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_MonBrightnessUp), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_MonBrightnessDown), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_KbdBrightnessUp), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_KbdBrightnessDown), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_AudioLowerVolume), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_AudioRaiseVolume), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_AudioMute), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_Eject), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_PowerOff), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XF86XK_Sleep), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Print), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Delete), ControlMask | Mod1Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_L), Mod4Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F2), Mod1Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Super_L), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Super_R), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_P), Mod4Mask | Mod1Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_O), Mod4Mask | Mod1Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F1), Mod4Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F2), Mod4Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_F3), Mod4Mask, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Num_Lock), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Caps_Lock), AnyModifier, QX11Info::appRootWindow());
    XUngrabKey(QX11Info::display(), XKeysymToKeycode(QX11Info::display(), XK_Return), Mod4Mask, QX11Info::appRootWindow());

    delete d;
}


bool NativeEventFilter::nativeEventFilter(const QByteArray &eventType, void *message, long *result) {
    Q_UNUSED(result)

    if (eventType == "xcb_generic_event_t") {
        xcb_generic_event_t* event = static_cast<xcb_generic_event_t*>(message);
        if (event->response_type == XCB_CLIENT_MESSAGE || event->response_type == (XCB_CLIENT_MESSAGE | 128)) { //System Tray Event
            //Get the message
            xcb_client_message_event_t* client = static_cast<xcb_client_message_event_t*>(message);

            if (client->type == d->systrayOpcode) {
                //Dock the system tray
                emit SysTrayEvent(client->data.data32[1], client->data.data32[2], client->data.data32[3], client->data.data32[4]);
            }
        } else if (event->response_type == XCB_GE_GENERIC) {
            GeEventMemMover ge(event);
            if (ge->extension == d->xiOpcode) {
                switch (ge->event_type) {
                    case XI_TouchBegin: {
                        xXIDeviceEvent* dEvent = reinterpret_cast<xXIDeviceEvent*>(event);

                        d->firstTouchPoints.insert(dEvent->detail, QPointF(fixed1616ToReal(dEvent->event_x), fixed1616ToReal(dEvent->event_y)));
                        break;
                    }
                    case XI_TouchUpdate: {
                        xXIDeviceEvent* dEvent = reinterpret_cast<xXIDeviceEvent*>(event);
                        if (d->touchTracking == dEvent->detail) {
                            switch (d->touchTrackingType) {
                                case NativeEventFilterPrivate::GatewayOpen:
                                    MainWin->getMenu()->showPartial(fixed1616ToReal(dEvent->event_x));
                                    break;
                            }

                            d->touchPoints.prepend(QPointF(fixed1616ToReal(dEvent->event_x), fixed1616ToReal(dEvent->event_y)));
                            if (d->touchPoints.count() > 10) d->touchPoints.removeLast();
                        }
                        break;
                    }
                    case XI_TouchEnd: {
                        xXIDeviceEvent* dEvent = reinterpret_cast<xXIDeviceEvent*>(event);

                        if (d->touchTracking == dEvent->detail) {
                            switch (d->touchTrackingType) {
                                case NativeEventFilterPrivate::GatewayOpen:
                                    if (!d->touchPoints.isEmpty() && d->touchPoints.last().x() < fixed1616ToReal(dEvent->event_x)) {
                                        MainWin->getMenu()->show();
                                    } else {
                                        MainWin->getMenu()->close();
                                    }
                            }

                            d->touchTracking = 0;
                            d->touchTrackingType = NativeEventFilterPrivate::None;
                        }
                        d->firstTouchPoints.remove(dEvent->detail);
                        break;
                    }
                    case XI_TouchOwnership: {
                        d->touchPoints.clear();

                        xXITouchOwnershipEvent* dEvent = reinterpret_cast<xXITouchOwnershipEvent*>(event);
                        if (d->firstTouchPoints.contains(dEvent->touchid)) {
                            QRect screenGeometry = QApplication::desktop()->screenGeometry();
                            QPointF point = d->firstTouchPoints.value(dEvent->touchid);
                            if (point.x() >= screenGeometry.x() && point.x() < screenGeometry.x() + 20 && !MainWin->getMenu()->isVisible() && d->settings.value("gestures/swipeGateway", true).toBool()) {
                                //Open the Gateway
                                d->touchTracking = dEvent->touchid;
                                d->touchTrackingType = NativeEventFilterPrivate::GatewayOpen;
                                XIAllowTouchEvents(QX11Info::display(), dEvent->deviceid, dEvent->touchid, QX11Info::appRootWindow(), XIAcceptTouch);
                                MainWin->getMenu()->prepareForShow();
                                return true;
                            }
                        }

                        //We don't know what to do with this, so reject it immediately
                        XIAllowTouchEvents(QX11Info::display(), dEvent->deviceid, dEvent->touchid, QX11Info::appRootWindow(), XIRejectTouch);
                        break;
                    }
                }
            }
        } else if (event->response_type == XCB_KEY_PRESS) { //Key Press Event
            if (d->lastPress.restart() > 100) {
                xcb_key_release_event_t* button = static_cast<xcb_key_release_event_t*>(message);

                //Get Current Brightness
                QProcess* backlight = new QProcess(this);
                backlight->start("xbacklight -get");
                backlight->waitForFinished();
                float currentBrightness = ceil(QString(backlight->readAll()).toFloat());
                delete backlight;

                //Get Current Volume
                int volume = AudioMan->MasterVolume();

                int kbdBrightness = -1, maxKbdBrightness = -1;
                QDBusInterface keyboardInterface("org.freedesktop.UPower", "/org/freedesktop/UPower/KbdBacklight", "org.freedesktop.UPower.KbdBacklight", QDBusConnection::systemBus());
                if (keyboardInterface.isValid()) {
                    kbdBrightness = keyboardInterface.call("GetBrightness").arguments().first().toInt();
                    maxKbdBrightness = keyboardInterface.call("GetMaxBrightness").arguments().first().toInt();
                }

                if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_MonBrightnessUp)) { //Increase brightness by 10%
                    currentBrightness = currentBrightness + 10;
                    if (currentBrightness > 100) currentBrightness = 100;

                    QProcess* backlightAdj = new QProcess(this);
                    backlightAdj->start("xbacklight -set " + QString::number(currentBrightness));
                    connect(backlightAdj, SIGNAL(finished(int)), backlightAdj, SLOT(deleteLater()));

                    d->Hotkeys->show(QIcon::fromTheme("video-display"), tr("Brightness"), (int) currentBrightness);
                } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_MonBrightnessDown)) { //Decrease brightness by 10%
                    currentBrightness = currentBrightness - 10;
                    if (currentBrightness < 0) currentBrightness = 0;

                    QProcess* backlightAdj = new QProcess(this);
                    backlightAdj->start("xbacklight -set " + QString::number(currentBrightness));
                    connect(backlightAdj, SIGNAL(finished(int)), backlightAdj, SLOT(deleteLater()));

                    d->Hotkeys->show(QIcon::fromTheme("video-display"), tr("Brightness"), (int) currentBrightness);
                } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_AudioRaiseVolume)) {
                    if (button->state & Mod4Mask) {
                        //Increase brightness
                        currentBrightness = currentBrightness + 10;
                        if (currentBrightness > 100) currentBrightness = 100;

                        QProcess* backlightAdj = new QProcess(this);
                        backlightAdj->start("xbacklight -set " + QString::number(currentBrightness));
                        connect(backlightAdj, SIGNAL(finished(int)), backlightAdj, SLOT(deleteLater()));

                        d->Hotkeys->show(QIcon::fromTheme("video-display"), tr("Brightness"), (int) currentBrightness);
                    } else {
                        //Increase volume
                        if (AudioMan->QuietMode() == AudioManager::mute) {
                            d->Hotkeys->show(QIcon::fromTheme("audio-volume-muted"), tr("Volume"), tr("Quiet Mode is set to Mute."));
                        } else {
                                volume = volume + 5;
                                if (volume - 5 < 100 && volume > 100) {
                                    volume = 100;
                                }
                                AudioMan->changeVolume(5);

                                //Play the audio change sound
                                SoundEngine::play(SoundEngine::Volume);

                                d->Hotkeys->show(QIcon::fromTheme("audio-volume-high"), tr("Volume"), volume);
                        }
                    }
                } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_AudioLowerVolume)) { //Decrease Volume by 5%
                    if (button->state & Mod4Mask) {
                        //Decrease brightness
                        currentBrightness = currentBrightness - 10;
                        if (currentBrightness < 0) currentBrightness = 0;

                        QProcess* backlightAdj = new QProcess(this);
                        backlightAdj->start("xbacklight -set " + QString::number(currentBrightness));
                        connect(backlightAdj, SIGNAL(finished(int)), backlightAdj, SLOT(deleteLater()));

                        d->Hotkeys->show(QIcon::fromTheme("video-display"), tr("Brightness"), (int) currentBrightness);
                    } else if (d->powerPressed) {
                        //Take a screenshot
                        screenshotWindow* screenshot = new screenshotWindow;
                        screenshot->show();
                        d->powerButtonTimer->stop();
                    } else {
                        if (AudioMan->QuietMode() == AudioManager::mute) {
                            d->Hotkeys->show(QIcon::fromTheme("audio-volume-muted"), tr("Volume"), tr("Quiet Mode is set to Mute."));
                        } else {
                            volume = volume - 5;
                            if (volume < 0) volume = 0;
                            AudioMan->changeVolume(-5);

                            //Play the audio change sound
                            SoundEngine::play(SoundEngine::Volume);

                            d->Hotkeys->show(QIcon::fromTheme("audio-volume-high"), tr("Volume"), volume);
                        }
                    }
                } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_AudioMute)) { //Toggle Quiet Mode
                    switch (AudioMan->QuietMode()) {
                        case AudioManager::none:
                            AudioMan->setQuietMode(AudioManager::critical);
                            d->Hotkeys->show(QIcon::fromTheme("quiet-mode-critical-only"), tr("Critical Only"), AudioMan->getCurrentQuietModeDescription(), 5000);
                            break;
                        case AudioManager::critical:
                            AudioMan->setQuietMode(AudioManager::notifications);
                            d->Hotkeys->show(QIcon::fromTheme("quiet-mode"), tr("No Notifications"), AudioMan->getCurrentQuietModeDescription(), 5000);
                            break;
                        case AudioManager::notifications:
                            AudioMan->setQuietMode(AudioManager::mute);
                            d->Hotkeys->show(QIcon::fromTheme("audio-volume-muted"), tr("Mute"), AudioMan->getCurrentQuietModeDescription(), 5000);
                            break;
                        case AudioManager::mute:
                            AudioMan->setQuietMode(AudioManager::none);
                            d->Hotkeys->show(QIcon::fromTheme("audio-volume-high"), tr("Sound"), AudioMan->getCurrentQuietModeDescription(), 5000);
                            break;
                    }
                } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_KbdBrightnessUp)) { //Increase keyboard brightness by 5%
                    kbdBrightness += (((float) maxKbdBrightness / 100) * 5);
                    if (kbdBrightness > maxKbdBrightness) kbdBrightness = maxKbdBrightness;
                    keyboardInterface.call("SetBrightness", kbdBrightness);

                    d->Hotkeys->show(QIcon::fromTheme("keyboard-brightness"), tr("Keyboard Brightness"), ((float) kbdBrightness / (float) maxKbdBrightness) * 100);
                } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_KbdBrightnessDown)) { //Decrease keyboard brightness by 5%
                    kbdBrightness -= (((float) maxKbdBrightness / 100) * 5);
                    if (kbdBrightness < 0) kbdBrightness = 0;
                    keyboardInterface.call("SetBrightness", kbdBrightness);

                    d->Hotkeys->show(QIcon::fromTheme("keyboard-brightness"), tr("Keyboard Brightness"), ((float) kbdBrightness / (float) maxKbdBrightness) * 100);
                } else if ((button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_PowerOff))) {
                    d->powerPressed = true;
                    if (!d->powerButtonTimer->isActive()) {
                        d->powerButtonTimer->start();
                    }
                }
            }
        } else if (event->response_type == XCB_KEY_RELEASE) {
            xcb_key_release_event_t* button = static_cast<xcb_key_release_event_t*>(message);

            if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_Eject)) { //Eject Disc
                QProcess* eject = new QProcess(this);
                eject->start("eject");
                connect(eject, SIGNAL(finished(int)), eject, SLOT(deleteLater()));

                d->Hotkeys->show(QIcon::fromTheme("media-eject"), tr("Eject"), tr("Attempting to eject disc..."));
            } else if ((button->detail == XKeysymToKeycode(QX11Info::display(), XK_P) && (button->state == (Mod4Mask | Mod1Mask))) ||
                       (button->detail == XKeysymToKeycode(QX11Info::display(), XK_Print)) ||
                       (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_PowerOff) && button->state == Mod4Mask)) { //Take screenshot
                if (button->state & Mod4Mask) {
                    d->ignoreSuper = true;
                }

                if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_Print) && button->state & ShiftMask) {
                    if (screenRecorder->recording()) {
                        screenRecorder->stop();
                    } else {
                        screenRecorder->start();
                    }
                } else {
                    screenshotWindow* screenshot = new screenshotWindow;
                    screenshot->show();
                }
            } else if ((button->detail == XKeysymToKeycode(QX11Info::display(), XK_O)) && (button->state == (Mod4Mask | Mod1Mask))) {
                if (screenRecorder->recording()) {
                    screenRecorder->stop();
                } else {
                    screenRecorder->start();
                }
                d->ignoreSuper = true;
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_PowerOff)) { //Power Off
                d->powerPressed = false;
                if (d->powerButtonTimer->isActive()) {
                    d->powerButtonTimer->stop();
                    handlePowerButton();
                }
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_Delete) && (button->state == (ControlMask | Mod1Mask))) {
                if (!d->isEndSessionBoxShowing) {
                    d->isEndSessionBoxShowing = true;

                    EndSessionWait* endSession;
                    if (d->settings.value("input/touch", false).toBool()) {
                        endSession = new EndSessionWait(EndSessionWait::slideOff);
                    } else {
                        endSession = new EndSessionWait(EndSessionWait::ask);
                    }
                    endSession->showFullScreen();
                    endSession->exec();

                    d->isEndSessionBoxShowing = false;
                }
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XF86XK_Sleep)) { //Suspend
                QList<QVariant> arguments;
                arguments.append(true);

                QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Suspend");
                message.setArguments(arguments);
                QDBusConnection::systemBus().send(message);
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_L) && button->state == Mod4Mask) { //Lock Screen
                d->ignoreSuper = true;
                DBusEvents->LockScreen();
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_F2) && button->state == Mod1Mask) { //Run
                RunDialog* run = new RunDialog();
                run->show();
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_Super_L) || button->detail == XKeysymToKeycode(QX11Info::display(), XK_Super_R)) {
                if (!d->ignoreSuper) { //Check that the user is not doing a key combination
                    MainWin->openMenu();
                }
                d->ignoreSuper = false;
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_F1) && (button->state == Mod4Mask)) {
                MainWin->getInfoPane()->show(InfoPaneDropdown::Clock);
                d->ignoreSuper = true;
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_F2) && (button->state == Mod4Mask)) {
                MainWin->getInfoPane()->show(InfoPaneDropdown::Battery);
                d->ignoreSuper = true;
            /*} else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_F3) && (button->state == Mod4Mask)) {
                MainWin->getInfoPane()->show(InfoPaneDropdown::Network);
                ignoreSuper = true;
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_F5) && (button->state == Mod4Mask)) {
                MainWin->getInfoPane()->show(InfoPaneDropdown::Print);
                ignoreSuper = true;*/
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_Return) && (button->state == Mod4Mask)) {
                QString newKeyboardLayout = MainWin->getInfoPane()->setNextKeyboardLayout();
                d->Hotkeys->show(QIcon::fromTheme("input-keyboard"), tr("Keyboard Layout"), tr("Keyboard Layout set to %1").arg(newKeyboardLayout), 5000);
                d->ignoreSuper = true;
            } else if (button->detail == XKeysymToKeycode(QX11Info::display(), XK_Num_Lock) || button->detail == XKeysymToKeycode(QX11Info::display(), XK_Caps_Lock)) {
                if (d->themeSettings->value("accessibility/bellOnCapsNumLock", false).toBool()) {
                    QSoundEffect* sound = new QSoundEffect();
                    sound->setSource(QUrl("qrc:/sounds/keylocks.wav"));
                    sound->play();
                    connect(sound, SIGNAL(playingChanged()), sound, SLOT(deleteLater()));
                }
            }
        }/* else if (event->response_type == XCB_MAP_WINDOW) {
            xcb_map_window_request_t* map = static_cast<xcb_map_window_request_t*>(message);

            qDebug() << "Window Mapped!" << map->window;
        } else if (event->response_type == XCB_UNMAP_WINDOW) {
            xcb_unmap_window_request_t* map = static_cast<xcb_unmap_window_request_t*>(message);
            qDebug() << "Window unmapped!" << map->window;
        }*/
    }
    return false;
}

void NativeEventFilter::handlePowerButton() {
    if (!d->isEndSessionBoxShowing && !d->powerPressed) {
        //Perform an action depending on what the user wants
        switch (d->settings.value("power/onPowerButtonPressed", 0).toInt()) {
            case 0: { //Ask what to do
                d->isEndSessionBoxShowing = true;

                EndSessionWait* endSession;
                if (d->settings.value("input/touch", false).toBool()) {
                    endSession = new EndSessionWait(EndSessionWait::slideOff);
                } else {
                    endSession = new EndSessionWait(EndSessionWait::ask);
                }
                endSession->showFullScreen();
                endSession->exec();

                d->isEndSessionBoxShowing = false;
                break;
            }
            case 1: { //Power Off
                EndSession(EndSessionWait::powerOff);
                break;
            }
            case 2: { //Reboot
                EndSession(EndSessionWait::reboot);
                break;
            }
            case 3: { //Log Out
                EndSession(EndSessionWait::logout);
                break;
            }
            case 4: { //Suspend
                EndSession(EndSessionWait::suspend);
                break;
            }
            case 5: { //Lock
                DBusEvents->LockScreen();
                break;
            }
            case 6: { //Turn off screen
                EndSession(EndSessionWait::screenOff);
                break;
            }
            case 7: { //Hibernate
                EndSession(EndSessionWait::hibernate);
                break;
            }
        }
    } else {
        d->isEndSessionBoxShowing = true;

        EndSessionWait* endSession;
        if (d->settings.value("input/touch", false).toBool()) {
            endSession = new EndSessionWait(EndSessionWait::slideOff);
        } else {
            endSession = new EndSessionWait(EndSessionWait::ask);
        }
        endSession->showFullScreen();
        endSession->exec();

        d->isEndSessionBoxShowing = false;
    }
}
