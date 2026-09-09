#pragma once

#include <QHash>
#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace lab::ui {

class ProtocolWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProtocolWidget(QWidget* parent = nullptr);

signals:
    void csvConfigurationRequested(QString sourceId, QStringList fields);
    void protocolConfigurationRequested(QString sourceId, QString path);
    void disableProtocolConfigurationRequested(QString sourceId);
    void resetSourceConfigurationRequested(QString sourceId);

public slots:
    void setProtocolLoaded(const QString& name, const QStringList& numericFields);
    void setProtocolCleared();
    void showLoadErrors(const QStringList& issues);
    void setParserSources(const QVariantList& sources);
    void showParserConfigured(QString sourceId,
                              QString mode,
                              QString name,
                              QStringList fields);
    void showParserErrors(QString sourceId, const QStringList& issues);
    void appendEvents(const QVariantList& events);
    void setStatistics(quint64 decoded,
                       quint64 discardedBytes,
                       quint64 checksumErrors,
                       quint64 lengthErrors,
                       quint64 decodeErrors);

private:
    [[nodiscard]] QString currentSourceId() const;
    void showSelectedConfiguration();

    QComboBox* sourceSelector_{};
    QLineEdit* csvFields_{};
    QPushButton* resetSourceButton_{};
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
    QHash<QString, QString> configurationSummaries_;
    QHash<QString, QStringList> configurationFields_;
};

}  // namespace lab::ui
