// Measures the letterbox bars of files the way the player does. Development tool, never released.
//
//   letterbox_test <file> [<file> ...]

#include "media/letterbox.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        int judged = 0;
        const float f = pixelvr::LetterboxDetector::measure(argv[i], &judged);
        std::printf("%5.1f %% top+bottom each  (%d frames judged)  %s\n", f * 100.0f, judged, argv[i]);
    }
    return 0;
}
