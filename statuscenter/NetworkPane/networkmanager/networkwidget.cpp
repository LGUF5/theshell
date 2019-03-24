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

struct NetworkWidgetPrivate {
    QSettings settings;

    QDBusInterface* nmInterface = new QDBusInterface("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", QDBusConnection::systemBus());
    bool flightMode = false;

    int wifiSwitch = -1;

    ChunkWidget* chunk;
    QLabel* snack;

    QDBusServiceWatcher* nmWatcher;
    bool nmAvailable = false;
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
    ui->flightModeIcon->setPixmap(QIcon::fromTheme("flight-mode").pixmap(QSize(16, 16) * theLibsGlobal::getDPIScaling()));

    d->chunk = new ChunkWidget();
    d->snack = new QLabel();
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
        sendMessage("register-snack", {QVariant::fromValue(d->snack)});

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
        }
    } else if (name == "switch-toggled") {
        if (args.at(0) == d->wifiSwitch) {
            d->nmInterface->setProperty("WirelessEnabled", args.at(1).toBool());
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
    QBoxLayout* layout = (QBoxLayout*) ui->devicesList->layout();
    QLayoutItem* i = layout->takeAt(0);
    while (i != nullptr) {
        if (i->widget() != nullptr) {
            i->widget()->deleteLater();
        }
        delete i;
        i = layout->takeAt(0);
    }

    if (!d->nmAvailable) return; //NetworkManager is not available; don't do anything

    bool haveWifi = false;

    QList<QDBusObjectPath> devices = d->nmInterface->property("AllDevices").value<QList<QDBusObjectPath>>();
    for (QDBusObjectPath device : devices) {
        DevicePanel* panel = new DevicePanel(device);
        layout->addWidget(panel);
        connect(panel, SIGNAL(connectToWirelessDevice(QDBusObjectPath)), this, SLOT(connectToWirelessDevice(QDBusObjectPath)));
        connect(panel, SIGNAL(getInformationAboutDevice(QDBusObjectPath)), this, SLOT(getInformationAboutDevice(QDBusObjectPath)));

        int type = panel->deviceType();
        if (type == Wifi) {
            haveWifi = true;
        }
    }

    layout->addSpacerItem(new QSpacerItem(20, 20, QSizePolicy::Minimum, QSizePolicy::Expanding));

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
        ui->SecuritySsidEdit->setText(ssid);
        ui->SecuritySsidEdit->setVisible(false);

        switch (ap.security) {
            case NoSecurity:
                ui->SecurityType->setCurrentIndex(0);
                ui->securityDescriptionLabel->setText(tr("Connect to %1?").arg(ssid));
                break;
            case Leap:
            case StaticWep:
                ui->SecurityType->setCurrentIndex(1);
                ui->securityDescriptionLabel->setText(tr("To connect to %1, you'll need to provide a key.").arg(ssid));
                break;
            case DynamicWep:
                ui->SecurityType->setCurrentIndex(2);
                ui->securityDescriptionLabel->setText(tr("To connect to %1, you'll need to provide a key.").arg(ssid));
                break;
            case WpaPsk:
            case Wpa2Psk:
                ui->SecurityType->setCurrentIndex(3);
                ui->securityDescriptionLabel->setText(tr("To connect to %1, you'll need to provide a key.").arg(ssid));
                break;
            case WpaEnterprise:
            case Wpa2Enterprise:
                ui->SecurityType->setCurrentIndex(4);
                ui->securityDescriptionLabel->setText(tr("To connect to %1, you'll need to provide authentication details.").arg(ssid));
                break;
        }
        ui->SecurityType->setVisible(false);

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

DevicePanel::DevicePanel(QDBusObjectPath device, QWidget* parent) : QWidget(parent) {
    deviceInterface = new QDBusInterface("org.freedesktop.NetworkManager", device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());
    this->device = device;
    QDBusConnection::systemBus().connect(deviceInterface->service(), device.path(), "org.freedesktop.DBus.Properties", "PropertiesChanged", this, SLOT(updateInfo()));

    QBoxLayout* infoLayout = new QBoxLayout(QBoxLayout::LeftToRight);

    QPushButton* infoButton = new QPushButton();
    infoButton->setIcon(QIcon::fromTheme("help-about"));
    infoButton->setFlat(true);
    connect(infoButton, &QPushButton::clicked, [=] {
        emit getInformationAboutDevice(device);
    });
    infoLayout->addWidget(infoButton);

    iconLabel = new QLabel();
    infoLayout->addWidget(iconLabel);

    QBoxLayout* textLayout = new QBoxLayout(QBoxLayout::TopToBottom);
    infoLayout->addLayout(textLayout);

    connectionNameLabel = new QLabel();
    connectionNameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    textLayout->addWidget(connectionNameLabel);

    connectionSubNameLabel = new QLabel();
    connectionSubNameLabel->setEnabled(false);
    textLayout->addWidget(connectionSubNameLabel);

    buttonLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    buttonLayout->setSpacing(0);
    infoLayout->addLayout(buttonLayout);

    this->setLayout(infoLayout);

    updateInfo();
}

DevicePanel::~DevicePanel() {
    deviceInterface->deleteLater();
    nmInterface->deleteLater();
}

int DevicePanel::deviceType() {
    return deviceInterface->property("DeviceType").toInt();
}

void DevicePanel::updateInfo() {
    QLayoutItem* i = buttonLayout->takeAt(0);
    while (i != nullptr) {
        if (i->widget() != nullptr) {
            i->widget()->deleteLater();
        }
        delete i;
        i = buttonLayout->takeAt(0);
    }

    NmDeviceState state = (NmDeviceState) deviceInterface->property("State").toInt();

    QIcon icon;

    switch (deviceInterface->property("DeviceType").toInt()) {
        case Ethernet: { //Ethernet
            if (state == Disconnected || state == Failed || state == Unavailable) {
                icon = QIcon::fromTheme("network-wired-unavailable");
                connectionNameLabel->setText(tr("Wired Connection"));
                connectionSubNameLabel->setText(tr("Disconnected"));

                if (state == Unavailable) {
                    QLabel* label = new QLabel();
                    label->setText(tr("To connect to this network, try plugging a cable in."));
                    buttonLayout->addWidget(label);
                } else {
                    QPushButton* networksButton = new QPushButton();
                    networksButton->setText(tr("Connect"));
                    networksButton->setIcon(QIcon::fromTheme("network-connect"));
                    connect(networksButton, &QPushButton::clicked, [=] {
                        nmInterface->call(QDBus::NoBlock, "ActivateConnection", QVariant::fromValue(QDBusObjectPath("/")), QVariant::fromValue(device), QVariant::fromValue(QDBusObjectPath("/")));
                    });
                    buttonLayout->addWidget(networksButton);
                }
            } else {
                icon = QIcon::fromTheme("network-wired-activated");
                connectionNameLabel->setText(tr("Wired Connection"));
                connectionSubNameLabel->setText(tr("Connected"));

                QPushButton* disconnectButton = new QPushButton();
                disconnectButton->setText(tr("Disconnect"));
                disconnectButton->setIcon(QIcon::fromTheme("network-disconnect"));
                disconnectButton->setProperty("type", "destructive");
                connect(disconnectButton, &QPushButton::clicked, [=] {
                    QDBusInterface deviceInterface(nmInterface->service(), device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());
                    deviceInterface.call(QDBus::NoBlock, "Disconnect");
                });
                buttonLayout->addWidget(disconnectButton);
            }
            break;
        }

        case Wifi: {
            QDBusInterface wirelessInterface(nmInterface->service(), device.path(), "org.freedesktop.NetworkManager.Device.Wireless", QDBusConnection::systemBus());
            QDBusObjectPath activeNetwork = wirelessInterface.property("ActiveAccessPoint").value<QDBusObjectPath>();

            if (state == Disconnected || state == Failed || state == Unavailable || activeNetwork.path() == "/" || activeNetwork.path() == "") {
                icon = QIcon::fromTheme("network-wireless-disconnected");
                connectionNameLabel->setText(tr("Wi-Fi"));

                if (state == Unavailable) {
                    connectionSubNameLabel->setText(tr("Disabled"));

                    QLabel* label = new QLabel();
                    label->setText(tr("To connect to a network, try switching on Wi-Fi."));
                    buttonLayout->addWidget(label);

                } else {
                    connectionSubNameLabel->setText(tr("Disconnected"));
                }
            } else {
                QDBusInterface activeNetworkInterface(nmInterface->service(), activeNetwork.path(), "org.freedesktop.NetworkManager.AccessPoint", QDBusConnection::systemBus());

                int strength = activeNetworkInterface.property("Strength").toInt();
                if (strength < 15) {
                    icon = QIcon::fromTheme("network-wireless-connected-00");
                } else if (strength < 35) {
                    icon = QIcon::fromTheme("network-wireless-connected-25");
                } else if (strength < 65) {
                    icon = QIcon::fromTheme("network-wireless-connected-50");
                } else if (strength < 85) {
                    icon = QIcon::fromTheme("network-wireless-connected-75");
                } else {
                    icon = QIcon::fromTheme("network-wireless-connected-100");
                }

                connectionNameLabel->setText(activeNetworkInterface.property("Ssid").toString());

                if (state == Activated) {
                    connectionSubNameLabel->setText(tr("Connected"));
                } else if (state == Prepare || state == Config || state == IpConfig || state == IpCheck || state == Secondaries) {
                    connectionSubNameLabel->setText(tr("Connecting"));
                } else if (state == NeedAuth) {
                    connectionSubNameLabel->setText(tr("Requires Attention"));
                } else if (state == Deactivating) {
                    connectionSubNameLabel->setText(tr("Disconnecting"));
                }

                QPushButton* disconnectButton = new QPushButton();
                disconnectButton->setText(tr("Disconnect"));
                disconnectButton->setIcon(QIcon::fromTheme("network-disconnect"));
                disconnectButton->setProperty("type", "destructive");
                connect(disconnectButton, &QPushButton::clicked, [=] {
                    QDBusInterface deviceInterface(nmInterface->service(), device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());
                    deviceInterface.call(QDBus::NoBlock, "Disconnect");
                });
                buttonLayout->addWidget(disconnectButton);
            }

            if (state != Unavailable) {
                QPushButton* networksButton = new QPushButton();
                networksButton->setText(tr("Choose Network"));
                networksButton->setIcon(QIcon::fromTheme("go-next"));
                connect(networksButton, &QPushButton::clicked, [=] {
                    emit connectToWirelessDevice(device);
                });
                buttonLayout->addWidget(networksButton);
            }
            break;
        }

        case Bluetooth: {
            QDBusInterface btInterface(nmInterface->service(), device.path(), "org.freedesktop.NetworkManager.Device.Bluetooth", QDBusConnection::systemBus());
            icon = QIcon::fromTheme("network-bluetooth");
            connectionNameLabel->setText(btInterface.property("Name").toString());


            if (state == Disconnected || state == Failed) {
                connectionSubNameLabel->setText(tr("Disconnected"));

                QPushButton* networksButton = new QPushButton();
                networksButton->setText(tr("Connect"));
                networksButton->setIcon(QIcon::fromTheme("network-connect"));
                connect(networksButton, &QPushButton::clicked, [=] {
                    nmInterface->call("ActivateConnection", QVariant::fromValue(QDBusObjectPath("/")), QVariant::fromValue(device), QVariant::fromValue(QDBusObjectPath("/")));
                });
                buttonLayout->addWidget(networksButton);
            } else if (state == Unavailable) {
                connectionSubNameLabel->setText(tr("Unavailable"));
            } else if (state == Prepare || state == Config || state == IpConfig || state == IpCheck || state == Secondaries) {
                connectionSubNameLabel->setText(tr("Connecting"));

                QPushButton* disconnectButton = new QPushButton();
                disconnectButton->setText(tr("Disconnect"));
                disconnectButton->setIcon(QIcon::fromTheme("network-disconnect"));
                disconnectButton->setProperty("type", "destructive");
                connect(disconnectButton, &QPushButton::clicked, [=] {
                    QDBusInterface deviceInterface(nmInterface->service(), device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());
                    deviceInterface.call(QDBus::NoBlock, "Disconnect");
                });
                buttonLayout->addWidget(disconnectButton);
            } else {
                connectionSubNameLabel->setText(tr("Connected"));

                QPushButton* disconnectButton = new QPushButton();
                disconnectButton->setText(tr("Disconnect"));
                disconnectButton->setIcon(QIcon::fromTheme("network-disconnect"));
                disconnectButton->setProperty("type", "destructive");
                connect(disconnectButton, &QPushButton::clicked, [=] {
                    QDBusInterface deviceInterface(nmInterface->service(), device.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());
                    deviceInterface.call(QDBus::NoBlock, "Disconnect");
                });
                buttonLayout->addWidget(disconnectButton);
            }
            break;
        }

        default:
            this->deleteLater();
    }

    iconLabel->setPixmap(icon.pixmap(32 * theLibsGlobal::getDPIScaling(), 32 * theLibsGlobal::getDPIScaling()));
}

void NetworkWidget::connectToWirelessDevice(QDBusObjectPath device) {
    ui->stackedWidget->setCurrentIndex(2);
    ui->AvailableNetworksList->setModel(new AvailableNetworksList(device));
}

void NetworkWidget::updateGlobals() {
    if (d->nmAvailable) {
        d->chunk->setVisible(true);
        d->snack->setVisible(true);

        QString text, supplementaryText;
        QIcon icon;
        QVariant primaryConnectionVariant = d->nmInterface->property("PrimaryConnection");
        QDBusObjectPath primaryConnection = primaryConnectionVariant.value<QDBusObjectPath>();
        NmDeviceType deviceType;

        if (!primaryConnectionVariant.isValid() || primaryConnection.path() == "/") {
            text = tr("Disconnected");
            icon = QIcon::fromTheme("network-wired-unavailable");
            deviceType = Generic;
        } else {
            QDBusInterface activeConnection(d->nmInterface->service(), primaryConnection.path(), "org.freedesktop.NetworkManager.Connection.Active", QDBusConnection::systemBus());
            QList<QDBusObjectPath> devices = activeConnection.property("Devices").value<QList<QDBusObjectPath>>();

            if (devices.length() != 0) {
                QDBusObjectPath firstDevice = devices.first();
                QDBusInterface deviceInterface("org.freedesktop.NetworkManager", firstDevice.path(), "org.freedesktop.NetworkManager.Device", QDBusConnection::systemBus());
                NmDeviceState state = (NmDeviceState) deviceInterface.property("State").toInt();
                deviceType = (NmDeviceType) deviceInterface.property("DeviceType").toInt();

                switch (deviceType) {
                    case Ethernet:
                        if (state == Disconnected || state == Failed || state == Unavailable) {
                            text = tr("Disconnected");
                            icon = QIcon::fromTheme("network-wired-unavailable");
                        } else {
                            text = tr("Wired");
                            icon = QIcon::fromTheme("network-wired-activated");
                        }
                        break;
                    case Wifi: {
                        QDBusInterface wirelessInterface(d->nmInterface->service(), devices.first().path(), "org.freedesktop.NetworkManager.Device.Wireless", QDBusConnection::systemBus());
                        QDBusObjectPath activeNetwork = wirelessInterface.property("ActiveAccessPoint").value<QDBusObjectPath>();

                        if (state == Disconnected || state == Failed || activeNetwork.path() == "/" || activeNetwork.path() == "") {
                            text = tr("Disconnected");
                            icon = QIcon::fromTheme("network-wireless-disconnected");
                        } else {
                            QDBusInterface activeNetworkInterface(d->nmInterface->service(), activeNetwork.path(), "org.freedesktop.NetworkManager.AccessPoint", QDBusConnection::systemBus());

                            int strength = activeNetworkInterface.property("Strength").toInt();
                            if (strength < 15) {
                                icon = QIcon::fromTheme("network-wireless-connected-00");
                            } else if (strength < 35) {
                                icon = QIcon::fromTheme("network-wireless-connected-25");
                            } else if (strength < 65) {
                                icon = QIcon::fromTheme("network-wireless-connected-50");
                            } else if (strength < 85) {
                                icon = QIcon::fromTheme("network-wireless-connected-75");
                            } else {
                                icon = QIcon::fromTheme("network-wireless-connected-100");
                            }

                            text = activeNetworkInterface.property("Ssid").toString();
                        }
                        break;
                    }
                    case Bluetooth: {
                        QDBusInterface btInterface(d->nmInterface->service(), devices.first().path(), "org.freedesktop.NetworkManager.Device.Bluetooth", QDBusConnection::systemBus());

                        if (state == Disconnected || state == Failed || state == Unavailable) {
                            text = tr("Disconnected");
                            icon = QIcon::fromTheme("network-bluetooth");
                        } else {
                            text = btInterface.property("Name").toString();
                            icon = QIcon::fromTheme("network-bluetooth");
                        }
                        break;
                    }
                }
            }

            //Check connectivity
            uint connectivity = d->nmInterface->property("Connectivity").toUInt();
            if (connectivity == 2) {
                supplementaryText = tr("Login Required", "Currently behind network Portal");
            } else if (connectivity == 3) {
                supplementaryText = tr("Can't get to the Internet", "Network Portal");
            }
        }

        if (text == tr("Disconnected") && d->flightMode) {
            icon = QIcon::fromTheme("flight-mode");
            text = tr("Flight Mode");
        }

        QString finalText;
        if (supplementaryText == "") {
            finalText = text;
        } else {
            finalText = text + " · " + supplementaryText;
        }

        //emit updateBarDisplay(finalText, icon);
        d->chunk->setIcon(icon);
        d->chunk->setText(finalText);
        d->snack->setPixmap(icon.pixmap(QSize(16, 16) * theLibsGlobal::getDPIScaling()));

        if (d->wifiSwitch != -1) {
            //Update the state of the WiFi switch
            sendMessage("set-switch", {d->wifiSwitch, d->nmInterface->property("WirelessEnabled").toBool()});
        }
    } else {
        //Make snack and chunk invisible
        d->chunk->setVisible(false);
        d->snack->setVisible(false);
    }
}

void NetworkWidget::on_SecurityConnectButton_clicked()
{
    QMap<QString, QVariantMap> settings;

    QVariantMap connection;
    connection.insert("type", "802-11-wireless");
    settings.insert("connection", connection);

    QVariantMap wireless;
    wireless.insert("ssid", ui->SecuritySsidEdit->text().toUtf8());
    wireless.insert("mode", "infrastructure");

    if (ui->SecuritySsidEdit->isVisible()) {
        wireless.insert("hidden", true);
    }

    QVariantMap security;
    switch (ui->SecurityType->currentIndex()) {
        case 0: //No security
            security.insert("key-mgmt", "none");
            break;
        case 1: //Static WEP
            security.insert("key-mgmt", "none");
            security.insert("auth-alg", "shared");
            security.insert("wep-key0", ui->securityKey->text());
            break;
        case 2: //Dynamic WEP
            security.insert("key-mgmt", "none");
            security.insert("auth-alg", "shared");
            security.insert("wep-key0", ui->securityKey->text());
            break;
        case 3: //WPA(2)-PSK
            security.insert("key-mgmt", "wpa-psk");
            security.insert("psk", ui->securityKey->text());
            break;
        case 4: { //WPA(2)-Enterprise
            QVariantMap enterpriseSettings;
            security.insert("key-mgmt", "wpa-eap");
            enterpriseSettings.insert("eap", QStringList() << "ttls");

            switch (ui->EnterpriseAuthMethod->currentIndex()) {
                case 0: //TLS
                    enterpriseSettings.insert("eap", QStringList() << "tls");
                    enterpriseSettings.insert("identity", ui->EnterpriseTLSIdentity->text());
                    enterpriseSettings.insert("client-cert", QUrl::fromLocalFile(ui->EnterpriseTLSUserCertificate->text()).toEncoded());
                    enterpriseSettings.insert("ca-cert", QUrl::fromLocalFile(ui->EnterpriseTLSCACertificate->text()).toEncoded());
                    enterpriseSettings.insert("subject-match", ui->EnterpriseTLSSubjectMatch->text());
                    enterpriseSettings.insert("altsubject-matches", ui->EnterpriseTLSAlternateSubjectMatch->text().split(","));
                    enterpriseSettings.insert("private-key", QUrl::fromLocalFile(ui->EnterpriseTLSPrivateKey->text()).toEncoded());
                    enterpriseSettings.insert("private-key-password", ui->EnterpriseTLSPrivateKeyPassword->text());
                    break;
                case 1: //LEAP
                    enterpriseSettings.insert("eap", QStringList() << "leap");
                    enterpriseSettings.insert("identity", ui->EnterpriseLEAPUsername->text());
                    enterpriseSettings.insert("password", ui->EnterpriseLEAPPassword->text());
                    break;
                case 2: { //FAST
                    enterpriseSettings.insert("eap", QStringList() << "fast");
                    enterpriseSettings.insert("anonymous-identity", ui->EnterpriseFASTAnonymousIdentity->text());
                    enterpriseSettings.insert("pac-file", ui->EnterpriseFASTPacFile->text());

                    int provisioning = 0;
                    if (ui->EnterpriseFASTPacProvisioningAnonymous->isChecked()) provisioning++;
                    if (ui->EnterpriseFASTPacProvisioningAuthenticated->isChecked()) provisioning += 2;
                    enterpriseSettings.insert("phase1-fast-provisioning", QString::number(provisioning));

                    if (ui->EnterpriseFASTPhase2Auth->currentIndex() == 0) { //GTC
                        enterpriseSettings.insert("phase2-auth", "gtc");
                    } else if (ui->EnterpriseFASTPhase2Auth->currentIndex() == 1) { //MSCHAPv2
                        enterpriseSettings.insert("phase2-auth", "mschapv2");
                    }

                    enterpriseSettings.insert("identity", ui->EnterpriseFASTUsername->text());
                    enterpriseSettings.insert("password", ui->EnterpriseFASTPassword->text());

                    break;
                }
                case 4: //PEAP
                    enterpriseSettings.insert("eap", QStringList() << "peap");

                    if (ui->EnterprisePEAPVer0->isChecked()) { //Force version 0
                        enterpriseSettings.insert("phase1-peapver", "0");
                    } else if (ui->EnterprisePEAPVer1->isChecked()) { //Force version 1
                        enterpriseSettings.insert("phase1-peapver", "1");
                    }

                    //fall through
                case 3: //TTLS
                    if (ui->EnterprisePEAPAnonymousIdentity->text() != "") {
                        enterpriseSettings.insert("anonymous-identity", ui->EnterprisePEAPAnonymousIdentity->text());
                    }

                    if (ui->EnterprisePEAPCaCertificate->text() != "") {
                        enterpriseSettings.insert("client-cert", QUrl::fromLocalFile(ui->EnterprisePEAPCaCertificate->text()).toEncoded());
                    }

                    if (ui->EnterprisePEAPPhase2Auth->currentIndex() == 0) { //MSCHAPv2
                        enterpriseSettings.insert("phase2-auth", "mschapv2");
                    } else if (ui->EnterprisePEAPPhase2Auth->currentIndex() == 1) { //MD5
                        enterpriseSettings.insert("phase2-auth", "md5");
                    } else if (ui->EnterprisePEAPPhase2Auth->currentIndex() == 2) { //GTC
                        enterpriseSettings.insert("phase2-auth", "gtc");
                    }

                    enterpriseSettings.insert("identity", ui->EnterprisePEAPUsername->text());
                    enterpriseSettings.insert("password", ui->EnterprisePEAPPassword->text());
                    break;
            }

            settings.insert("802-1x", enterpriseSettings);
            break;
        }
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
    toast->setText(tr("Connecting to %1...").arg(ui->SecuritySsidEdit->text()));
    connect(toast, SIGNAL(dismissed()), toast, SLOT(deleteLater()));
    toast->show(this->window());
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_networksManualButton_clicked()
{
    ui->SecuritySsidEdit->setVisible(true);
    ui->SecurityType->setVisible(true);
    ui->securityDescriptionLabel->setText(tr("Enter the information to connect to a new network"));
    ui->stackedWidget->setCurrentIndex(3);
}

void NetworkWidget::on_SecurityType_currentIndexChanged(int index)
{
    switch (index) {
        case 0: //No security
            ui->SecurityKeysStack->setCurrentIndex(0);
            break;
        case 1: //Static WEP
            ui->SecurityKeysStack->setCurrentIndex(1);
            break;
        case 2: //Dynamic WEP
            ui->SecurityKeysStack->setCurrentIndex(1);
            break;
        case 3: //WPA(2)-PSK
            ui->SecurityKeysStack->setCurrentIndex(1);
            break;
        case 4: //WPA(2) Enterprise
            ui->SecurityKeysStack->setCurrentIndex(2);
            break;
    }
}

void NetworkWidget::on_EnterpriseAuthMethod_currentIndexChanged(int index)
{
    if (index == 4) {
        ui->peapVersionButtons->setVisible(true);
        ui->peapVersionLabel->setVisible(true);
        ui->WpaEnterpriseAuthDetails->setCurrentIndex(3);
    } else {
        ui->peapVersionButtons->setVisible(false);
        ui->peapVersionLabel->setVisible(false);
        ui->WpaEnterpriseAuthDetails->setCurrentIndex(index);
    }
}

QString NetworkWidget::selectCertificate() {
    QFileDialog* dialog = new QFileDialog(this);
    dialog->setNameFilter("Certificates (*.der *.pem *.crt *.cer)");
    if (dialog->exec() == QFileDialog::Accepted) {
        dialog->deleteLater();
        return dialog->selectedFiles().first();
    } else {
        dialog->deleteLater();
        return "";
    }
}
void NetworkWidget::on_EnterpriseTLSUserCertificateSelect_clicked()
{
    ui->EnterpriseTLSUserCertificate->setText(selectCertificate());
}

void NetworkWidget::on_EnterpriseTLSCACertificateSelect_clicked()
{
    ui->EnterpriseTLSCACertificate->setText(selectCertificate());
}

void NetworkWidget::on_knownNetworksButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(1);
}

void NetworkWidget::on_knownNetworksBackButton_clicked()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void NetworkWidget::on_EnterprisePEAPCaCertificateSelect_clicked()
{
    ui->EnterprisePEAPCaCertificate->setText(selectCertificate());
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
    toast->setText(tr("Preparing Tethering").arg(ui->SecuritySsidEdit->text()));
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