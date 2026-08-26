#pragma once

#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QTableWidget;

namespace lab::ui {

class ProtocolWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProtocolWidget(QWidget* parent = nullptr);

signals:
    void loadProtocolRequested(QString path);
    void disableProtocolRequested();

public slots:
    void setProtocolLoaded(const QString& name, const QStringList& numericFields);
    void setProtocolCleared();
    void showLoadErrors(const QStringList& issues);
    void appendEvents(const QVariantList& events);
    void setStatistics(quint64 decoded,
                       quint64 discardedBytes,
                       quint64 checksumErrors,
                       quint64 lengthErrors,
                       quint64 decodeErrors);

private:
    QLabel* statusLabel_{};
    QLabel* fieldsLabel_{};
    QLabel* decodedLabel_{};
    QLabel* discardedLabel_{};
    QLabel* checksumLabel_{};
    QLabel* lengthLabel_{};
    QLabel* decodeLabel_{};
    QTableWidget* packets_{};
    QTableWidget* fields_{};
    QPlainTextEdit* rawHex_{};
};

}  // namespace lab::ui
