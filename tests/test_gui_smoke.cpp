#include "ui/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QTimer>

#include <iostream>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LabDebugger"));
    QCoreApplication::setApplicationName(QStringLiteral("Lab Debugger GUI Test"));

    int result = -1;
    {
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
