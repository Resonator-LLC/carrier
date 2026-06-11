/*  account_defaults.hpp — Platform-conditional defaults carrier bakes into
 *  every libjami account it creates. Factored out of carrier_jami.cc so the
 *  unit-test suite can exercise the logic without dragging in libjami.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_ACCOUNT_DEFAULTS_HPP
#define CARRIER_ACCOUNT_DEFAULTS_HPP

#include <map>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace carrier_account {

/* Compile-time platform flag fed to apply_platform_defaults() by the
 * production call sites; tests pass both values explicitly. */
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
inline constexpr bool kIosBuild = true;
#else
inline constexpr bool kIosBuild = false;
#endif

/* Mutate `details` (the map handed to libjami::addAccount) with the
 * defaults carrier enforces per platform.
 *
 * iOS: the port-mapping subsystem is disabled (CMP-021 decision,
 * 2026-06-11). libjami defaults Account.upnpEnabled to true, which makes
 * dhtnet's PUPnP protocol emit SSDP multicast to 239.255.255.250:1900 —
 * on iOS that requires the Apple-granted
 * com.apple.developer.networking.multicast entitlement, without which the
 * discovery silently fails. Setting the knob false means libjami never
 * creates the account's upnp::Controller (account.cpp:88), so neither
 * SSDP multicast nor NAT-PMP gateway probing happens; NAT traversal
 * falls back to ICE + DHT hole punching + the TURN relay
 * (turn.jami.net, on by default). */
inline void apply_platform_defaults(std::map<std::string, std::string> &details,
                                    bool ios_build)
{
    if (ios_build) {
        details["Account.upnpEnabled"] = "false";
    }
}

} /* namespace carrier_account */

#endif /* CARRIER_ACCOUNT_DEFAULTS_HPP */
