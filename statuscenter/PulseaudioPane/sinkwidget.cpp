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
#include "sinkwidget.h"
#include "ui_sinkwidget.h"

#include <tvariantanimation.h>
#include "maps.h"

#include <QThread>
#include <tpromise.h>
#include <QMutex>
#include <Context>
#include <Sink>
#include <Port>
#include <hotkeyhud.h>
#include <quietmodedaemon.h>

struct SinkWidgetPrivate {
    PulseAudioQt::Sink* sink;
    int lastIndex = 0;
    int masterTracking = 0;

    bool movingMaster = false;
    QList<double> masterScales;

    QString sinkName;
    QMutex volumeChangeLocker;
    QString lastPort;

    bool wasMutedBeforeQuietMode = false;
};

SinkWidget::SinkWidget(PulseAudioQt::Sink* sink, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SinkWidget)
{
    ui->setupUi(this);

    d = new SinkWidgetPrivate;
    d->sink = sink;

    ui->volumeSlider->setMaximum(PulseAudioQt::normalVolume() * 1.5);
    ui->volumeSlider->setPageStep(PulseAudioQt::normalVolume() / 20);

    ui->allVolumesWidget->setFixedHeight(0);

    connect(d->sink, &PulseAudioQt::Sink::descriptionChanged, this, [=] {
        ui->deviceName->setText(sink->description().toUpper());
    });
    connect(d->sink, &PulseAudioQt::Sink::mutedChanged, this, [=] {
        ui->muteButton->setChecked(sink->isMuted());
    });
    connect(d->sink, &PulseAudioQt::Sink::defaultChanged, this, [=] {
        ui->defaultButton->setChecked(sink->isDefault());
    });
    connect(d->sink, &PulseAudioQt::Sink::channelsChanged, this, &SinkWidget::updateChannels);
    connect(d->sink, &PulseAudioQt::Sink::channelVolumesChanged, this, &SinkWidget::updateChannelVolumes);
    connect(d->sink, &PulseAudioQt::Sink::portsChanged, this, &SinkWidget::updatePorts);
    connect(d->sink, &PulseAudioQt::Sink::activePortIndexChanged, this, &SinkWidget::updatePortIndex);

    connect(QuietModeDaemon::instance(), &QuietModeDaemon::QuietModeChanged, this, [=](QuietModeDaemon::QuietMode newMode, QuietModeDaemon::QuietMode oldMode) {
        if (oldMode == QuietModeDaemon::Mute) {
            d->sink->setMuted(d->wasMutedBeforeQuietMode);
            this->setEnabled(true);
        }
        if (newMode == QuietModeDaemon::Mute) {
            d->wasMutedBeforeQuietMode = d->sink->isMuted();
            d->sink->setMuted(true);
            this->setEnabled(false);
        }
    });

    if (QuietModeDaemon::getQuietMode() == QuietModeDaemon::Mute) {
        d->wasMutedBeforeQuietMode = d->sink->isMuted();
        d->sink->setMuted(true);
        this->setEnabled(false);
    }

    ui->deviceName->setText(sink->description().toUpper());
    ui->muteButton->setChecked(sink->isMuted());
    ui->defaultButton->setChecked(sink->isDefault());

    updateChannels();
    updateChannelVolumes();
    updatePorts();
}

SinkWidget::~SinkWidget()
{
    delete d;
    delete ui;
}

void SinkWidget::updateChannels() {
    QLayoutItem* item;
    while ((item = ui->volumesLayout->takeAt(0))) {
        ui->volumesLayout->removeItem(item);
        item->widget()->deleteLater();
    }

    for (int i = 0; i < d->sink->channels().count(); i++) {
        int speakerIndex = i;
        QString text = tr("Speaker");
//        if (channelPositionToString.contains(d->sink->channels().at(i)) {
//            text = QApplication::translate("maps", channelPositionToString.value(info.channel_map.map[speakerIndex]));
//        }
        text = d->sink->channels().at(i);

        QLabel* label = new QLabel();
        label->setText(text);
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        ui->volumesLayout->addWidget(label, speakerIndex, 0);

        int channelIndex = i;
        QSlider* slider = new QSlider();
        slider->setMaximum(PulseAudioQt::normalVolume() * 1.5);
        slider->setPageStep(PulseAudioQt::normalVolume() / 20);
        slider->setOrientation(Qt::Horizontal);
        slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(slider, &QSlider::valueChanged, slider, [=](int value) {
            d->sink->setChannelVolume(channelIndex, value);
        });
        ui->volumesLayout->addWidget(slider, speakerIndex, 1);
    }

    if (ui->allVolumesWidget->height() != 0) ui->allVolumesWidget->setFixedHeight(ui->allVolumesWidget->sizeHint().height());
}

void SinkWidget::updateChannelVolumes()
{
    int maxVolumeIndex = 0;
    for (int i = 0; i < d->sink->channelVolumes().count(); i++) {
        qreal volume = d->sink->channelVolumes().at(i);
        QSlider* slider = static_cast<QSlider*>(ui->volumesLayout->itemAtPosition(i, 1)->widget());
        QSignalBlocker blocker(slider);
        slider->setValue(volume);

        if (d->sink->channelVolumes().at(maxVolumeIndex) < volume) {
            maxVolumeIndex = i;
        }
    }

    if (!d->movingMaster) {
        QSignalBlocker blocker(ui->volumeSlider);
        ui->volumeSlider->setValue(d->sink->channelVolumes().at(maxVolumeIndex));
        d->masterTracking = maxVolumeIndex;
    }
}

void SinkWidget::updatePorts()
{
    QLayoutItem* item;
    while ((item = ui->portsLayout->takeAt(0))) {
        ui->portsLayout->removeItem(item);
        item->widget()->deleteLater();
    }

    for (int i = 0; i < d->sink->ports().count(); i++) {
        PulseAudioQt::Port* port = d->sink->ports().at(i);
        int index = i;
        QPushButton* button = new QPushButton(this);

        QIcon icon;
        if (port->name().contains("speaker", Qt::CaseInsensitive)) {
            icon = QIcon::fromTheme("audio-speakers");
        } else if (port->name().contains("headphones", Qt::CaseInsensitive)) {
            icon = QIcon::fromTheme("audio-headphones");
        } else {
            icon = QIcon::fromTheme("audio-volume-high");
        }

        button->setIcon(icon);
        button->setFlat(true);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setEnabled(port->availability() != PulseAudioQt::Port::Unavailable);
        button->setToolTip(port->description());
        if (i == d->sink->activePortIndex()) button->setChecked(true);
        connect(button, &QPushButton::clicked, this, [=] {
            button->setChecked(true);
            d->sink->setActivePortIndex(index);
        });
        ui->portsLayout->addWidget(button);
    }
}

void SinkWidget::updatePortIndex()
{
    if (d->sink->isDefault()) {
        PulseAudioQt::Port* port = d->sink->ports().at(d->sink->activePortIndex());
        if (port->availability() == PulseAudioQt::Port::Unavailable) {
            //Weird thing? Use a workaround here
            QList<PulseAudioQt::Port*> availablePorts;
            for (PulseAudioQt::Port* port : d->sink->ports()) {
                if (port->availability() != PulseAudioQt::Port::Unavailable) availablePorts.append(port);
            }

            if (availablePorts.count() == 1) {
                port = availablePorts.first();
            } else {
                port = nullptr;
            }
        }

        if (port != nullptr) {
            QString newPort;
            if (port->name().contains("headphones", Qt::CaseInsensitive)) {
                newPort = "headphones";
            } else if (port->name().contains("speaker", Qt::CaseInsensitive)) {
                newPort = "speakers";
            }

            if (d->lastPort != newPort && d->lastPort != "") {
                //Show the HUD
                d->lastPort = newPort;
                updateVolumeAndShowHud(d->sink->volume());
            }
            d->lastPort = newPort;
        }
    }
    SinkWidget::updatePorts();
}

void SinkWidget::defaultSinkChanged(QString defaultSinkName) {
    ui->defaultButton->setChecked(d->sinkName == defaultSinkName);
}

void SinkWidget::updateVolumeAndShowHud(qint64 volume)
{
    d->sink->setVolume(volume);

    QIcon icon;
    QString text;

    if (d->lastPort == "speakers") {
        icon = QIcon::fromTheme("audio-volume-high");
        text = tr("Speakers");
    } else if (d->lastPort == "headphones") {
        icon = QIcon::fromTheme("audio-headphones");
        text = tr("Headphones");
    } else {
        icon = QIcon::fromTheme("audio-volume-high");
        text = tr("Volume");
    }

    HotkeyHud::show(icon, text, static_cast<int>(volume / static_cast<double>(PulseAudioQt::normalVolume()) * 100));
}

void SinkWidget::on_muteButton_toggled(bool checked)
{
    d->sink->setMuted(checked);
}

PulseAudioQt::Sink* SinkWidget::sink() {
    return d->sink;
}

QString SinkWidget::currentPort()
{
    return d->lastPort;
}

void SinkWidget::on_expandVolumesButton_clicked()
{
    tVariantAnimation* a = new tVariantAnimation();
    a->setStartValue(ui->allVolumesWidget->height());
    if (ui->allVolumesWidget->height() == 0) {
        a->setEndValue(ui->allVolumesWidget->sizeHint().height());
        ui->expandVolumesButton->setIcon(QIcon::fromTheme("go-up"));
    } else {
        a->setEndValue(0);
        ui->expandVolumesButton->setIcon(QIcon::fromTheme("go-down"));
    }
    a->setDuration(500);
    a->setEasingCurve(QEasingCurve::OutCubic);
    connect(a, &tVariantAnimation::valueChanged, a, [=](QVariant value) {
        ui->allVolumesWidget->setFixedHeight(value.toInt());
    });
    connect(a, &tVariantAnimation::finished, a, &tVariantAnimation::deleteLater);
    a->start();
}

void SinkWidget::on_volumeSlider_sliderPressed()
{
    d->movingMaster = true;

    d->masterScales.clear();
    for (int i = 0; i < d->sink->channels().count(); i++) {
        if (ui->volumeSlider->value() == 0) {
            d->masterScales.append(1);
        } else {
            QSlider* slider = static_cast<QSlider*>(ui->volumesLayout->itemAtPosition(i, 1)->widget());
            d->masterScales.append(static_cast<double>(slider->value()) / static_cast<double>(ui->volumeSlider->value()));
        }
    }
}

void SinkWidget::on_volumeSlider_sliderReleased()
{
    d->movingMaster = false;
}

void SinkWidget::on_volumeSlider_valueChanged(int value)
{
//    new tPromise<void>([=](QString error) {
//        if (!d->volumeChangeLocker.tryLock()) return;
//        for (int i = 0; i < d->sink->channels().count(); i++) {
//            int newVol = value * d->masterScales.value(i);
//            d->sink->setChannelVolume(i, newVol);
//            QThread::msleep(20);
//        }
//        d->volumeChangeLocker.unlock();
//    });
    d->sink->setVolume(value);
}

void SinkWidget::on_defaultButton_toggled(bool checked)
{
    if (checked) {
        d->sink->setDefault(true);
    }
}
