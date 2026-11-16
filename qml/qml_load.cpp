// Loads one QML document with one directory on the import path, as a consumer that links nothing but Qt
// would, and says whether it instantiated: `loaded <type>`, or why not. A case of the binding runs it
// against the module as installed.
#include <cstdio>

#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: qml_load <import path> <document>\n");
        return 2;
    }
    QCoreApplication app(argc, argv);
    QQmlEngine engine;
    engine.addImportPath(QString::fromLocal8Bit(argv[1]));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[2])));
    QObject *made = component.create();
    if (made == nullptr) {
        std::fprintf(stderr, "%s\n", component.errorString().toLocal8Bit().constData());
        return 1;
    }
    std::printf("loaded %s\n", made->metaObject()->className());
    delete made;
    return 0;
}
