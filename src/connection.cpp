#include "connection.h"
#include "mainwindow.h"
#include "settings.h"
#include "ui_connection.h"
#include "firsttimewizard.h"
#include "ui_createzcashconfdialog.h"
#include "controller.h"


#include "../res/libzecwalletlite/piratewalletlitelib.h"

#include "precompiled.h"

using json = nlohmann::json;

ConnectionLoader::ConnectionLoader(MainWindow* main, Controller* rpc) {
    this->main = main;
    this->rpc  = rpc;

    d = new QDialog(main);
    connD = new Ui_ConnectionDialog();
    connD->setupUi(d);
    QPixmap logo(":img/res/logo.png");
    connD->topIcon->setPixmap(logo.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    isSyncing = new QAtomicInteger<bool>();
}

ConnectionLoader::~ConnectionLoader() {
    delete isSyncing;
    delete connD;
    delete d;
}

void ConnectionLoader::loadConnection() {
    QTimer::singleShot(1000, [=]() { this->doAutoConnect(); });
    if (!Settings::getInstance()->isHeadless())
        d->exec();
}

void ConnectionLoader::doAutoConnect() {
    qDebug() << "Doing autoconnect";

    auto config = std::shared_ptr<ConnectionConfig>(new ConnectionConfig());
    config->dangerous = false;
    config->server = Settings::getInstance()->getSettings().server;

    qDebug() << "Connecting to Server";
    if (!litelib_check_server(config->server.toStdString().c_str())) {
        config->server = Settings::getInstance()->setDefaultServer().server;
    } else if (config->server.contains("cryptoforge", Qt::CaseInsensitive)) {
        config->server = Settings::getInstance()->setDefaultServer().server;
    }
    qDebug() << "Server Connected";

    // Initialize the library
    main->logger->write(QObject::tr("Attempting to initialize library with ") + config->server);

    // Check to see if there's an existing wallet
    if (litelib_wallet_exists()) {
        main->logger->write(QObject::tr("Using existing wallet."));
        char* resp = litelib_initialize_existing(config->server.toStdString().c_str());
        QString response = litelib_process_response(resp);

        if (response.toUpper().trimmed() != "OK") {
            showError(response);
            return;
        }
    } else {
        main->logger->write(QObject::tr("Create/restore wallet."));
        createOrRestore(config->dangerous, config->server);
        d->show();
    }

    auto connection = makeConnection(config);
    rpc->setConnection(connection);
    d->accept();
    QTimer::singleShot(1, [=]() { delete this; });

    return;

}

void ConnectionLoader::createOrRestore(bool dangerous, QString server) {
    // Close the startup dialog, since we'll be showing the wizard
    d->hide();

    // Create a wizard
    FirstTimeWizard wizard(dangerous, server);

    wizard.exec();
}

void ConnectionLoader::doRPCSetConnection(Connection* conn) {
    qDebug() << "Connectionloader finished, setting connection";
    rpc->setConnection(conn);

    d->accept();

    QTimer::singleShot(1, [=]() { delete this; });
}

Connection* ConnectionLoader::makeConnection(std::shared_ptr<ConnectionConfig> config) {
    return new Connection(main, config);
}

// Update the UI with the status
void ConnectionLoader::showInformation(QString info, QString detail) {
    connD->status->setText(info);
    connD->statusDetail->setText(detail);
}

/**
 * Show error will close the loading dialog and show an error.
*/
void ConnectionLoader::showError(QString explanation) {
    rpc->noConnection();

    QMessageBox::critical(main, QObject::tr("Connection Error"), explanation, QMessageBox::Ok);
    d->close();
}

QString litelib_process_response(char* resp) {
    char* resp_copy = new char[strlen(resp) + 1];
    strcpy(resp_copy, resp);
    litelib_rust_free_string(resp);

    QString reply = QString::fromStdString(resp_copy);
    memset(resp_copy, '-', strlen(resp_copy));
    delete[] resp_copy;

    return reply;
}

/***********************************************************************************
 *  Connection, Executor and Callback Class
 ************************************************************************************/
void Executor::run() {
    char* resp = litelib_execute(this->cmd.toStdString().c_str(), this->args.toStdString().c_str());

    QString reply = litelib_process_response(resp);

    //qDebug() << "RPC Reply=" << reply;
    auto parsed = json::parse(reply.toStdString().c_str(), nullptr, false);
    if (parsed.is_discarded() || parsed.is_null()) {
        emit handleError(reply);
    } else {
        emit responseReady(parsed);
    }
}


void Callback::processRPCCallback(json resp) {
    this->cb(resp);

    // Destroy self
    delete this;
}

void Callback::processError(QString resp) {
    this->errCb(resp);

    // Destroy self
    delete this;
}

Connection::Connection(MainWindow* m, std::shared_ptr<ConnectionConfig> conf) {
    this->config      = conf;
    this->main        = m;

    // Register the JSON type as a type that can be passed between signals and slots.
    qRegisterMetaType<json>("json");
}

void Connection::doRPC(const QString cmd, const QString args, const std::function<void(json)>& cb,
                       const std::function<void(QString)>& errCb) {
    if (shutdownInProgress) {
        // Ignoring RPC because shutdown in progress
        return;
    }

    //qDebug() << "Doing RPC: " << cmd;

    // Create a runner.
    auto runner = new Executor(cmd, args);

    // Callback object. Will delete itself
    auto c = new Callback(cb, errCb);

    QObject::connect(runner, &Executor::responseReady, c, &Callback::processRPCCallback);
    QObject::connect(runner, &Executor::handleError, c, &Callback::processError);

    QThreadPool::globalInstance()->start(runner);
}

void Connection::doRPCWithDefaultErrorHandling(const QString cmd, const QString args, const std::function<void(json)>& cb) {
    doRPC(cmd, args, cb, [=] (QString err) {
        this->showTxError(err);
    });
}

void Connection::doRPCIgnoreError(const QString cmd, const QString args, const std::function<void(json)>& cb) {
    doRPC(cmd, args, cb, [=] (auto) {
        // Ignored error handling
    });
}

void Connection::showTxError(const QString& error) {
    if (error.isNull()) return;

    // Prevent multiple dialog boxes from showing, because they're all called async
    static bool shown = false;
    if (shown)
        return;

    shown = true;
    QMessageBox::critical(main, QObject::tr("Transaction Error"), QObject::tr("There was an error sending the transaction. The error was:") + "\n\n"
        + error, QMessageBox::StandardButton::Ok);
    shown = false;
}

/**
 * Prevent all future calls from going through
 */
void Connection::shutdown() {
    shutdownInProgress = true;
}
