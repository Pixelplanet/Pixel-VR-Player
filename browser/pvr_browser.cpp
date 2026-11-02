// pvr-browser: offscreen web view helper for PixelVR's browser screen.
//
// Renders one web page offscreen (Qt WebEngine for now; the protocol is
// engine-neutral so WPE can replace it), hands frames over and takes input:
//   - shared memory (--shm NAME, created by the app): BrowserShmHeader + RGBA
//     pixels, published with a sequence counter (odd while writing),
//   - stdin, one command per line (coordinates in page pixels):
//       move X Y | down X Y | up X Y | wheel X Y DY | key NAME | text UTF8
//       nav URL | back | forward | reload | quit
//   - stdout, one event per line: ready | url URL | title TEXT | loading 0|1
//
// Uses the Widgets view: on SteamOS the installed WebEngine (6.7) doesn't match
// Qt 6.8's private QML API, so its QML module can't load.
#include "browser_protocol.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSocketNotifier>
#include <QTimer>
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWheelEvent>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

void emitLine(const QString& line) {
    const QByteArray b = line.toUtf8() + '\n';
    std::fwrite(b.constData(), 1, static_cast<size_t>(b.size()), stdout);
    std::fflush(stdout);
}

struct Shm {
    BrowserShmHeader* header = nullptr;
    uint8_t* pixels = nullptr;
    size_t size = 0;
};

bool openShm(const QString& name, int width, int height, Shm& out) {
    const QByteArray n = name.toUtf8();
    const int fd = shm_open(n.constData(), O_RDWR, 0600);
    if (fd < 0) return false;
    out.size = browserShmSize(width, height);
    void* p = mmap(nullptr, out.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    out.header = static_cast<BrowserShmHeader*>(p);
    out.pixels = static_cast<uint8_t*>(p) + sizeof(BrowserShmHeader);
    return true;
}

Qt::Key keyFromName(const QString& name) {
    if (name == "Enter") return Qt::Key_Return;
    if (name == "Backspace") return Qt::Key_Backspace;
    if (name == "Delete") return Qt::Key_Delete;
    if (name == "Left") return Qt::Key_Left;
    if (name == "Right") return Qt::Key_Right;
    if (name == "Up") return Qt::Key_Up;
    if (name == "Down") return Qt::Key_Down;
    if (name == "Escape") return Qt::Key_Escape;
    if (name == "Tab") return Qt::Key_Tab;
    if (name == "Space") return Qt::Key_Space;
    return Qt::Key_unknown;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);

    QString shmName;
    QString url = QStringLiteral("https://m.youtube.com");
    int width = 1280;
    int height = 720;
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--shm") shmName = args[++i];
        else if (args[i] == "--url") url = args[++i];
        else if (args[i] == "--width") width = args[++i].toInt();
        else if (args[i] == "--height") height = args[++i].toInt();
    }
    width = qBound(320, width, kBrowserMaxWidth);
    height = qBound(240, height, kBrowserMaxHeight);

    Shm shm;
    if (shmName.isEmpty() || !openShm(shmName, width, height, shm)) {
        std::fprintf(stderr, "pvr-browser: cannot open shared memory '%s'\n", qPrintable(shmName));
        return 2;
    }

    // Persistent profile (cookies, logins) in the app's config folder.
    QWebEngineProfile profile(QStringLiteral("pixelvr"));
    profile.setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    QWebEnginePage page(&profile);
    page.settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    page.settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    QObject::connect(&page, &QWebEnginePage::fullScreenRequested,
                     [](QWebEngineFullScreenRequest r) { r.accept(); });

    QWebEngineView view;
    view.setPage(&page);
    view.resize(width, height);
    view.load(QUrl::fromUserInput(url));
    view.show();

    // Page state -> stdout.
    QObject::connect(&view, &QWebEngineView::urlChanged,
                     [](const QUrl& u) { emitLine("url " + u.toString()); });
    QObject::connect(&view, &QWebEngineView::titleChanged,
                     [](const QString& t) { emitLine("title " + t); });
    QObject::connect(&view, &QWebEngineView::loadStarted, [] { emitLine("loading 1"); });
    QObject::connect(&view, &QWebEngineView::loadFinished, [](bool) { emitLine("loading 0"); });

    // Frames -> shared memory at ~30 fps (a grab of a 720p page takes ~6 ms).
    QTimer grabTimer;
    grabTimer.setInterval(33);
    QObject::connect(&grabTimer, &QTimer::timeout, [&] {
        QImage img = view.grab().toImage();
        if (img.isNull()) return;
        if (img.format() != QImage::Format_RGBA8888) img = img.convertToFormat(QImage::Format_RGBA8888);
        const int w = qMin(img.width(), width);
        const int h = qMin(img.height(), height);
        BrowserShmHeader* hd = shm.header;
        const uint64_t seq = hd->seq.load(std::memory_order_relaxed);
        hd->seq.store(seq + 1, std::memory_order_release);  // odd: writing
        for (int y = 0; y < h; ++y) {
            std::memcpy(shm.pixels + static_cast<size_t>(y) * width * 4, img.constScanLine(y),
                        static_cast<size_t>(w) * 4);
        }
        hd->width = static_cast<uint32_t>(width);
        hd->height = static_cast<uint32_t>(height);
        hd->seq.store(seq + 2, std::memory_order_release);  // even: frame ready
    });
    grabTimer.start();

    // stdin commands -> input events. WebEngine takes input on the view's
    // focus proxy (the render widget), not on the view itself.
    Qt::MouseButtons buttons = Qt::NoButton;
    auto target = [&]() -> QWidget* { return view.focusProxy() ? view.focusProxy() : &view; };
    auto sendMouse = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button) {
        QWidget* w = target();
        QMouseEvent ev(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &ev);
    };
    auto sendKey = [&](Qt::Key k, const QString& text) {
        QWidget* w = target();
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
        QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
        QCoreApplication::sendEvent(w, &press);
        QCoreApplication::sendEvent(w, &release);
    };
    QByteArray pending;
    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&stdinNotifier, &QSocketNotifier::activated, [&] {
        char buf[4096];
        const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {  // the app went away
            app.quit();
            return;
        }
        pending.append(buf, static_cast<int>(n));
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(pending.left(nl));
            pending.remove(0, nl + 1);
            const int sp = line.indexOf(' ');
            const QString cmd = sp < 0 ? line : line.left(sp);
            const QString rest = sp < 0 ? QString() : line.mid(sp + 1);
            const QStringList a = rest.split(' ', Qt::SkipEmptyParts);
            if (cmd == "move" && a.size() >= 2) {
                sendMouse(QEvent::MouseMove, QPointF(a[0].toDouble(), a[1].toDouble()), Qt::NoButton);
            } else if (cmd == "down" && a.size() >= 2) {
                buttons |= Qt::LeftButton;
                sendMouse(QEvent::MouseButtonPress, QPointF(a[0].toDouble(), a[1].toDouble()), Qt::LeftButton);
            } else if (cmd == "up" && a.size() >= 2) {
                buttons &= ~Qt::LeftButton;
                sendMouse(QEvent::MouseButtonRelease, QPointF(a[0].toDouble(), a[1].toDouble()), Qt::LeftButton);
            } else if (cmd == "wheel" && a.size() >= 3) {
                QWidget* w = target();
                const QPointF pos(a[0].toDouble(), a[1].toDouble());
                const int dy = a[2].toInt();
                QWheelEvent ev(pos, w->mapToGlobal(pos), QPoint(0, dy), QPoint(0, dy), buttons,
                               Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(w, &ev);
            } else if (cmd == "key" && !rest.isEmpty()) {
                const Qt::Key k = keyFromName(rest);
                sendKey(k, k == Qt::Key_Return ? QStringLiteral("\r")
                           : k == Qt::Key_Space ? QStringLiteral(" ") : QString());
            } else if (cmd == "text" && !rest.isEmpty()) {
                for (const QChar c : rest) sendKey(Qt::Key_unknown, QString(c));
            } else if (cmd == "nav" && !rest.isEmpty()) {
                view.load(QUrl::fromUserInput(rest));
            } else if (cmd == "back") {
                view.back();
            } else if (cmd == "forward") {
                view.forward();
            } else if (cmd == "reload") {
                view.reload();
            } else if (cmd == "quit") {
                app.quit();
            }
        }
    });

    emitLine("ready");
    const int rc = app.exec();
    munmap(shm.header, shm.size);
    return rc;
}
