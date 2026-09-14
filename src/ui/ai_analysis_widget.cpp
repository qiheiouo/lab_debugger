#include "ui/ai_analysis_widget.hpp"

#include "competition/session_summary.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace lab::ui {

AiAnalysisWidget::AiAnalysisWidget(QWidget* parent)
    : QWidget(parent), client_(this) {
    setObjectName(QStringLiteral("aiAnalysisWidget"));

    auto* explanation = new QLabel(
        tr("比赛专用功能：程序先在本地汇总 Session 的字段统计与少量 Marker/告警，"
           "你确认预览后才调用 DeepSeek。原始 CDR、原始字节流和完整 values.csv "
           "不会发送。比赛结束后本页不合并回主线。"),
        this);
    explanation->setWordWrap(true);
    explanation->setObjectName(QStringLiteral("secondaryText"));

    sessionPath_ = new QLineEdit(this);
    sessionPath_->setObjectName(QStringLiteral("aiSessionPath"));
    sessionPath_->setReadOnly(true);
    auto* chooseButton = new QPushButton(tr("选择 Session"), this);
    chooseButton->setObjectName(QStringLiteral("chooseAiSessionButton"));
    summarizeButton_ = new QPushButton(tr("生成本地摘要"), this);
    summarizeButton_->setObjectName(QStringLiteral("generateAiSummaryButton"));
    auto* sessionRow = new QHBoxLayout;
    sessionRow->addWidget(sessionPath_, 1);
    sessionRow->addWidget(chooseButton);
    sessionRow->addWidget(summarizeButton_);

    model_ = new QComboBox(this);
    model_->setObjectName(QStringLiteral("deepSeekModel"));
    model_->addItem(tr("DeepSeek V4 Flash（省费用，推荐演示）"),
                    QStringLiteral("deepseek-v4-flash"));
    model_->addItem(tr("DeepSeek V4 Pro（更强，费用更高）"),
                    QStringLiteral("deepseek-v4-pro"));
    apiKey_ = new QLineEdit(this);
    apiKey_->setObjectName(QStringLiteral("deepSeekApiKey"));
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(tr("运行时粘贴 DeepSeek API Key（不会写入项目或 Session）"));
    apiKey_->setMaxLength(4096);
    context_ = new QPlainTextEdit(this);
    context_->setObjectName(QStringLiteral("aiExperimentContext"));
    context_->setPlaceholderText(
        tr("可选：说明设备、实验目标和你观察到的问题。例如：ROS2 小车急转弯时"
           "里程计漂移，请结合 IMU 与 cmd_vel 判断。最多发送前 2000 个字符。"));
    context_->setMaximumHeight(90);

    auto* settings = new QFormLayout;
    settings->addRow(tr("Session"), sessionRow);
    settings->addRow(tr("模型"), model_);
    settings->addRow(tr("API Key"), apiKey_);
    settings->addRow(tr("实验背景"), context_);
    auto* settingsBox = new QGroupBox(tr("分析输入"), this);
    settingsBox->setLayout(settings);

    summaryPreview_ = new QPlainTextEdit(this);
    summaryPreview_->setObjectName(QStringLiteral("aiSummaryPreview"));
    summaryPreview_->setReadOnly(true);
    summaryPreview_->setPlaceholderText(tr("先选择 Session 并生成摘要"));
    confirmSend_ = new QCheckBox(
        tr("我已检查上方摘要，同意将这些聚合信息发送给 DeepSeek"), this);
    confirmSend_->setObjectName(QStringLiteral("confirmAiSend"));
    auto* summaryBox = new QGroupBox(tr("待发送内容预览"), this);
    auto* summaryLayout = new QVBoxLayout(summaryBox);
    summaryLayout->addWidget(summaryPreview_, 1);
    summaryLayout->addWidget(confirmSend_);

    report_ = new QPlainTextEdit(this);
    report_->setObjectName(QStringLiteral("aiDiagnosisReport"));
    report_->setReadOnly(true);
    report_->setPlaceholderText(
        tr("DeepSeek 将返回：总体判断、异常现象、证据、可能原因、验证步骤与风险"));
    auto* reportBox = new QGroupBox(tr("证据化诊断报告"), this);
    auto* reportLayout = new QVBoxLayout(reportBox);
    reportLayout->addWidget(report_, 1);

    analyzeButton_ = new QPushButton(tr("发送并分析"), this);
    analyzeButton_->setObjectName(QStringLiteral("startAiAnalysisButton"));
    cancelButton_ = new QPushButton(tr("取消请求"), this);
    cancelButton_->setObjectName(QStringLiteral("cancelAiAnalysisButton"));
    cancelButton_->setEnabled(false);
    saveButton_ = new QPushButton(tr("保存报告"), this);
    saveButton_->setObjectName(QStringLiteral("saveAiReportButton"));
    saveButton_->setEnabled(false);
    status_ = new QLabel(tr("尚未生成本地摘要"), this);
    status_->setObjectName(QStringLiteral("aiAnalysisStatus"));
    status_->setWordWrap(true);
    auto* actions = new QHBoxLayout;
    actions->addWidget(analyzeButton_);
    actions->addWidget(cancelButton_);
    actions->addWidget(saveButton_);
    actions->addStretch(1);
    actions->addWidget(status_, 1);

    auto* content = new QHBoxLayout;
    content->addWidget(summaryBox, 1);
    content->addWidget(reportBox, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(explanation);
    layout->addWidget(settingsBox);
    layout->addLayout(content, 1);
    layout->addLayout(actions);

    connect(chooseButton, &QPushButton::clicked,
            this, &AiAnalysisWidget::chooseSessionDirectory);
    connect(summarizeButton_, &QPushButton::clicked,
            this, &AiAnalysisWidget::generateSummary);
    connect(analyzeButton_, &QPushButton::clicked,
            this, &AiAnalysisWidget::startAnalysis);
    connect(cancelButton_, &QPushButton::clicked,
            this, [this] { client_.cancel(); });
    connect(saveButton_, &QPushButton::clicked,
            this, &AiAnalysisWidget::saveReport);
    client_.setCallbacks(
        [this](bool busy) { setBusy(busy); },
        [this](const QString& report, const QVariantMap& usage) {
                reportText_ = report;
                report_->setPlainText(report);
                saveButton_->setEnabled(true);
                const auto inputTokens = usage.value(QStringLiteral("prompt_tokens"))
                                             .toLongLong();
                const auto outputTokens = usage.value(QStringLiteral("completion_tokens"))
                                              .toLongLong();
                setStatus(tr("分析完成；输入 %1 tokens，输出 %2 tokens")
                              .arg(inputTokens)
                              .arg(outputTokens));
        },
        [this](const QString& message) { setStatus(message, true); });
    connect(sessionPath_, &QLineEdit::textChanged, this, [this] {
        summaryJson_.clear();
        summaryPreview_->clear();
        confirmSend_->setChecked(false);
    });
}

AiAnalysisWidget::~AiAnalysisWidget() {
    client_.setCallbacks({}, {}, {});
    client_.cancel();
}

QString AiAnalysisWidget::sessionDirectory() const {
    return sessionPath_->text();
}

QByteArray AiAnalysisWidget::summaryJson() const {
    return summaryJson_;
}

QString AiAnalysisWidget::reportText() const {
    return reportText_;
}

void AiAnalysisWidget::setSessionDirectory(const QString& directory) {
    if (directory.trimmed().isEmpty()) {
        sessionPath_->clear();
        setStatus(tr("尚未选择 Session"), true);
        return;
    }
    sessionPath_->setText(QDir(directory).absolutePath());
    setStatus(tr("已选择 Session；请生成并检查本地摘要"));
}

void AiAnalysisWidget::generateSummary() {
    const auto result = lab::competition::SessionSummaryBuilder::build(
        sessionPath_->text());
    if (!result.success) {
        summaryJson_.clear();
        summaryPreview_->clear();
        confirmSend_->setChecked(false);
        setStatus(result.error, true);
        return;
    }
    summaryJson_ = result.json;
    summaryPreview_->setPlainText(QString::fromUtf8(summaryJson_));
    confirmSend_->setChecked(false);
    setStatus(
        tr("本地摘要完成：扫描 %1 行，异常行 %2%3；确认后才会联网")
            .arg(result.valueRowsScanned)
            .arg(result.malformedRows)
            .arg(result.truncated ? tr("，已按上限截断") : QString()));
}

void AiAnalysisWidget::chooseSessionDirectory() {
    const auto initial = sessionPath_->text().isEmpty()
                             ? QStandardPaths::writableLocation(
                                   QStandardPaths::DocumentsLocation)
                             : sessionPath_->text();
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("选择 Lab Debugger Session"), initial);
    if (!selected.isEmpty()) setSessionDirectory(selected);
}

void AiAnalysisWidget::startAnalysis() {
    if (summaryJson_.isEmpty()) {
        setStatus(tr("请先生成本地摘要"), true);
        return;
    }
    if (!confirmSend_->isChecked()) {
        setStatus(tr("请先检查摘要并勾选发送确认"), true);
        return;
    }
    lab::competition::DeepSeekRequest request;
    request.apiKey = apiKey_->text();
    request.model = model_->currentData().toString();
    request.sessionSummaryJson = summaryJson_;
    request.experimentContext = context_->toPlainText();
    if (client_.start(request)) {
        reportText_.clear();
        report_->clear();
        saveButton_->setEnabled(false);
        setStatus(tr("正在调用 DeepSeek，请稍候……"));
    }
}

void AiAnalysisWidget::saveReport() {
    if (reportText_.isEmpty()) return;
    const auto suggested = QDir(sessionPath_->text()).filePath(
        QStringLiteral("deepseek_diagnosis_report.json"));
    const auto path = QFileDialog::getSaveFileName(
        this, tr("保存 DeepSeek 诊断报告"), suggested,
        tr("JSON 文档 (*.json);;文本文件 (*.txt)"));
    if (path.isEmpty()) return;
    const auto bytes = reportText_.toUtf8();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        setStatus(tr("报告保存失败：%1").arg(path), true);
        return;
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        setStatus(tr("报告保存失败：%1").arg(path), true);
        return;
    }
    setStatus(tr("报告已保存：%1").arg(path));
}

void AiAnalysisWidget::setBusy(bool busy) {
    summarizeButton_->setEnabled(!busy);
    analyzeButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
}

void AiAnalysisWidget::setStatus(const QString& message, bool error) {
    status_->setText(message);
    status_->setStyleSheet(error ? QStringLiteral("color: #ff7b72;")
                                 : QStringLiteral("color: #5ad49b;"));
}

}  // namespace lab::ui
