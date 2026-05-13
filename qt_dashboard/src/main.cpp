#include "mainwindow.h"

#include <QApplication>
#include <QFont>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QFont font(QStringLiteral("Microsoft YaHei UI"), 10);
    app.setFont(font);

    qRegisterMetaType<ProbeSnapshot>("ProbeSnapshot");

    MainWindow window;
    window.show();
    return app.exec();
}

