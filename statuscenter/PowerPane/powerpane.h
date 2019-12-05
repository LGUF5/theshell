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
#ifndef POWERPANE_H
#define POWERPANE_H

#include <QWidget>
#include <statuscenterpaneobject.h>

namespace Ui {
    class PowerPane;
}

struct PowerPanePrivate;
class PowerPane : public QWidget, public StatusCenterPaneObject
{
        Q_OBJECT

    public:
        explicit PowerPane(QWidget *parent = nullptr);
        ~PowerPane();

        QWidget*mainWidget();
        QString name();
        StatusPaneTypes type();
        int position();
        void message(QString name, QVariantList args);

    private slots:
        void updateShownDevices();

        void on_powerStretchSwitch_toggled(bool checked);

    private:
        Ui::PowerPane *ui;
        PowerPanePrivate* d;
};

#endif // POWERPANE_H
