#pragma once

#include "xr/openxr_headers.hpp"
#include "util/logging.hpp"

namespace pixelvr {

// Fills `buffer` with the human-readable name of an XrResult, falling back to
// the numeric value. `buffer` must be at least XR_MAX_RESULT_STRING_SIZE bytes.
const char* xr_result_string(XrInstance instance, XrResult result, char* buffer);

} // namespace pixelvr

// Logs and returns false on failure. Use inside functions returning bool.
#define PIXELVR_XR_CHECK(instance, expr)                                            \
    do {                                                                            \
        XrResult _res = (expr);                                                     \
        if (XR_FAILED(_res)) {                                                      \
            char _buf[XR_MAX_RESULT_STRING_SIZE];                                   \
            PIXELVR_LOG_ERROR("%s failed: %s", #expr,                               \
                              ::pixelvr::xr_result_string((instance), _res, _buf)); \
            return false;                                                           \
        }                                                                           \
    } while (0)

// Logs on failure but continues; evaluates to the XrResult.
#define PIXELVR_XR_WARN(instance, expr)                                             \
    ([&]() -> XrResult {                                                            \
        XrResult _res = (expr);                                                     \
        if (XR_FAILED(_res)) {                                                      \
            char _buf[XR_MAX_RESULT_STRING_SIZE];                                   \
            PIXELVR_LOG_WARN("%s: %s", #expr,                                       \
                             ::pixelvr::xr_result_string((instance), _res, _buf));  \
        }                                                                           \
        return _res;                                                                \
    }())
