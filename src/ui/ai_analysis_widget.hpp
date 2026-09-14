#pragma once

#include "competition/deepseek_client.hpp"

#include <QByteArray>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace lab::ui {

class AiAnalysisWidget final : public QWidget {
public:
    explicit AiAnalysisWidget(QWidget* parent = nullptr);
    ~AiAnalysisWidget() override;

    [[nodiscard]] QString sessionDirectory() const;
    [[nodiscard]] QByteArray summaryJson() const;
    [[nodiscard]] QString reportText() const;

    void setSessionDirectory(const QString& directory);
    void generateSummary();

private:
    void chooseSessionDirectory();
    void loadBundledDemo();
    void startAnalysis();
    void saveReport();
    void setBusy(bool busy);
    void setStatus(const QString& message, bool error = false);

    lab::competition::DeepSeekClient client_;
    QByteArray summaryJson_;
    QString reportText_;
    QLineEdit* sessionPath_{};
    QLineEdit* apiKey_{};
    QComboBox* model_{};
    QPlainTextEdit* context_{};
    QPlainTextEdit* summaryPreview_{};
    QPlainTextEdit* report_{};
    QCheckBox* confirmSend_{};
    QLabel* status_{};
    QPushButton* summarizeButton_{};
    QPushButton* analyzeButton_{};
    QPushButton* cancelButton_{};
    QPushButton* saveButton_{};
};

}  // namespace lab::ui
