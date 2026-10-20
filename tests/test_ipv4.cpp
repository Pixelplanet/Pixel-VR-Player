#include "util/ipv4.hpp"
#include <cassert>
#include <iostream>

using namespace pixelvr;

int main() {
    std::cout << "[TEST] Starting IPv4 sanitization & validation test..." << std::endl;

    // Normal valid IP
    assert(sanitizeIpv4("192.168.1.1", true) == "192.168.1.1");
    assert(isValidIpv4("192.168.1.1"));

    // Leading zeros dropped in each section
    assert(sanitizeIpv4("192.168.01.005", true) == "192.168.1.5");
    assert(sanitizeIpv4("010.000.001.020", true) == "10.0.1.20");
    assert(sanitizeIpv4("0.0.0.0", true) == "0.0.0.0");

    // Clamping > 255
    assert(sanitizeIpv4("192.168.1.300", true) == "192.168.1.255");
    assert(sanitizeIpv4("999.999.999.999", true) == "255.255.255.255");

    // Live typing with trailing dot
    assert(sanitizeIpv4("192.168.", false) == "192.168.");
    assert(sanitizeIpv4("192.168.", true) == "192.168");

    // Non-digit characters discarded
    assert(sanitizeIpv4("192.abc.168.foo.1.bar.100", true) == "192.168.1.100");
    assert(sanitizeIpv4("  10 . 0 . 0 . 1  ", true) == "10.0.0.1");

    // Max 4 sections (extra sections ignored)
    assert(sanitizeIpv4("192.168.1.1.99.88", true) == "192.168.1.1");

    std::cout << "[ALL TESTS PASSED] IPv4 sanitization is verified!" << std::endl;
    return 0;
}
