#include "overlay_model.h"

#include <QtTest/QTest>

class OverlayModelTest final : public QObject {
    Q_OBJECT

private slots:
    void detectsDesktopStyle() {
        const QByteArray previous = qgetenv("XDG_CURRENT_DESKTOP");
        qputenv("XDG_CURRENT_DESKTOP", "KDE");
        OverlayModel kde;
        QCOMPARE(kde.desktopStyle(), QStringLiteral("kde"));
        qputenv("XDG_CURRENT_DESKTOP", "GNOME");
        OverlayModel gnome;
        QCOMPARE(gnome.desktopStyle(), QStringLiteral("gnome"));
        if (previous.isNull()) qunsetenv("XDG_CURRENT_DESKTOP");
        else qputenv("XDG_CURRENT_DESKTOP", previous);
    }

    void ignoresIdleHello() {
        OverlayModel model;
        model.processLine(R"({"event":"hello","recording":false})");
        QVERIFY(!model.panelVisible());
        QVERIFY(!model.recording());
    }

    void followsRecordingLifecycle() {
        OverlayModel model;
        model.processLine(R"({"event":"state","recording":true})");
        QVERIFY(model.panelVisible());
        QVERIFY(model.recording());
        QCOMPARE(model.status(), QStringLiteral("正在听…"));

        model.processLine(R"({"event":"level","rms":0.125})");
        QCOMPARE(model.level(), 0.125);
        model.processLine(R"({"event":"source","text":"USB Microphone"})");
        QCOMPARE(model.source(), QStringLiteral("USB Microphone"));
        model.processLine(R"({"event":"partial","text":"你好 NixOS"})");
        QCOMPARE(model.text(), QStringLiteral("你好 NixOS"));
        QCOMPARE(model.status(), QStringLiteral("正在识别…"));

        model.processLine(R"({"event":"final","text":"你好 NixOS"})");
        QCOMPARE(model.status(), QStringLiteral("正在写入…"));
        model.processLine(R"({"event":"state","recording":false})");
        QVERIFY(!model.recording());
        QCOMPARE(model.status(), QStringLiteral("已输入"));
        QVERIFY(model.panelVisible());
    }

    void repeatedStatePreservesTranscript() {
        OverlayModel model;
        model.processLine(R"({"event":"state","recording":true})");
        model.processLine(R"({"event":"partial","text":"保留这句话"})");
        model.processLine(R"({"event":"state","recording":true})");
        QCOMPARE(model.text(), QStringLiteral("保留这句话"));
        QCOMPARE(model.status(), QStringLiteral("正在识别…"));
        model.processLine(R"({"event":"finishing"})");
        QCOMPARE(model.status(), QStringLiteral("正在收尾…"));
        model.processLine(R"({"event":"final","text":"保留这句话。"})");
        model.processLine(R"({"event":"output-success"})");
        QCOMPARE(model.status(), QStringLiteral("已输入，继续听…"));
        QVERIFY(model.recording());
    }

    void processingCanCompleteOrCancel() {
        OverlayModel model;
        model.processLine(R"({"event":"state","recording":true})");
        model.processLine(R"({"event":"partial","text":"draft"})");
        model.processLine(R"({"event":"processing"})");
        QVERIFY(model.processing());
        QVERIFY(!model.recording());
        QVERIFY(model.panelVisible());
        QCOMPARE(model.status(), QStringLiteral("正在校对…"));
        QCOMPARE(model.text(), QStringLiteral("draft"));
        model.processLine(R"({"event":"final","text":"corrected"})");
        model.processLine(R"({"event":"output-success"})");
        model.processLine(R"({"event":"state","recording":false})");
        QVERIFY(!model.processing());
        QCOMPARE(model.status(), QStringLiteral("已输入"));
        model.processLine(R"({"event":"state","recording":true})");
        model.processLine(R"({"event":"processing"})");
        model.processLine(R"({"event":"cancelled"})");
        model.processLine(R"({"event":"state","recording":false})");
        QVERIFY(!model.processing());
        QVERIFY(model.text().isEmpty());
        QCOMPARE(model.status(), QStringLiteral("已取消"));
    }

    void rawSignalFeedbackDoesNotReplaceTranscript() {
        OverlayModel model;
        model.processLine(R"({"event":"state","recording":true})");
        model.processLine(R"({"event":"partial","text":"保留文字"})");
        model.processLine(R"({"event":"level","rms":0.1,"raw_rms":0.1,"clipping":0.02})");
        QVERIFY(model.hint().contains(QStringLiteral("过响")));
        QCOMPARE(model.text(), QStringLiteral("保留文字"));
        for (int i = 0; i < 25; ++i)
            model.processLine(R"({"event":"level","rms":0.01,"raw_rms":0.001,"clipping":0})");
        QVERIFY(model.hint().contains(QStringLiteral("偏小")));
        model.processLine(R"({"event":"level","rms":0.03,"raw_rms":0.03,"clipping":0})");
        QVERIFY(model.hint().isEmpty());
    }

    void reportsOutputFailure() {
        OverlayModel model;
        model.processLine(R"({"event":"state","recording":true})");
        model.processLine(R"({"event":"final","text":"测试"})");
        model.processLine(R"({"event":"output-error","backend":"fcitx5"})");
        model.processLine(R"({"event":"state","recording":false})");
        QVERIFY(model.error());
        QCOMPARE(model.status(), QStringLiteral("无法写入当前输入框"));
        QVERIFY(model.panelVisible());
    }
};

QTEST_GUILESS_MAIN(OverlayModelTest)
#include "overlay_model_test.moc"
