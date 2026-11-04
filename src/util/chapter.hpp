#pragma once
// A chapter of the playing video (a file's own chapters, or YouTube's).

#include <string>
#include <vector>

namespace pixelvr {

struct Chapter {
    double start = 0.0;  // seconds
    std::string title;
};
using ChapterList = std::vector<Chapter>;

}  // namespace pixelvr
