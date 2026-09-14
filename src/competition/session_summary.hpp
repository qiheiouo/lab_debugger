#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>

namespace lab::competition {

struct SessionSummaryLimits {
    std::size_t maximumValueRows{200'000};
    std::size_t maximumFields{48};
    std::size_t maximumEvents{50};
    qint64 maximumLineBytes{64 * 1024};
    qsizetype maximumOutputBytes{96 * 1024};
};

struct SessionSummaryResult {
    bool success{};
    QString error;
    QByteArray json;
    quint64 valueRowsScanned{};
    quint64 malformedRows{};
    quint64 omittedFields{};
    bool truncated{};
};

class SessionSummaryBuilder final {
public:
    [[nodiscard]] static SessionSummaryResult build(
        const QString& sessionDirectory,
        const SessionSummaryLimits& limits = {});
};

}  // namespace lab::competition
