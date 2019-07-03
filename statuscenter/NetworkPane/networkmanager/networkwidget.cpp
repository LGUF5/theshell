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

#include "networkwidget.h"
#include "ui_networkwidget.h"

#include <QFileDialog>
#include <QDBusServiceWatcher>
#include <QDBusConnectionInterface>
#include "devicepanel.h"

struct NetworkWidgetPrivate {
    QSettings settings;

    QDBusInterface* nmInterface = new QDBusInterface("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", QDBusConnection::systemBus());
    bool flightMode = false;

    int wifiSwitch = -1;
    int cellularSwitch = -1;

    ChunkWidget* chunk;

    QDBusServiceWatcher* nmWatcher;
    bool nmAvailable = false;

    QMap<QString, DevicePanel*> devicesWidgets;
};

NetworkWidget::NetworkWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::NetworkWidget)
{
    ui->setupUi(this);
    d = new NetworkWidgetPrivate();

    d->nmWatcher = new QDBusServiceWatcher("org.freedesktop.NetworkManager", QDBusConnection::systemBus());
    connect(d->nmWatcher, &QDBusServiceWatcher::serviceRegistered, this, &NetworkWidget::networkManagerRunning);
    connect(d->nmWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &NetworkWidget::networkManagerGone);

    ui->flightModeWarning->setVisible(false);
    ui->flightModeIcon->setPixmap(QIcon::fromTheme("flight-mode").pixmap(SC_DPI_T(QSize(16, 16), QSize)));

    d->chunk = new ChunkWidget();
    connect(d->chunk, &ChunkWidget::showNetworkPane, [=] {
        sendMessage("show", {});
    });

    ui->knownNetworksDeleteButton->setProperty("type", "destructive");

    ui->KnownNetworksList->setModel(new SavedNetworksList());
    connect(ui->KnownNetworksList->selectionModel(), &QItemSelectionModel::currentRowChanged, [=](QModelIndex previous, QModelIndex current) {
        Q_UNUSED(previous)
        if (current.isValid()) {
            ui->knownNetworksDeleteButton->setEnabled(true);
        } else {
            ui->knownNetworksDeleteButton->setEnabled(false);
        }
    });

    ui->AvailableNetworksList->setItemDelegate(new AvailableNetworksListDelegate());
    ui->KnownNetworksList->setItemDelegate(new AvailableNetworksListDelegate());

    this->informationalAttributes.darkColor = QColor(50, 50, 100);
    this->informationalAttributes.lightColor = QColor(100, 100, 255);

    ui->stackedWidget->setCurrentAnimation(tStackedWidget::SlideHorizontal);

    if (QDBusConnection::systemBus().interface()->isServiceRegistered("org.freedesktop.NetworkManager").value()) {
        networkManagerRunning();
    } else {
        networkManagerGone();
    }

    QTimer::singleShot(0, [=] {
        sendMessage("register-chunk", {QVariant::fromValue(d->chunk)});
        sendMessage("register-snack", {QVariant::fromValue(d->chunk->snackWidget())});

        updateDevices();
        updateGlobals();
    });
}

NetworkWidget::~NetworkWidget()
{
    delete ui;
    delete d;
}

QWidget* NetworkWidget::mainWidget() {
    return this;
}

QString NetworkWidget::name() {
    return tr("Network");
}

StatusCenterPaneObject::StatusPaneTypes NetworkWidget::type() {
    return Informational;
}

int NetworkWidget::position() {
    return 3;
}

void NetworkWidget::message(QString name, QVariantList args) {
    if (name == "flight-mode-changed") {
        bool flightModeOn = args.first().toBool();

        if (flightModeOn) {
            d->settings.setValue("flightmode/wifi", d->nmInterface->property("WirelessEnabled"));

            //Disable WiFi, WiMAX and mobile networking
            d->nmInterface->setProperty("WirelessEnabled", false);
            d->nmInterface->setProperty("WwanEnabled", false);
            d->nmInterface->setProperty("WimaxEnabled", false);
        } else {
            bool wifiEnabled = d->settings.value("flightmode/wifi").toBool();


            //Enable WiFi, WiMAX and mobile networking
            d->nmInterface->setProperty("WirelessEnabled", wifiEnabled);
            d->nmInterface->setProperty("WwanEnabled", true);
            d->nmInterface->setProperty("WimaxEnabled", true);
        }

        flightModeChanged(flightModeOn);
    } else if (name == "switch-registered") {
        if (args.at(1) == "wireless") {
            d->wifiSwitch = args.at(0).toUInt();
        } else if (args.at(1) == "cellular") {
            d->cellularSwitch = args.at(0).toUInt();
        }
    } else if (name == "switch-toggled") {
        if (args.at(0) == d->wifiSwitch) {
            d->nmInterface->setProperty("WirelessEnabled", args.at(1).toBool());
        } else if (args.at(0) == d->cellularSwitch) {
            d->nmInterface->setProperty("WwanEnabled", args.at(1).toBool());
        }
    }
}

void NetworkWidget::networkManagerGone() {
    d->nmAvailable = false;
    ui->mainStack->setCurrentIndex(0);
}

void NetworkWidget::networkManagerRunning() {
    d->nmAvailable = true;
    ui->mainStack->setCurrentIndex(1);

    QDBusConnection::systemBus().connect(d->nmInterface->service(), d->nmInterface->path(), d->nmInterface->interface(), "DeviceAdded", this, SLOT(updateDevices()));
    QDBusConnection::systemBus().connect(d->nmInterface->service(), d->nmInterface->path(), d->nmInterface->interface(), "DeviceRemoved", this, SLOT(updateDevices()));
    QDBusConnection::systemBus().connect(d->nmInterface->service(), d->nmInterface->path(), "org.freedesktop.DBus.Properties", "PropertiesChanged", this, SLOT(updateGlobals()));

    QTimer::singleShot(1000, [=] {
        updateDevices();
        updateGlobals();
    });
}

void NetworkWidget::flightModeChanged(bool flight) {
    d->flightMode = flight;
    ui->flightModeWarning->setVisible(flight);
    updateGlobals();
}

void NetworkWidget::updateDevices() {
    QBoxLayout* layout = ui->devicesLayout;

    if (!d->nmAvailable) return; //NetworkManager is not available; don't do anything

    bool haveWifi = false;
    bool haveCellular = false;

    //Remove stale devices
    QList<QDBusObjectPath> devices = d->nmInterface->property("AllDevices").value<QList<QDBusObjectPath>>();
    QStringList devicesToRemove;
    for (QString device : d->devicesWidgets.keys()) {
        QDBusObjectPath path(device);
        if (!devices.contains(path)) {
            devicesToRemove.append(device);
        }
    }

    for (QString device : devicesToRemove) {
        DevicePanel* panel = d->devicesWidgets.value(device);
        layout->removeWidget(panel);
        panel->deleteLater();
    }

    //Add new devices
    for (QDBusObjectPath device : devices) {
        DevicePanel* panel;
        if (d->devicesWidgets.contains(device.path())) {
            panel = d->devicesWidgets.value(device.path());
        } else {
            panel = new DevicePanel(device, this);
            layout->addWidget(panel);
            connect(panel, SIGNAL(connectToWirelessDevice(QDBusObjectPath)), this, SLOT(connectToWirelessDevice(QDBusObjectPath)));
            connect(panel, SIGNAL(getInformationAboutDevice(QDBusObjectPath)), this, SLOT(getInformationAboutDevice(QDBusObjectPath)));
            connect(panel, &DevicePanel::destroyed, this, [=] {
                d->devicesWidgets.remove(device.path());
            });
            d->devicesWidgets.insert(device.path(), panel);
        }

        DevicePanel::DevicePanelType type = panel->deviceType();
        if (type == DevicePanel::Wifi) {
            haveWifi = true;
        } else if (type == DevicePanel::Cellular) {
            haveCellular = true;
        }
    }

    QTimer::singleShot(0, [=] {
        //In case this is run during the constructor
        if (haveWifi && d->wifiSwitch == -1) {
            //Register the WiFi switch
            sendMessage("register-switch", {tr("Wi-Fi"), d->nmInterface->property("WirelessEnabled").toBool(), "wireless"});
        } else if (!haveWifi && d->wifiSwitch != -1) {
            //Deregister the WiFi switch
            sendMessage("deregister-switch", {d->wifiSwitch});
            d->wifiSwitch = -1;
        }

        if (haveCellular && d->cellularSwitch == -1) {
            //Register the WiFi switch
            sendMessage("register-switch", {tr("Cellular"), d->nmInterface->property("WwanEnabled").toBool(), "cellular"});
        } else if (!haveCellular && d->cellularSwitch != -1) {
            //Deregister the WiFi switch
            sendMessage("deregister-switch", {d->cellularSwitch});
            d->cellularSwitch = -1;
        }
    });
}

void NetworkWidget::on_networksBackButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_SecurityBackButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(2);
}

void NetworkWidget::on_AvailableNetworksList_clicked(const QModelIndex &index)
{
    //Determine if we need secrets for this network
    QString ssid = index.data(Qt::DisplayRole).toString();
    AvailableNetworksList::AccessPoint ap = index.data(Qt::UserRole).value<AvailableNetworksList::AccessPoint>();

    QDBusInterface settings(d->nmInterface->service(), "/org/freedesktop/NetworkManager/Settings", "org.freedesktop.NetworkManager.Settings", QDBusConnection::systemBus());
    QList<QDBusObjectPath> connectionSettings = settings.property("Connections").value<QList<QDBusObjectPath>>();
    QList<QDBusObjectPath> availableSettings;

    for (QDBusObjectPath settingsPath : connectionSettings) {
        //QDBusInterface settingsInterface(nmInterface->service(), settingsPath.path(), "org.freedesktop.NetworkManager.Settings.Connection");
        QDBusMessage msg = QDBusMessage::createMethodCall(d->nmInterface->service(), settingsPath.path(), "org.freedesktop.NetworkManager.Settings.Connection", "GetSettings");
        QDBusMessage msgReply = QDBusConnection::systemBus().call(msg);

        if (msgReply.arguments().count() != 0) {
            QMap<QString, QVariantMap> settings;

            QDBusArgument arg1 = msgReply.arguments().first().value<QDBusArgument>();
            arg1 >> settings;

            for (QString key : settings.keys()) {
                if (key == "802-11-wireless") {
                    QVariantMap wireless = settings.value("802-11-wireless");
                    if (wireless.value("ssid") == ssid) {
                        availableSettings.append(settingsPath);
                    }
                }
            }
        }
    }

    //Try to connect using all matching settings
    if (availableSettings.count() == 0) {
        ui->securityWidget->addNewNetwork(ssid, ap.security);
        ui->stackedWidget->setCurrentIndex(3);
    } else {
        tToast* toast = new tToast();
        toast->setTitle(tr("Wi-Fi"));
        toast->setText(tr("Connecting to %1...").arg(ssid));
        connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
        toast->show(this->window());
        ui->stackedWidget->setCurrentIndex(0);

        bool success = false;

        for (QDBusObjectPath settingsPath : availableSettings) {
            //Connect to the network
            QDBusPendingCall pending = d->nmInterface->asyncCall("ActivateConnection", QVariant::fromValue(settingsPath), QVariant::fromValue(((AvailableNetworksList*) index.model())->devicePath()), QVariant::fromValue(ap.path));

            QEventLoop* loop = new QEventLoop();

            QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(pending);
            connect(watcher, &QDBusPendingCallWatcher::finished, [=] {
                if (pending.isError()) {
                    /*tToast* toast = new tToast();
                    toast->setTitle(tr("Connection Error"));
                    toast->setText(pending.error().message());
                    connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
                    toast->show(this->parentWidget());*/
                    loop->exit(1);
                } else {
                    loop->exit(0);
                }
                watcher->deleteLater();
            });

            if (loop->exec() == 0) {
                loop->deleteLater();
                success = true;
                break;
            }
            loop->deleteLater();
        }

        if (!success) {

        }
    }
}

QList<QTreeWidgetItem*> getInfoChildren(QVariantMap parent) {
    QList<QTreeWidgetItem*> items;
    for (QString key : parent.keys()) {
        QVariant val = parent.value(key);
        QTreeWidgetItem* item = new QTreeWidgetItem();

        if (val.type() == QVariant::String) {
            item->setText(0, key);
            item->setText(1, val.toString());
        } else {
            item->setText(0, key);
            item->addChildren(getInfoChildren(val.toMap()));
        }
        items.append(item);
    }
    return items;
}

void NetworkWidget::getInformationAboutDevice(QDBusObjectPath device) {
    ui->stackedWidget->setCurrentIndex(4);

    QDBusInterface deviceInterface("org.freedesktop.NetworkManager", device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());

    QVariantMap data;
    data.insert("Interface", deviceInterface.property("Interface"));
    data.insert("MTU Value", deviceInterface.property("Mtu").toString());

    QDBusObjectPath ipv4Path = deviceInterface.property("Ip4Config").value<QDBusObjectPath>();
    if (ipv4Path.path() != "/") {
        QVariantMap ip4Conf;
        QDBusInterface ip4(deviceInterface.service(), ipv4Path.path(), "org.freedesktop.NetworkManager.IP4Config", QDBusConnection::systemBus());

        QDBusMessage addressesMessage = QDBusMessage::createMethodCall(deviceInterface.service(), ipv4Path.path(), "org.freedesktop.DBus.Properties", "Get");
        addressesMessage.setArguments(QList<QVariant>() << ip4.interface() << "AddressData");
        QDBusMessage addressReplyMessage = QDBusConnection::systemBus().call(addressesMessage);

        QDBusArgument addressArg = addressReplyMessage.arguments().first().value<QDBusVariant>().variant().value<QDBusArgument>();
        QList<QVariantMap> addresses;

        addressArg >> addresses;

        QVariantMap addressMap;
        for (QVariantMap addressData : addresses) {
            addressMap.insert(addressData.value("address").toString(), "");
        }
        ip4Conf.insert("Addresses", addressMap);
        ip4Conf.insert("Gateway", ip4.property("Gateway"));

        data.insert("IPv4", ip4Conf);
    }

    QDBusObjectPath ipv6Path = deviceInterface.property("Ip6Config").value<QDBusObjectPath>();
    if (ipv6Path.path() != "/") {
        QVariantMap ip6Conf;
        QDBusInterface ip6(deviceInterface.service(), ipv6Path.path(), "org.freedesktop.NetworkManager.IP6Config", QDBusConnection::systemBus());

        QDBusMessage addressesMessage = QDBusMessage::createMethodCall(deviceInterface.service(), ipv6Path.path(), "org.freedesktop.DBus.Properties", "Get");
        addressesMessage.setArguments(QList<QVariant>() << ip6.interface() << "AddressData");
        QDBusMessage addressReplyMessage = QDBusConnection::systemBus().call(addressesMessage);

        QDBusArgument addressArg = addressReplyMessage.arguments().first().value<QDBusVariant>().variant().value<QDBusArgument>();
        QList<QVariantMap> addresses;

        addressArg >> addresses;

        QVariantMap addressMap;
        for (QVariantMap addressData : addresses) {
            addressMap.insert(addressData.value("address").toString(), "");
        }
        ip6Conf.insert("Addresses", addressMap);
        ip6Conf.insert("Gateway", ip6.property("Gateway"));

        data.insert("IPv6", ip6Conf);
    }

    switch (deviceInterface.property("DeviceType").toInt()) {
        case Ethernet: {
            QDBusInterface wiredInterface("org.freedesktop.NetworkManager", device.path(), "org.freedesktop.NetworkManager.Device.Wired", QDBusConnection::systemBus());
            data.insert("MAC Address", wiredInterface.property("HwAddress"));
            break;
        }
        case Wifi: {
            QDBusInterface wifiInterface("org.freedesktop.NetworkManager", device.path(), "org.freedesktop.NetworkManager.Device.Wireless", QDBusConnection::systemBus());
            data.insert("MAC Address", wifiInterface.property("HwAddress"));

            QDBusObjectPath ActiveApPath = wifiInterface.property("ActiveAccessPoint").value<QDBusObjectPath>();
            if (ActiveApPath.path() != "/") {
                QDBusInterface activeApInterface("org.freedesktop.NetworkManager", ActiveApPath.path(), "org.freedesktop.NetworkManager.AccessPoint", QDBusConnection::systemBus());
                data.insert("Frequency", QString::number(activeApInterface.property("Frequency").toFloat() / 1000).append(" GHz"));
                data.insert("Remote MAC Address", activeApInterface.property("HwAddress"));
                data.insert("Signal Strength", QString::number(activeApInterface.property("Strength").toInt()) + "%");
                data.insert("SSID", activeApInterface.property("Ssid").toString());
            }
            break;
        }

    }

    ui->InformationTable->clear();
    ui->InformationTable->addTopLevelItems(getInfoChildren(data));
}

void NetworkWidget::connectToWirelessDevice(QDBusObjectPath device) {
    ui->stackedWidget->setCurrentIndex(2);
    ui->AvailableNetworksList->setModel(new AvailableNetworksList(device));
}

void NetworkWidget::updateGlobals() {
    if (d->nmAvailable) {
        d->chunk->setVisible(true);

        QString supplementaryText;
        QVariant primaryConnectionVariant = d->nmInterface->property("PrimaryConnection");
        QDBusObjectPath primaryConnection = primaryConnectionVariant.value<QDBusObjectPath>();

        if (!primaryConnectionVariant.isValid() || primaryConnection.path() == "/") {
            d->chunk->endWatch();
            if (d->flightMode) {
                d->chunk->setIcon(QIcon::fromTheme("flight-mode"), true);
                d->chunk->setText(tr("Flight Mode"));
            } else {
                d->chunk->setText(tr("Disconnected"));
                d->chunk->setIcon(QIcon::fromTheme("network-wired-unavailable"));
            }
        } else {
            //Check connectivity
            uint connectivity = d->nmInterface->property("Connectivity").toUInt();
            if (connectivity == 2) {
                supplementaryText = tr("Login Required", "Currently behind network Portal");
            } else if (connectivity == 3) {
                supplementaryText = tr("Can't get to the Internet", "Network Portal");
            }

            QDBusInterface activeConnection(d->nmInterface->service(), primaryConnection.path(), "org.freedesktop.NetworkManager.Connection.Active", QDBusConnection::systemBus());
            QList<QDBusObjectPath> devices = activeConnection.property("Devices").value<QList<QDBusObjectPath>>();

            if (devices.length() != 0) {
                QDBusObjectPath firstDevice = devices.first();
                DevicePanel* panel = d->devicesWidgets.value(firstDevice.path());
                d->chunk->watch(panel);
            }
        }
        d->chunk->setSupplementaryText(supplementaryText);

        if (d->wifiSwitch != -1) {
            //Update the state of the WiFi switch
            sendMessage("set-switch", {d->wifiSwitch, d->nmInterface->property("WirelessEnabled").toBool()});
        }
        if (d->cellularSwitch != -1) {
            //Update the state of the Cellular switch
            sendMessage("set-switch", {d->cellularSwitch, d->nmInterface->property("WwanEnabled").toBool()});
        }
    } else {
        //Make chunk invisible
        d->chunk->setVisible(false);
    }
}

void NetworkWidget::on_SecurityConnectButton_clicked()
{
    QMap<QString, QVariantMap> settings;

    QVariantMap connection;
    connection.insert("type", "802-11-wireless");
    settings.insert("connection", connection);

    QVariantMap wireless = ui->securityWidget->getNetwork();
    QVariantMap security = ui->securityWidget->getSecurity();
    if (security.value("key-mgmt") == "wpa-eap") {
        settings.insert("802-1x", ui->securityWidget->getEap());
    }

    settings.insert("802-11-wireless", wireless);
    settings.insert("802-11-wireless-security", security);

    QDBusPendingCall pendingCall = d->nmInterface->asyncCall("AddAndActivateConnection", QVariant::fromValue(settings), QVariant::fromValue(((AvailableNetworksList*) ui->AvailableNetworksList->model())->devicePath()), QVariant::fromValue(QDBusObjectPath("/")));

    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(pendingCall);
    connect(watcher, &QDBusPendingCallWatcher::finished, [=] {
        watcher->deleteLater();
        if (pendingCall.isError()) {
            tToast* toast = new tToast();
            toast->setTitle(tr("Connection Error"));
            toast->setText(pendingCall.error().message());
            toast->setTimeout(10000);
            connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
            toast->show(this->window());
        }
    });

    tToast* toast = new tToast();
    toast->setTitle(tr("Wi-Fi"));
    toast->setText(tr("Connecting to %1...").arg(wireless.value("ssid").toString()));
    connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
    toast->show(this->window());
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_networksManualButton_clicked()
{
    ui->securityWidget->addNewNetwork();
    ui->stackedWidget->setCurrentIndex(3);
}

void NetworkWidget::on_knownNetworksButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(1);
}

void NetworkWidget::on_knownNetworksBackButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_knownNetworksDeleteButton_clicked()
{
    class Setting s = ui->KnownNetworksList->model()->data(ui->KnownNetworksList->selectionModel()->selectedIndexes().first(), Qt::UserRole).value<class Setting>();
    s.del();
}

void NetworkWidget::on_tetheringEnableTetheringButton_clicked()
{

    QMap<QString, QVariantMap> settings;

    QVariantMap connection;
    connection.insert("id", "Tethering");
    connection.insert("type", "802-11-wireless");
    settings.insert("connection", connection);

    QVariantMap wireless;
    wireless.insert("ssid", ui->tetheringSSID->text().toUtf8());
    wireless.insert("mode", "ap");
    wireless.insert("security", "802-11-wireless-security");

    QVariantMap security;
    switch (ui->tetheringSecurity->currentIndex()) {
        case 0: //No security
            security.insert("key-mgmt", "none");
            break;
        case 1: //WPA2-PSK
            security.insert("key-mgmt", "wpa-psk");
            security.insert("psk", ui->tetheringKey->text());
            break;
    }

    QVariantMap ipv4;
    ipv4.insert("method", "shared");

    QVariantMap ipv6;
    ipv6.insert("method", "auto");

    settings.insert("ipv4", ipv4);
    settings.insert("ipv6", ipv6);
    settings.insert("802-11-wireless", wireless);
    settings.insert("802-11-wireless-security", security);

    QDBusObjectPath devPath("/");
    QList<QDBusObjectPath> devices = d->nmInterface->property("AllDevices").value<QList<QDBusObjectPath>>();
    if (devices.length() != 0) {
        for (QDBusObjectPath device : devices) {
            QDBusInterface deviceInterface("org.freedesktop.NetworkManager", device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());

            if (deviceInterface.property("DeviceType").toInt() == Wifi) {
                devPath = device;
            }
        }
    }


    QDBusPendingCall pendingCall = d->nmInterface->asyncCall("AddAndActivateConnection", QVariant::fromValue(settings), QVariant::fromValue(devPath), QVariant::fromValue(QDBusObjectPath("/")));

    QDBusPendingCallWatcher* watcher = new QDBusPendingCallWatcher(pendingCall);
    connect(watcher, &QDBusPendingCallWatcher::finished, [=] {
        watcher->deleteLater();
        if (pendingCall.isError()) {
            tToast* toast = new tToast();
            toast->setTitle(tr("Tethering Error"));
            toast->setText(pendingCall.error().message());
            toast->setTimeout(10000);
            connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
            toast->show(this->window());
        }
    });

    tToast* toast = new tToast();
    toast->setTitle(tr("Tethering"));
    //toast->setText(tr("Preparing Tethering").arg(ui->SecuritySsidEdit->text()));
    connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
    toast->show(this->window());
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_tetheringBackButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_tetheringButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(5);
}

void NetworkWidget::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
        updateDevices();
    }
    QWidget::changeEvent(event);
}
