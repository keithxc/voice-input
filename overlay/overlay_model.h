#ifndef VOICE_INPUT_OVERLAY_MODEL_H
#define VOICE_INPUT_OVERLAY_MODEL_H

#include <QObject>
#include <QLocalSocket>
#include <QTimer>

class OverlayModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool panelVisible READ panelVisible NOTIFY changed)
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(bool error READ error NOTIFY changed)
    Q_PROPERTY(double level READ level NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QString source READ source NOTIFY changed)

public:
    explicit OverlayModel(QObject *parent = nullptr);

    bool panelVisible() const { return panelVisible_; }
    bool recording() const { return recording_; }
    bool error() const { return error_; }
    double level() const { return level_; }
    QString status() const { return status_; }
    QString text() const { return text_; }
    QString source() const { return source_; }

    void connectToDaemon();
    void processLine(const QByteArray &line);

signals:
    void changed();

private:
    void readAvailable();
    void scheduleReconnect();
    void showFor(int milliseconds);

    QLocalSocket socket_;
    QTimer reconnectTimer_;
    QTimer hideTimer_;
    QByteArray receiveBuffer_;
    bool panelVisible_ = false;
    bool recording_ = false;
    bool error_ = false;
    bool sawFinal_ = false;
    double level_ = 0.0;
    QString status_ = QStringLiteral("待机");
    QString text_;
    QString source_;
};

#endif
