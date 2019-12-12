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

#include "rundialog.h"
#include "ui_rundialog.h"

#include <tpropertyanimation.h>
#include <QScreen>

RunDialog::RunDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::RunDialog)
{
    ui->setupUi(this);

    ui->errorContainer->setFixedHeight(0);
}

RunDialog::~RunDialog()
{
    delete ui;
}

void RunDialog::setGeometry(int x, int y, int w, int h) { //Use wmctrl command because KWin has a problem with moving windows offscreen.
    QDialog::setGeometry(x, y, w, h);
    QProcess::execute("wmctrl -r " + this->windowTitle() + " -e 0," +
                      QString::number(x) + "," + QString::number(y) + "," +
                      QString::number(w) + "," + QString::number(h));
}

void RunDialog::setGeometry(QRect geometry) {
    this->setGeometry(geometry.x(), geometry.y(), geometry.width(), geometry.height());
}

void RunDialog::on_cancelButton_clicked()
{
    this->close();
}

void RunDialog::on_runButton_clicked()
{
    if (QProcess::startDetached(ui->command->text())) {
        this->close();
    } else {
        showError(tr("Couldn't run that command."));
    }
}

void RunDialog::on_command_returnPressed()
{
    ui->runButton->click();
}

void RunDialog::show() {
    Atom DesktopWindowTypeAtom;
    DesktopWindowTypeAtom = XInternAtom(QX11Info::display(), "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    int retval = XChangeProperty(QX11Info::display(), this->winId(), XInternAtom(QX11Info::display(), "_NET_WM_WINDOW_TYPE", False),
                     XA_ATOM, 32, PropModeReplace, (unsigned char*) &DesktopWindowTypeAtom, 1); //Change Window Type

    unsigned long desktop = 0xFFFFFFFF;
    retval = XChangeProperty(QX11Info::display(), this->winId(), XInternAtom(QX11Info::display(), "_NET_WM_DESKTOP", False),
                     XA_CARDINAL, 32, PropModeReplace, (unsigned char*) &desktop, 1); //Set visible on all desktops

    QDialog::show();

    QRect screenGeometry = QApplication::screens().first()->geometry();
    this->setGeometry(screenGeometry.x(), screenGeometry.y() - this->height(), screenGeometry.width(), this->sizeHint().height());

    tPropertyAnimation *anim = new tPropertyAnimation(this, "geometry");
    anim->setStartValue(this->geometry());
    anim->setEndValue(QRect(screenGeometry.x(), screenGeometry.y(), screenGeometry.width(), this->sizeHint().height()));
    anim->setDuration(100);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start();
    connect(anim, &QPropertyAnimation::finished, [=](){
        this->repaint();
    });

    XEvent event;

    event.xclient.type = ClientMessage;
    event.xclient.serial = 0;
    event.xclient.send_event = True;
    event.xclient.message_type = XInternAtom(QX11Info::display(), "_NET_ACTIVE_WINDOW", False);
    event.xclient.window = this->winId();
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;

    XSendEvent(QX11Info::display(), DefaultRootWindow(QX11Info::display()), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);

    ui->command->setFocus();
}

void RunDialog::close() {
    QRect screenGeometry = QApplication::screens().first()->geometry();

    tPropertyAnimation *anim = new tPropertyAnimation(this, "geometry");
    anim->setStartValue(this->geometry());
    anim->setEndValue(QRect(screenGeometry.x(), screenGeometry.y() - this->height(), screenGeometry.width(), this->height()));
    anim->setDuration(500);
    anim->setEasingCurve(QEasingCurve::InCubic);
    connect(anim, &QPropertyAnimation::finished, [=]() {
        QDialog::close();
        this->deleteLater();
    });
    anim->start();
}

void RunDialog::reject() {
    this->close();
}

void RunDialog::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setPen(this->palette().color(QPalette::WindowText));
    painter.drawLine(0, this->height() - 1, this->width(), this->height() - 1);
    event->accept();
}

void RunDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        if (!this->isActiveWindow()) {
            this->close();
        }
    }
}

void RunDialog::on_command_textChanged(const QString &arg1)
{
    if (arg1 != "" && theLibsGlobal::searchInPath(arg1.split(" ").first()).isEmpty()) {
        showError(tr("Can't find that command"));
    } else {
        hideError();
    }
}

void RunDialog::showError(QString error) {
    ui->errorLabel->setText(error);

    if (!errorAnimPointer.isNull()) {
        errorAnimPointer->stop();
        errorAnimPointer->deleteLater();
    }
    errorAnimPointer = new tVariantAnimation();
    errorAnimPointer->setStartValue(ui->errorContainer->height());
    errorAnimPointer->setEndValue(ui->errorContainer->sizeHint().height());
    errorAnimPointer->setDuration(500);
    errorAnimPointer->setEasingCurve(QEasingCurve::OutCubic);
    connect(errorAnimPointer.data(), &tVariantAnimation::valueChanged, ui->errorContainer, [=](QVariant value) {
        ui->errorContainer->setFixedHeight(value.toInt());
        this->setGeometry(this->x(), this->y(), this->width(), this->sizeHint().height());
    });
    connect(errorAnimPointer.data(), &tVariantAnimation::finished, errorAnimPointer.data(), &tVariantAnimation::deleteLater);
    errorAnimPointer->start();
}

void RunDialog::hideError() {
    if (!errorAnimPointer.isNull()) {
        errorAnimPointer->stop();
        errorAnimPointer->deleteLater();
    }
    errorAnimPointer = new tVariantAnimation();
    errorAnimPointer->setStartValue(ui->errorContainer->height());
    errorAnimPointer->setEndValue(0);
    errorAnimPointer->setDuration(500);
    errorAnimPointer->setEasingCurve(QEasingCurve::OutCubic);
    connect(errorAnimPointer.data(), &tVariantAnimation::valueChanged, ui->errorContainer, [=](QVariant value) {
        ui->errorContainer->setFixedHeight(value.toInt());
        this->setGeometry(this->x(), this->y(), this->width(), this->sizeHint().height());
    });
    connect(errorAnimPointer.data(), &tVariantAnimation::finished, errorAnimPointer.data(), &tVariantAnimation::deleteLater);
    errorAnimPointer->start();
}
