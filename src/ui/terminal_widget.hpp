#pragma once

#include <QByteArray>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QPlainTextEdit;

namespace lab::ui {

class TerminalWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);

public slots:
    void appendChunk(const QByteArray& bytes,
                     bool transmitted,
                     qint64 timestampNs,
                     const QString& sourceId);

private:
    [[nodiscard]] QString formatPayload(const QByteArray& bytes) const;

    QPlainTextEdit* output_{};
    QComboBox* mode_{};
    QCheckBox* autoScroll_{};
    QCheckBox* paused_{};
};

}  // namespace lab::ui
