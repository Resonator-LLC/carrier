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

/* ---------------------------------------------------------------------------
 * Resonator's hardwired DHT / network endpoints.
 *
 * The Resonator project runs no private DHT of its own yet, so these point at
 * Jami's public infrastructure (see arch/mother-network.md — a self-hosted
 * OpenDHT bootstrap/proxy on mother.resonator.network is the M6+/T-010
 * backlog). When that lands, swap kResonatorProxyServer for the deployed
 * proxy and kResonatorBootstrap for its bootstrap node — nothing else moves.
 *
 * Why a proxy at all: a full DHT node keeps a long-lived UDP socket open,
 * which mobile OSes (iOS especially) reap in the background. A DHT proxy is
 * an HTTP(S) endpoint that performs DHT operations on the client's behalf, so
 * the account can reach the network without sustaining its own node. Desktop
 * keeps the full-DHT path (see apply_platform_defaults — proxy is mobile-only)
 * because it connects fine and a direct node has lower latency.
 * ---------------------------------------------------------------------------*/
/* INTERIM (pending the self-hosted mother.resonator.network proxy). Jami's bare
 * `dhtproxy.jami.net` round-robins across a healthy IP (141.94.96.2) and a DEAD
 * one (141.94.96.254 = dhtproxy1.jami.net, every port closed) — an iOS device
 * that draws the dead node never registers and sits amber. libjami also REQUIRES
 * the port-range form `host:[lo-hi]` (jamiaccount.cpp PROXY_REGEX); a bare
 * hostname yields no port and silently fails. So pin a verified-healthy proxy
 * (dhtproxy2..5 are live; checked via the OpenDHT node-info endpoint) with the
 * range. Swap for "mother.resonator.network:[80-95]" once that proxy is up. */
inline constexpr const char *kResonatorProxyServer     = "dhtproxy2.jami.net:[81-95]";
inline constexpr const char *kResonatorDhtProxyListUrl = "https://config.jami.net/dhtproxyList";
inline constexpr const char *kResonatorBootstrap       = "bootstrap.jami.net";

/* The DHT/proxy knobs carrier hardwires so a mobile account reaches the
 * network through a proxy instead of a full DHT node. Returned as a partial
 * libjami detail map, suitable for both addAccount() (account creation) and
 * setAccountDetails() (runtime reconfigure) — libjami merges partial maps, so
 * untouched keys keep their values.
 *
 * proxyListEnabled is forced false so the account talks to proxyServer
 * directly rather than fetching a proxy list from dhtProxyListUrl (which can
 * 404 / lag); dhtProxyListUrl is still set so a later flip to list mode has a
 * sane target. */
inline std::map<std::string, std::string> resonator_dht_defaults()
{
    return {
        {"Account.proxyEnabled",     "true"},
        {"Account.proxyServer",      kResonatorProxyServer},
        {"Account.proxyListEnabled", "false"},
        {"Account.dhtProxyListUrl",  kResonatorDhtProxyListUrl},
        {"Account.hostname",         kResonatorBootstrap},
    };
}

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
 * (turn.jami.net, on by default).
 *
 * iOS also gets the Resonator DHT proxy defaults (resonator_dht_defaults):
 * a full DHT node can't survive iOS background suspension, so the account
 * would never reach a connected state. Desktop is intentionally left on the
 * full-DHT path. Android is embedded too but compiles with this flag false
 * (no TARGET_OS_IOS); Android accounts opt in via the runtime
 * carrier:SetAccountConfig "resonator" preset instead. */
inline void apply_platform_defaults(std::map<std::string, std::string> &details,
                                    bool ios_build)
{
    if (ios_build) {
        details["Account.upnpEnabled"] = "false";
        for (const auto &[key, value] : resonator_dht_defaults()) {
            details[key] = value;
        }
    }
}

} /* namespace carrier_account */

#endif /* CARRIER_ACCOUNT_DEFAULTS_HPP */
