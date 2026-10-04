#include "xr/xr_result.hpp"

#include <cstdio>

namespace pixelvr {

const char* xr_result_string(XrInstance instance, XrResult result, char* buffer) {
    if (instance != XR_NULL_HANDLE &&
        XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
        return buffer;
    }
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XrResult(%d)",
                  static_cast<int>(result));
    return buffer;
}

} // namespace pixelvr
