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

#include "bluetoothmanagement.h"
#include "ui_bluetoothmanagement.h"

QString calculateSize(quint64 size) {
    QString ret;
    if (size > 1073741824) {
        ret = QString::number(((float) size / 1024 / 1024 / 1024), 'f', 2).append(" GiB");
    } else if (size > 1048576) {
        ret = QString::number(((float) size / 1024 / 1024), 'f', 2).append(" MiB");
    } else if (size > 1024) {
        ret = QString::number(((float) size / 1024), 'f', 2).append(" KiB");
    } else {
        ret = QString::number((float) size, 'f', 2).append(" B");
    }

    return ret;
}

extern float getDPIScaling();

BluetoothManagement::BluetoothManagement(QWidget *parent) :
    QStackedWidget(parent),
    ui(new Ui::BluetoothManagement)
{
    ui->setupUi(this);

    this->settingAttributes.icon = QIcon::fromTheme("preferences-system-bluetooth");
    this->settingAttributes.menuWidget = ui->menuStackedWidget;

    mgr = new BluezQt::Manager;
    obexMgr = new BluezQt::ObexManager;

    agent = new BTAgent(mgr, this);
    connect(agent, SIGNAL(pairRequest(QString,BluezQt::Request<>,QString,bool)), this, SLOT(pairRequest(QString,BluezQt::Request<>,QString,bool)));
    connect(agent, SIGNAL(pairReturnRequest(BluezQt::Request<QString>,QString)), this, SLOT(pairReturnRequest(BluezQt::Request<QString>,QString)));
    connect(agent, SIGNAL(pairCancel()), this, SLOT(pairCancel()));
    ui->deviceList->setModel(new DevicesListModel(mgr));
    ui->deviceList->setItemDelegate(new DevicesDelegate(this));

    ui->pairDeviceList->setModel(new DevicesListModel(mgr, DevicesListModel::Unpaired));
    ui->pairDeviceList->setItemDelegate(new DevicesDelegate(this));
    ui->menuStackedWidget->setCurrentAnimation(tStackedWidget::SlideHorizontal);

    obexAgent = new BTObexAgent(obexMgr, mgr, this);
    connect(obexAgent, &BTObexAgent::newTransfer, [=](BluezQt::ObexTransferPtr transfer) {
        obexTransferModel->pushTransfer(transfer, true);
    });
    obexTransferModel = new TransfersListModel(obexMgr, this);
    ui->transfersList->setModel(obexTransferModel);
    ui->transfersList->setItemDelegate(new TransfersDelegate);

    ui->pairSpinner->setFixedSize(32, 32);
    ui->enterKeyIcon->setPixmap(QIcon::fromTheme("key-enter").pixmap(16, 16));

    connect(ui->deviceList->selectionModel(), SIGNAL(currentChanged(QModelIndex,QModelIndex)), this, SLOT(selectedDeviceChanged(QModelIndex,QModelIndex)));
    connect(ui->transfersList->selectionModel(), SIGNAL(currentChanged(QModelIndex,QModelIndex)), this, SLOT(selectedTransferChanged(QModelIndex,QModelIndex)));

    connect(mgr, SIGNAL(allAdaptersRemoved()), this, SLOT(updateAdapters()));
    connect(mgr, &BluezQt::Manager::deviceChanged, [=](BluezQt::DevicePtr device) {
        if (currentDevice == device) loadCurrentDevice();
    });
    connect(mgr, &BluezQt::Manager::adapterAdded, [=](BluezQt::AdapterPtr adapter) {
        sendMessage("register-switch", QVariantList() << tr("Bluetooth") << adapter->isPowered() << adapter->address());
        updateAdapters();
    });
    connect(mgr, &BluezQt::Manager::adapterRemoved, [=](BluezQt::AdapterPtr adapter) {
        uint key = adapterSwitches.key(adapter);
        sendMessage("deregister-switch", QVariantList() << key);
        adapterSwitches.remove(key);
        updateAdapters();
    });
    connect(mgr, &BluezQt::Manager::adapterChanged, [=](BluezQt::AdapterPtr adapter) {
        uint key = adapterSwitches.key(adapter);
        sendMessage("set-switch", QVariantList() << key << adapter->isPowered());
        updateAdapters();
    });
    connect(mgr, &BluezQt::Manager::operationalChanged, this, &BluetoothManagement::updateAdapters);

    ui->menuStackedWidget->setFixedWidth(250 * getDPIScaling());

    chunk = new ChunkWidget(mgr);
    connect(chunk, &ChunkWidget::showBluetoothPane, [=] {
        sendMessage("show", {});
    });
    QTimer::singleShot(0, [=] {
        sendMessage("register-chunk", {QVariant::fromValue(chunk)});

        updateAdapters();
    });
}

BluetoothManagement::~BluetoothManagement()
{
    delete ui;
}

QWidget* BluetoothManagement::mainWidget() {
    return this;
}

QString BluetoothManagement::name() {
    return tr("Bluetooth");
}

BluetoothManagement::StatusPaneTypes BluetoothManagement::type() {
    return Setting;
}

int BluetoothManagement::position() {
    return 1000;
}

void BluetoothManagement::message(QString name, QVariantList args) {
    if (name == "switch-registered") {
        BluezQt::AdapterPtr adapter = mgr->adapterForAddress(args.at(1).toString());
        if (!adapter.isNull()) {
            adapterSwitches.insert(args.first().toUInt(), adapter);
        }
    } else if (name == "switch-toggled") {
        BluezQt::AdapterPtr adapter = adapterSwitches.value(args.first().toUInt());
        if (!adapter.isNull()) {
            adapter->setPowered(args.at(1).toBool());
        }
    } else if (name == "flight-mode-changed") {
        bool flightModeOn = args.first().toBool();

        if (flightModeOn) {
            //Disable all Bluetooth adapters
            for (BluezQt::AdapterPtr adapter : mgr->adapters()) {
                adapter->setProperty("poweredBeforeFlightMode", adapter->isPowered());
                adapter->setPowered(false);
            }
        } else {
            //Enable bluetooth adapters that were powered
            for (BluezQt::AdapterPtr adapter : mgr->adapters()) {
                if (adapter->property("poweredBeforeFlightMode").toBool()) {
                    adapter->setPowered(true);
                }
            }
        }
    }
}

void BluetoothManagement::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
    }
}

void BluetoothManagement::updateAdapters() {
    if (!mgr->isOperational()) {
        this->setCurrentIndex(1); //Bluetooth not operational
        ui->menuStackedWidget->setProperty("STATUSCENTER_notavailable", true);

        if (showing) ui->mainMenuButton->click(); //Return to the main menu
    } else if (mgr->adapters().count() == 0) {
        this->setCurrentIndex(0); //No Bluetooth Hardware
        ui->menuStackedWidget->setProperty("STATUSCENTER_notavailable", true);

        if (showing) ui->mainMenuButton->click(); //Return to the main menu
    } else {
        this->setCurrentIndex(2);
        ui->menuStackedWidget->setProperty("STATUSCENTER_notavailable", false);

        if (!mgr->adapters().contains(currentAdapter)) {
            currentAdapter = mgr->adapters().first();
        }
        ui->deviceName->setText(tr("Device Name: <b>%1</b>").arg(currentAdapter.data()->name()));
        ui->addressLabel->setText(tr("Address: <b>%1</b>").arg(currentAdapter.data()->address()));

        ui->enableCheck->setChecked(currentAdapter.data()->isPowered());
        ui->visibilityCheck->setEnabled(currentAdapter.data()->isPowered());
        ui->visibilityCheck->setChecked(currentAdapter.data()->isDiscoverable());
    }
}

void BluetoothManagement::on_visibilityCheck_toggled(bool checked)
{
    currentAdapter.data()->setDiscoverable(checked);
}

void BluetoothManagement::on_enableCheck_toggled(bool checked)
{
    currentAdapter.data()->setPowered(checked);
}

void BluetoothManagement::on_changeNameButton_clicked()
{
    bool ok;
    QString oldName = currentAdapter.data()->name();
    QString newName = QInputDialog::getText(this, tr("Rename Device"), tr("What do you want to call this device?"), QLineEdit::Normal, oldName, &ok);
    if (ok) {
        currentAdapter.data()->setName(newName);
    }

}

void BluetoothManagement::selectedDeviceChanged(QModelIndex current, QModelIndex previous) {
    if (current.isValid()) {
        ui->deviceStack->setCurrentIndex(1);

        currentDevice = current.data(Qt::UserRole + 1).value<BluezQt::DevicePtr>();
        loadCurrentDevice();
    } else {
        ui->deviceStack->setCurrentIndex(0);
    }
}

void BluetoothManagement::loadCurrentDevice() {
    ui->btDeviceName->setText(currentDevice.data()->name());
    ui->deviceNameEdit->setText(currentDevice.data()->name());
    ui->deviceAddress->setText(currentDevice.data()->address());

    if (currentDevice.data()->isConnected()) {
        ui->connectButton->setText(tr("Disconnect from Device"));
        ui->connectButton->setDescription(tr("Stop communicating with this Bluetooth device"));
    } else {
        ui->connectButton->setText(tr("Connect to Device"));
        ui->connectButton->setDescription(tr("Connect to all available Bluetooth profiles on this device"));
    }

    if (currentDevice.data()->uuids().contains(BluezQt::Services::Panu)) {
        ui->connectPANButton->setVisible(true);
    } else {
        ui->connectPANButton->setVisible(false);
    }

    if (currentDevice.data()->uuids().contains(BluezQt::Services::ObexObjectPush)) {
        ui->sendFilesButton->setVisible(true);
    } else {
        ui->sendFilesButton->setVisible(false);
    }
}

void BluetoothManagement::on_mainMenuButton_clicked()
{
    sendMessage("main-menu", QVariantList());
}

void BluetoothManagement::on_backButton_clicked()
{
    currentAdapter.data()->stopDiscovery();
    selectedDeviceChanged(ui->deviceList->currentIndex(), ui->deviceList->currentIndex());
    sendMessage("show-menu", QVariantList());
}

void BluetoothManagement::on_pairButton_clicked()
{
    if (!currentAdapter.data()->isPowered()) {
        if (QMessageBox::warning(this, tr("Enable Bluetooth"), tr("Bluetooth needs to be enabled to pair a new device. Do you want to enable Bluetooth now?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::No) {
            return;
        }

        currentAdapter.data()->setPowered(true)->waitForFinished();
    }

    ui->deviceStack->setCurrentIndex(2);
    currentAdapter.data()->startDiscovery();
    sendMessage("hide-menu", QVariantList());
}

void BluetoothManagement::on_unpairDeviceButton_clicked()
{
    QString deviceName = currentDevice.data()->name();
    if (QMessageBox::question(this, tr("Unpair Device"), tr("Unpair %1 from this device?").arg(deviceName), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
        BluezQt::PendingCall* call = currentDevice.data()->adapter().data()->removeDevice(currentDevice);
        connect(call, &BluezQt::PendingCall::finished, [=] {
            tToast* t = new tToast();
            t->setTitle(tr("Unpair"));
            t->setText(tr("%1 has been unpaired from this device.").arg(deviceName));
            t->show(this->window());
            connect(t, SIGNAL(dismissed()), t, SLOT(deleteLater()));
        });
    }
}

void BluetoothManagement::on_pairDeviceList_activated(const QModelIndex &index)
{
    pairingDevice = index.data(Qt::UserRole + 1).value<BluezQt::DevicePtr>();
    ui->deviceStack->setCurrentIndex(3);
    ui->pairDeviceName->setText(pairingDevice.data()->name());
    agent->setPairingRequestDevice(pairingDevice);
    BluezQt::PendingCall* call = pairingDevice.data()->pair();
    connect(call, &BluezQt::PendingCall::finished, [=] {
        if (call->error() != BluezQt::PendingCall::NoError) {
            pairCancel();
        } else {
            agent->setPairingRequestDevice(nullptr);

            currentAdapter.data()->stopDiscovery();
            selectedDeviceChanged(ui->deviceList->currentIndex(), ui->deviceList->currentIndex());
            sendMessage("show-menu", QVariantList());

            tToast* toast = new tToast();
            toast->setTitle(tr("Pairing Complete"));
            toast->setText(tr("%1 has been paired with this device.").arg(pairingDevice.data()->name()));
            connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
            toast->show(this->window());
        }
    });
}

void BluetoothManagement::on_cancelPairingButton_clicked()
{
    pairingDevice.data()->cancelPairing();
    agent->setPairingRequestDevice(nullptr);
    ui->deviceStack->setCurrentIndex(2);
}

void BluetoothManagement::pairRequest(QString pin, BluezQt::Request<> req, QString explanation, bool showContinueButton) {
    currentRequest = req;
    ui->pairingTypeStack->setCurrentIndex(0);
    ui->deviceStack->setCurrentIndex(4);
    ui->pairingCode->setText(pin);
    ui->pairCodeInstructions->setText(explanation);
    ui->acceptCodeButton->setVisible(showContinueButton);
    ui->enterKeyLabel->setVisible(!showContinueButton);
    ui->enterKeyIcon->setVisible(!showContinueButton);
}

void BluetoothManagement::pairReturnRequest(BluezQt::Request<QString> req, QString explanation) {
    currentReturnRequest = req;
    ui->pairingTypeStack->setCurrentIndex(1);
    ui->deviceStack->setCurrentIndex(4);
    ui->pairCodeInputInstructions->setText(explanation);
}

void BluetoothManagement::on_acceptCodeInputButton_clicked()
{
    currentReturnRequest.accept(ui->pairingCodeInput->text());
    ui->pairingCodeInput->setText("");
    ui->deviceStack->setCurrentIndex(3);
}

void BluetoothManagement::on_acceptCodeButton_clicked()
{
    currentRequest.accept();
    ui->deviceStack->setCurrentIndex(3);
}

void BluetoothManagement::pairCancel() {
    agent->setPairingRequestDevice(nullptr);

    currentAdapter.data()->stopDiscovery();
    selectedDeviceChanged(ui->deviceList->currentIndex(), ui->deviceList->currentIndex());
    sendMessage("show-menu", QVariantList());

    tToast* toast = new tToast();
    toast->setTitle(tr("Pairing Failed"));
    toast->setText(tr("Couldn't pair with %1.").arg(pairingDevice.data()->name()));
    connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
    toast->show(this->window());
}

void BluetoothManagement::on_deviceNameEdit_textChanged(const QString &arg1)
{
    currentDevice.data()->setName(arg1);
}

void BluetoothManagement::on_connectButton_clicked()
{
    if (currentDevice.data()->isConnected()) {
        currentDevice.data()->disconnectFromDevice();
    } else {
        connectAllProfiles(0, currentDevice);
    }
}

void BluetoothManagement::connectAllProfiles(int profileIndex, BluezQt::DevicePtr device) {
    if (profileIndex < device.data()->uuids().count()) {
        BluezQt::PendingCall* call = device.data()->connectProfile(device.data()->uuids().at(profileIndex));
        connect(call, &BluezQt::PendingCall::finished, [=] {
            connectAllProfiles(profileIndex + 1, device);
        });
    }
}

void BluetoothManagement::on_connectPANButton_clicked()
{
    currentDevice.data()->connectProfile(BluezQt::Services::Panu);
}

void BluetoothManagement::on_backButton_2_clicked()
{
    selectedDeviceChanged(ui->deviceList->currentIndex(), ui->deviceList->currentIndex());
    ui->menuStackedWidget->setCurrentIndex(0);
}

void BluetoothManagement::on_activeFileTransfersButton_clicked()
{
    selectedTransferChanged(ui->transfersList->currentIndex(), ui->transfersList->currentIndex());
    ui->menuStackedWidget->setCurrentIndex(1);
}

void BluetoothManagement::on_sendFilesButton_clicked()
{
    QFileDialog* d = new QFileDialog();
    d->setWindowTitle("Send File over Bluetooth");
    d->setAcceptMode(QFileDialog::AcceptOpen);
    d->setFileMode(QFileDialog::ExistingFiles);
    if (d->exec() == QFileDialog::Accepted) {
        QVariantMap SessionArgs;
        SessionArgs.insert("Target", "opp");
        SessionArgs.insert("Source", currentDevice.data()->adapter().data()->address());

        BluezQt::PendingCall* SessionCreate = obexMgr->createSession(currentDevice.data()->address(), SessionArgs);
        connect(SessionCreate, &BluezQt::PendingCall::finished, [=] {
            QDBusObjectPath sessionPath = SessionCreate->value().value<QDBusObjectPath>();
            BluezQt::ObexObjectPush* push = new BluezQt::ObexObjectPush(sessionPath);
            for (QString file : d->selectedFiles()) {
                BluezQt::PendingCall* FileSend = push->sendFile(file);
                connect(FileSend, &BluezQt::PendingCall::finished, [=] {
                    if (FileSend->error() == BluezQt::PendingCall::NoError) {
                        BluezQt::ObexTransferPtr transfer = FileSend->value().value<BluezQt::ObexTransferPtr>();
                        obexTransferModel->pushTransfer(transfer, false);
                    } else {
                        tToast* toast = new tToast();
                        toast->setTitle(tr("Send Error"));
                        toast->setText(tr("Couldn't send the file to %1.").arg(currentDevice.data()->name()));
                        connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
                        toast->show(this->window());
                    }
                });
            }
            on_activeFileTransfersButton_clicked();
            d->deleteLater();
        });
    } else {
        d->deleteLater();
    }
}

void BluetoothManagement::selectedTransferChanged(QModelIndex current, QModelIndex previous) {
    if (!currentTransfer.isNull()) {
        disconnect(transferTransferredSignal);
        disconnect(transferStateChangedSignal);
    }

    if (current.isValid()) {
        ui->deviceStack->setCurrentIndex(6);

        currentTransfer = current.data(Qt::UserRole).value<BluezQt::ObexTransferPtr>();
        transferTransferredSignal = connect(currentTransfer.data(), SIGNAL(transferredChanged(quint64)), this, SLOT(loadCurrentTransfer()));
        transferStateChangedSignal = connect(currentTransfer.data(), SIGNAL(statusChanged(Status)), this, SLOT(loadCurrentTransfer()));
        loadCurrentTransfer();
    } else {
        ui->deviceStack->setCurrentIndex(5);
    }
}

void BluetoothManagement::loadCurrentTransfer() {
    ui->transferProgressBar->setMaximum(currentTransfer.data()->size());
    ui->transferProgressBar->setValue(currentTransfer.data()->transferred());
    ui->transferProgress->setText(calculateSize(currentTransfer.data()->transferred()));
    ui->transferSize->setText(calculateSize(currentTransfer.data()->size()));

    if (currentTransfer.data()->status() == BluezQt::ObexTransfer::Active || currentTransfer.data()->status() == BluezQt::ObexTransfer::Queued) {
        ui->cancelTransferButton->setVisible(true);
    } else {
        ui->cancelTransferButton->setVisible(false);
    }
}

void BluetoothManagement::on_cancelTransferButton_clicked()
{
    currentTransfer.data()->cancel();
}

