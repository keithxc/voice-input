#include "overlay_model.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

OverlayModel::OverlayModel(QObject *parent) : QObject(parent) {
    reconnectTimer_.setInterval(1000);
    reconnectTimer_.setSingleShot(true);
    hideTimer_.setSingleShot(true);

    connect(&socket_, &QLocalSocket::readyRead, this, &OverlayModel::readAvailable);
    connect(&socket_, &QLocalSocket::disconnected,
            this, &OverlayModel::scheduleReconnect);
    connect(&socket_, &QLocalSocket::errorOccurred,
            this, [this](QLocalSocket::LocalSocketError) { scheduleReconnect(); });
    connect(&reconnectTimer_, &QTimer::timeout, this, &OverlayModel::connectToDaemon);
    connect(&hideTimer_, &QTimer::timeout, this, [this] {
        panelVisible_ = false;
        emit changed();
    });
}

void OverlayModel::connectToDaemon() {
    if (socket_.state() != QLocalSocket::UnconnectedState) return;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        scheduleReconnect();
        return;
    }
    socket_.connectToServer(QDir(runtime).filePath(
        QStringLiteral("voice-input/voice-input.sock")), QIODevice::ReadOnly);
}

void OverlayModel::scheduleReconnect() {
    if (!reconnectTimer_.isActive()) reconnectTimer_.start();
}

void OverlayModel::showFor(int milliseconds) {
    panelVisible_ = true;
    hideTimer_.start(milliseconds);
}

void OverlayModel::readAvailable() {
    receiveBuffer_.append(socket_.readAll());
    for (;;) {
        const qsizetype newline = receiveBuffer_.indexOf('\n');
        if (newline < 0) break;
        processLine(receiveBuffer_.left(newline));
        receiveBuffer_.remove(0, newline + 1);
    }
}

void OverlayModel::processLine(const QByteArray &line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject object = document.object();
    const QString event = object.value(QStringLiteral("event")).toString();

    if (event == QStringLiteral("hello")) {
        recording_ = object.value(QStringLiteral("recording")).toBool();
        if (recording_) {
            error_ = false;
            sawFinal_ = false;
            text_.clear();
            status_ = QStringLiteral("正在听…");
            panelVisible_ = true;
            hideTimer_.stop();
            emit changed();
        }
        return;
    }

    if (event == QStringLiteral("state")) {
        const bool nextRecording = object.value(QStringLiteral("recording")).toBool();
        const bool wasRecording = recording_;
        recording_ = nextRecording;
        if (nextRecording) {
            error_ = false;
            sawFinal_ = false;
            text_.clear();
            level_ = 0.0;
            status_ = QStringLiteral("正在听…");
            panelVisible_ = true;
            hideTimer_.stop();
        } else if (wasRecording || sawFinal_ || error_) {
            level_ = 0.0;
            if (!error_) {
                status_ = sawFinal_ ? QStringLiteral("已输入")
                                    : QStringLiteral("未识别到语音");
            }
            showFor(error_ ? 3000 : 1800);
        }
        emit changed();
        return;
    }

    if (event == QStringLiteral("level")) {
        level_ = qBound(0.0, object.value(QStringLiteral("rms")).toDouble(), 1.0);
        emit changed();
        return;
    }

    if (event == QStringLiteral("source")) {
        source_ = object.value(QStringLiteral("text")).toString();
        emit changed();
        return;
    }

    if (event == QStringLiteral("partial")) {
        text_ = object.value(QStringLiteral("text")).toString();
        status_ = QStringLiteral("正在识别…");
        panelVisible_ = true;
        emit changed();
        return;
    }

    if (event == QStringLiteral("final")) {
        text_ = object.value(QStringLiteral("text")).toString();
        sawFinal_ = !text_.isEmpty();
        status_ = QStringLiteral("正在写入…");
        panelVisible_ = true;
        emit changed();
        return;
    }

    if (event == QStringLiteral("output-error") || event == QStringLiteral("error")) {
        error_ = true;
        status_ = event == QStringLiteral("output-error")
            ? QStringLiteral("无法写入当前输入框")
            : QStringLiteral("语音输入发生错误");
        showFor(3000);
        emit changed();
    }
}
