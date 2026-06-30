/*  test_account_defaults.cc — Pins the platform-conditional account
 *  defaults: UPnP/port-mapping off on iOS (CMP-021) and the hardwired
 *  Resonator DHT proxy knobs that let a mobile account reach the network
 *  through a proxy instead of a full DHT node. The production call sites in
 *  carrier_jami.cc pass carrier_account::kIosBuild; the suite runs on the
 *  host, so both branches are exercised explicitly here.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include "../src/account_defaults.hpp"

extern "C" {
#include "test.h"
}

TEST(test_ios_build_disables_upnp)
{
    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";

    carrier_account::apply_platform_defaults(details, /*ios_build=*/true);

    ASSERT(details.count("Account.upnpEnabled") == 1);
    ASSERT_STR_EQ(details["Account.upnpEnabled"].c_str(), "false");
    /* Pre-existing keys are untouched. */
    ASSERT_STR_EQ(details["Account.type"].c_str(), "RING");
}

TEST(test_ios_build_enables_dht_proxy)
{
    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";

    carrier_account::apply_platform_defaults(details, /*ios_build=*/true);

    /* Mobile rides a DHT proxy, pointed at our hardwired endpoint, with the
     * list-fetch path disabled so it talks to the proxy directly. */
    ASSERT_STR_EQ(details["Account.proxyEnabled"].c_str(), "true");
    ASSERT_STR_EQ(details["Account.proxyServer"].c_str(),
                  carrier_account::kResonatorProxyServer);
    ASSERT_STR_EQ(details["Account.proxyListEnabled"].c_str(), "false");
    ASSERT_STR_EQ(details["Account.hostname"].c_str(),
                  carrier_account::kResonatorBootstrap);

    /* type(1) + upnp(1) + the 5 DHT knobs = 7 keys, nothing extra. */
    ASSERT_EQ(details.size(), 7u);
}

TEST(test_resonator_dht_defaults_shape)
{
    const auto d = carrier_account::resonator_dht_defaults();
    ASSERT_EQ(d.size(), 5u);
    ASSERT_EQ(d.count("Account.proxyEnabled"), 1u);
    ASSERT_EQ(d.count("Account.proxyServer"), 1u);
    ASSERT_EQ(d.count("Account.proxyListEnabled"), 1u);
    ASSERT_EQ(d.count("Account.dhtProxyListUrl"), 1u);
    ASSERT_EQ(d.count("Account.hostname"), 1u);
}

TEST(test_non_ios_build_leaves_details_untouched)
{
    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";

    carrier_account::apply_platform_defaults(details, /*ios_build=*/false);

    /* Desktop keeps the full-DHT path: no proxy, no upnp override. */
    ASSERT_EQ(details.count("Account.upnpEnabled"), 0u);
    ASSERT_EQ(details.count("Account.proxyEnabled"), 0u);
    ASSERT_EQ(details.size(), 1u);
}

TEST(test_host_build_flag_is_not_ios)
{
    /* The suite runs on macOS/Linux hosts; TARGET_OS_IOS must resolve
     * false here or every host-created account would lose UPnP. */
    ASSERT(carrier_account::kIosBuild == false);
}

extern "C" void test_account_defaults_all(void)
{
    printf("account_defaults:\n");
    RUN_TEST(test_ios_build_disables_upnp);
    RUN_TEST(test_ios_build_enables_dht_proxy);
    RUN_TEST(test_resonator_dht_defaults_shape);
    RUN_TEST(test_non_ios_build_leaves_details_untouched);
    RUN_TEST(test_host_build_flag_is_not_ios);
    printf("\n");
}
