#include "overlay_model.h"

#include <LayerShellQt/Window>
#include <QCommandLineParser>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("voice-input-overlay"));
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("demo"), QStringLiteral("Show deterministic demo content")});
    parser.addOption({QStringLiteral("screenshot"),
                      QStringLiteral("Save a screenshot and exit"),
                      QStringLiteral("path")});
    parser.process(application);

    OverlayModel model;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("overlayModel"), &model);
    engine.loadFromModule(QStringLiteral("VoiceInput"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "voice-input-overlay: QML root window was not created";
        return 1;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        qCritical() << "voice-input-overlay: root object is not a QQuickWindow";
        return 1;
    }
    if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
        auto *layerWindow = LayerShellQt::Window::get(window);
        layerWindow->setScope(QStringLiteral("voice-input-overlay"));
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        layerWindow->setAnchors(LayerShellQt::Window::AnchorBottom);
        layerWindow->setMargins(QMargins(0, 0, 0, 72));
        layerWindow->setExclusiveZone(0);
        layerWindow->setKeyboardInteractivity(
            LayerShellQt::Window::KeyboardInteractivityNone);
        layerWindow->setActivateOnShow(false);
        layerWindow->setWantsToBeOnActiveScreen(true);
    }

    if (parser.isSet(QStringLiteral("demo"))) {
        model.processLine(R"({"event":"state","recording":true})");
        model.processLine(R"({"event":"source","text":"AB13X USB Microphone"})");
        model.processLine(R"({"event":"level","rms":0.18})");
        model.processLine(R"({"event":"partial","text":"NixOS 原生流式语音输入"})");
    } else {
        model.connectToDaemon();
    }

    const QString screenshot = parser.value(QStringLiteral("screenshot"));
    if (!screenshot.isEmpty()) {
        QTimer::singleShot(500, &application, [&application, window, screenshot] {
            const bool saved = window != nullptr && window->grabWindow().save(screenshot);
            if (!saved) {
                qCritical() << "voice-input-overlay: failed to render screenshot to"
                            << screenshot;
            }
            application.exit(saved ? 0 : 2);
        });
    }

    return application.exec();
}
