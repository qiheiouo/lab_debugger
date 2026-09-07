#include "ui/main_window.hpp"
#include "ui/plot_widget.hpp"
#include "ui/send_panel.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QTimer>

#include <iostream>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LabDebugger"));
    QCoreApplication::setApplicationName(QStringLiteral("Lab Debugger GUI Test"));

    int result = -1;
    {
        lab::ui::SendPanel sendPanel;
        sendPanel.setTarget(1);
        if (sendPanel.target() != 1) {
            std::cerr << "GUI smoke test failed: network send target was not selected\n";
            return 1;
        }

        lab::core::TimeSeriesStore store(100);
        lab::ui::PlotWidget plot(&store);
        const auto csvFields = plot.fieldNames();
        plot.useExternalFields(
            {QStringLiteral("serial:COM5.speed"),
             QStringLiteral("udp:127.0.0.1:9000.speed")});
        const auto* visibleFields = plot.findChild<QListWidget*>();
        if (!visibleFields || visibleFields->count() != 2 ||
            visibleFields->item(0)->text() != QStringLiteral("serial:COM5.speed") ||
            plot.fieldNames() != csvFields) {
            std::cerr << "GUI smoke test failed: source-qualified plot fields changed CSV parsing\n";
            return 1;
        }

        lab::ui::MainWindow window;
        window.show();
        if (!window.isVisible()) {
            std::cerr << "GUI smoke test failed: main window is not visible\n";
            return 1;
        }
        QTimer::singleShot(150, &application, [] {
            QCoreApplication::exit(0);
        });
        result = application.exec();
    }

    if (result != 0) {
        std::cerr << "GUI smoke test failed: event loop returned " << result << '\n';
        return 1;
    }
    std::cout << "Lab Debugger GUI smoke test passed.\n";
    return 0;
}
