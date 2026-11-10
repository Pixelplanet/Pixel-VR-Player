// Opens a frame-sequential 3D file with the player's MediaEngine (no VR) and checks that it is delivered
// as full side-by-side pairs at half the frame rate. Writes the luma of one pair to <out>.pgm.
// Development tool, never released.
//
//   fs3d_test <file> <out.pgm>

#include "media/media_engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using pixelvr::MediaEngine;
using pixelvr::VideoFrame;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    MediaEngine e;
    if (!e.open(argv[1])) {
        std::printf("FAIL open: %s\n", e.lastError().c_str());
        return 1;
    }
    e.setMuted(true);
    e.setPaused(false);
    const double from = argc > 3 ? std::atof(argv[3]) : 0.0;
    if (from > 0) e.seek(from);
    std::printf("size %dx%d, %.3f fps (per eye)\n", e.width(), e.height(), e.frameRate());
    VideoFrame f;
    uint64_t lastSerial = 0;
    int frames = 0;
    double firstPts = -1.0, lastPts = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    bool saved = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) {
        if (e.latestFrame(f) && f.serial != lastSerial) {
            lastSerial = f.serial;
            ++frames;
            if (firstPts < 0) firstPts = f.pts;
            lastPts = f.pts;
            {
                double d = 0.0;
                const int hw2 = f.width / 2;
                for (int r = 0; r < f.height; r += 8)
                    for (int c = 0; c < hw2; c += 8) d += std::abs(f.y[r * f.width + c] - f.y[r * f.width + hw2 + c]);
                d /= (f.height / 8) * (hw2 / 8);
                if (frames % 12 == 1) std::printf("  pts %.3f  left/right difference %.2f\n", f.pts, d);
            }
            if (!saved && f.pts > from + 3.0 && f.width > 0) {
                FILE* o = std::fopen(argv[2], "wb");
                std::fprintf(o, "P5\n%d %d\n255\n", f.width, f.height);
                std::fwrite(f.y.data(), 1, f.y.size(), o);
                std::fclose(o);
                // How different the halves are (0 = identical: not stereo / pairing wrong).
                double diff = 0.0;
                const int hw = f.width / 2;
                for (int r = 0; r < f.height; r += 4)
                    for (int c = 0; c < hw; c += 4)
                        diff += std::abs(f.y[r * f.width + c] - f.y[r * f.width + hw + c]);
                std::printf("pair at %.3f s: %dx%d, mean left/right luma difference %.2f\n", f.pts, f.width, f.height,
                            diff / ((f.height / 4) * (hw / 4)));
                saved = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double span = lastPts - firstPts;
    std::printf("%d frames delivered over %.2f s of video: %.2f per second\n", frames, span, span > 0 ? (frames - 1) / span : 0.0);
    return 0;
}
