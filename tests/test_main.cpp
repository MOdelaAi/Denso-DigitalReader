// Custom Catch2 entry point: the DB tests use QSqlDatabase, whose driver/plugin
// machinery requires a live QCoreApplication (the app gets one from QApplication
// in main.cpp). Create it before running the test session so those tests don't
// segfault.
#include <catch2/catch_session.hpp>

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
