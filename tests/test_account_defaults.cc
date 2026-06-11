/*  test_account_defaults.cc — Pins the platform-conditional account
 *  defaults (CMP-021: UPnP/port-mapping off on iOS). The production call
 *  sites in carrier_jami.cc pass carrier_account::kIosBuild; the suite
 *  runs on the host, so both branches are exercised explicitly here.
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
    /* Only the upnp knob is added — nothing else in the map moves. */
    ASSERT_EQ(details.size(), 2u);
    ASSERT_STR_EQ(details["Account.type"].c_str(), "RING");
}

TEST(test_non_ios_build_leaves_details_untouched)
{
    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";

    carrier_account::apply_platform_defaults(details, /*ios_build=*/false);

    ASSERT_EQ(details.count("Account.upnpEnabled"), 0u);
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
    RUN_TEST(test_non_ios_build_leaves_details_untouched);
    RUN_TEST(test_host_build_flag_is_not_ios);
    printf("\n");
}
