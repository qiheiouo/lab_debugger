#include "lab/core/logger.hpp"
#include "ui/main_window.hpp"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QStandardPaths>
#include <QTimer>

#include <filesystem>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LabDebugger"));
    QCoreApplication::setApplicationName(QStringLiteral("Lab Debugger"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.11.0"));

    const auto dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDirectory);
    lab::core::Logger::instance().setLogFile(
        std::filesystem::path((dataDirectory + QStringLiteral("/lab_debugger.log")).toStdWString()));

    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSize(10);
    application.setFont(font);
    application.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #151b25; color: #d7dee8; }
        QToolBar { background: #1b2431; border-bottom: 1px solid #2a3648; spacing: 7px; padding: 5px; }
        QStatusBar { background: #1b2431; border-top: 1px solid #2a3648; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QListWidget, QTableWidget {
            background: #0f1520; border: 1px solid #344257; border-radius: 4px; padding: 4px;
            selection-background-color: #285b8f;
        }
        QComboBox::drop-down { border: 0; width: 20px; }
        QPushButton, QToolButton {
            background: #243246; border: 1px solid #3b4c63; border-radius: 4px; padding: 5px 10px;
        }
        QPushButton:hover, QToolButton:hover { background: #30435c; }
        QPushButton:pressed, QToolButton:pressed { background: #1d6ca1; }
        QPushButton:disabled { color: #697587; background: #1b222d; }
        QTabWidget::pane { border: 1px solid #2a3648; }
        QTabBar::tab { background: #1b2431; padding: 8px 18px; border: 1px solid #2a3648; }
        QTabBar::tab:selected { background: #26364b; color: #ffffff; }
        QHeaderView::section { background: #202b3a; border: 0; border-right: 1px solid #344257; padding: 5px; }
        QSplitter::handle { background: #2a3648; }
        QLabel#secondaryText { color: #8592a3; }
        QToolTip { color: #e7edf5; background: #202b3a; border: 1px solid #50627a; }
    )"));

    lab::ui::MainWindow window;
    window.show();
    if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(300, &application, [] {
            QCoreApplication::exit(0);
        });
    }
    return application.exec();
}
