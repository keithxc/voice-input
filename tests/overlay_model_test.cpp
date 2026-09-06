#include "overlay_model.h"

#include <QtTest/QTest>

class OverlayModelTest final : public QObject {
    Q_OBJECT

private slots:
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
