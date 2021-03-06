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
#ifndef SOUNDENGINE_H
#define SOUNDENGINE_H

#include <QObject>
#include <QUrl>
#include "debuginformationcollector.h"

struct SoundEnginePrivate;
class SoundEngine : public QObject
{
        Q_OBJECT
    public:
        enum KnownSound {
            Notification,
            Volume,
            Login,
            Logout,
            Screenshot
        };

    signals:
        void done();

    public slots:
        static SoundEngine* play(QString soundName, qreal volume = 1);
        static SoundEngine* play(QUrl path, qreal volume = 1);
        static SoundEngine* play(KnownSound sound, qreal volume = 1);

    private:
        SoundEnginePrivate* d;

        explicit SoundEngine(QObject *parent = T_QOBJECT_ROOT);
        static SoundEngine* playKnownSound(QString soundName, QString soundSetting, qreal volume = 1);
};

#endif // SOUNDENGINE_H
