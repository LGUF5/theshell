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
#ifndef LOCALEPANE_H
#define LOCALEPANE_H

#include <QWidget>
#include <statuscenterpaneobject.h>

namespace Ui {
    class LocalePane;
}

struct LocalePanePrivate;
class LocalePane : public QWidget, public StatusCenterPaneObject
{
        Q_OBJECT

    public:
        explicit LocalePane(QWidget *parent = nullptr);
        ~LocalePane();

        QWidget*mainWidget();
        QString name();
        StatusPaneTypes type();
        int position();
        void message(QString name, QVariantList args);

    private slots:
        void on_activeLanguagesList_activated(const QModelIndex &index);

        void on_activeLanguagesList_customContextMenuRequested(const QPoint &pos);

    private:
        Ui::LocalePane *ui;
        LocalePanePrivate* d;

        void changeEvent(QEvent* event);
};

#endif // LOCALEPANE_H
