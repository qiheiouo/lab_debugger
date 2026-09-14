#include "ui/ai_analysis_widget.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include <iostream>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LabDebugger"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Lab Debugger Competition AI Widget Test"));

    lab::ui::AiAnalysisWidget widget;
    widget.show();
    widget.setSessionDirectory(QStringLiteral("missing-session-for-ui-test"));
    widget.generateSummary();
    const auto* model = widget.findChild<QComboBox*>(
        QStringLiteral("deepSeekModel"));
    const auto* key = widget.findChild<QLineEdit*>(
        QStringLiteral("deepSeekApiKey"));
    const auto* preview = widget.findChild<QPlainTextEdit*>(
        QStringLiteral("aiSummaryPreview"));
    const auto* confirm = widget.findChild<QCheckBox*>(
        QStringLiteral("confirmAiSend"));
    const auto* analyze = widget.findChild<QPushButton*>(
        QStringLiteral("startAiAnalysisButton"));
    const auto* status = widget.findChild<QLabel*>(
        QStringLiteral("aiAnalysisStatus"));
    if (!widget.isVisible() || !model || model->count() != 2 || !key ||
        key->echoMode() != QLineEdit::Password || !preview || !confirm ||
        !analyze || !status || !widget.summaryJson().isEmpty() ||
        !status->text().contains(QStringLiteral("有效的 Session"))) {
        std::cerr << "Lab Debugger competition AI widget test failed.\n";
        return 1;
    }
    std::cout << "Lab Debugger competition AI widget test passed.\n";
    return 0;
}
