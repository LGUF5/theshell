/****************************************
 *
 *   INSERT-PROJECT-NAME-HERE - INSERT-GENERIC-NAME-HERE
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
#ifndef GLOBALKEYBOARDENGINE_H
#define GLOBALKEYBOARDENGINE_H

#include <QObject>
#include <QAbstractNativeEventFilter>
#include "debuginformationcollector.h"

struct GlobalKeyboardKeyPrivate;
class GlobalKeyboardKey : public QObject
{
        Q_OBJECT
    public:
        explicit GlobalKeyboardKey(QKeySequence key, QString section = "", QString name = "", QString description = "", QObject* parent = nullptr);
        ~GlobalKeyboardKey();

        int chordCount();
        unsigned long nativeKey(uint chordNumber);
        unsigned long nativeModifiers(uint chordNumber);
        QString section();
        QString name();
        QString description();
        QKeySequence key();

        void deregister();

        void grabKey();
        void ungrabKey();

    signals:
        void shortcutActivated();
        void deregistered();

    private:
        GlobalKeyboardKeyPrivate* d;
};

struct GlobalKeyboardEnginePrivate;
class GlobalKeyboardEngine : public QObject, public QAbstractNativeEventFilter
{
        Q_OBJECT
    public:
        static GlobalKeyboardKey* registerKey(QKeySequence keySequence, QString name, QString section, QString humanReadableName, QString description);

        static GlobalKeyboardEngine* instance();

        enum KnownKeyNames {
            BrightnessUp,
            BrightnessDown,
            VolumeUp,
            VolumeDown,
            QuietModeToggle,
            TakeScreenshot,
            CaptureScreenVideo,
            LockScreen,
            Run,
            Suspend,
            PowerOff,
            NextKeyboardLayout,
            KeyboardBrightnessUp,
            KeyboardBrightnessDown,
            OpenGateway,
            PowerOptions,
            Eject
        };
        static QString keyName(KnownKeyNames name);

        static void pauseListening();
        static void startListening();

        static QPixmap getKeyShortcutImage(QKeySequence keySequence, QFont font, QPalette pal);
        static QPixmap getKeyIcon(QString key, QFont font, QPalette pal);

    signals:
        void keyShortcutRegistered(QString name, GlobalKeyboardKey* shortcut);

    public slots:

    private:
        explicit GlobalKeyboardEngine(QObject *parent = T_QOBJECT_ROOT);
        static GlobalKeyboardEnginePrivate* d;

        bool nativeEventFilter(const QByteArray &eventType, void *message, long *result);
};


#endif // GLOBALKEYBOARDENGINE_H
