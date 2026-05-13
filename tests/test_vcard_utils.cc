/*  test_vcard_utils.cc — Unit tests for carrier_vcard::extract_vcard_fn,
 *  base64_encode, vcard_path_for_peer, and slurp_file.
 *
 *  These helpers are the basis of the AccountReady ContactName replay
 *  (see ISSUE-103 / carrier_jami_signals.cc::replay_contact_names). The
 *  live replay loop needs libjami running to exercise end-to-end; these
 *  pure unit tests pin the parsing + path-construction primitives that
 *  the loop depends on so a future refactor can't silently break them.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "test.h"
#include "vcard_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using carrier_vcard::base64_encode;
using carrier_vcard::extract_vcard_fn;
using carrier_vcard::slurp_file;
using carrier_vcard::vcard_path_for_peer;

/* ---------------------------------------------------------------------------
 * extract_vcard_fn
 * ---------------------------------------------------------------------------*/

TEST(test_extract_vcard_fn_basic_lf)
{
    const std::string vcard =
        "BEGIN:VCARD\n"
        "FN:bob\n"
        "VERSION:2.1\n"
        "END:VCARD\n";
    const std::string fn = extract_vcard_fn(vcard);
    ASSERT_STR_EQ(fn.c_str(), "bob");
}

TEST(test_extract_vcard_fn_crlf)
{
    const std::string vcard =
        "BEGIN:VCARD\r\n"
        "FN:Alice Liddell\r\n"
        "VERSION:2.1\r\n"
        "END:VCARD\r\n";
    const std::string fn = extract_vcard_fn(vcard);
    ASSERT_STR_EQ(fn.c_str(), "Alice Liddell");
}

TEST(test_extract_vcard_fn_real_jami_shape)
{
    /* Verbatim shape libjami writes to <profiles>/<b64>.vcf (verified
     * empirically against the running messenger2 alice/bob accounts). */
    const std::string vcard =
        "BEGIN:VCARD\n"
        "FN:bob\n"
        "PHOTO;ENCODING=BASE64;TYPE=PNG:\n"
        "VERSION:2.1\n"
        "END:VCARD";
    const std::string fn = extract_vcard_fn(vcard);
    ASSERT_STR_EQ(fn.c_str(), "bob");
}

TEST(test_extract_vcard_fn_missing_returns_empty)
{
    const std::string vcard =
        "BEGIN:VCARD\n"
        "VERSION:2.1\n"
        "END:VCARD\n";
    const std::string fn = extract_vcard_fn(vcard);
    ASSERT(fn.empty());
}

TEST(test_extract_vcard_fn_empty_input_returns_empty)
{
    const std::string fn = extract_vcard_fn("");
    ASSERT(fn.empty());
}

TEST(test_extract_vcard_fn_fn_not_at_line_start_skipped)
{
    /* "FN:" appearing mid-line (e.g. inside a NOTE) must NOT match. */
    const std::string vcard =
        "BEGIN:VCARD\n"
        "NOTE:see FN:elsewhere\n"
        "VERSION:2.1\n"
        "END:VCARD\n";
    const std::string fn = extract_vcard_fn(vcard);
    ASSERT(fn.empty());
}

TEST(test_extract_vcard_fn_empty_value)
{
    /* "FN:" with no value yields an empty string — caller treats as
     * "no display name", same as missing-FN. */
    const std::string vcard =
        "BEGIN:VCARD\n"
        "FN:\n"
        "VERSION:2.1\n"
        "END:VCARD\n";
    const std::string fn = extract_vcard_fn(vcard);
    ASSERT(fn.empty());
}

/* ---------------------------------------------------------------------------
 * base64_encode
 *
 * Vector ground truth: the encoded filenames libjami uses for the cached
 * vCards in messenger2 (verified by listing the running profiles dir).
 * ---------------------------------------------------------------------------*/

TEST(test_base64_encode_bob_uri)
{
    const std::string in  = "4748d985a10c8e84990f592a0ec0232efb733293";
    const std::string out = base64_encode(in);
    ASSERT_STR_EQ(out.c_str(), "NDc0OGQ5ODVhMTBjOGU4NDk5MGY1OTJhMGVjMDIzMmVmYjczMzI5Mw==");
}

TEST(test_base64_encode_alice_uri)
{
    const std::string in  = "08f2ce69bd38bf6c46912396fd6059dd31acd863";
    const std::string out = base64_encode(in);
    ASSERT_STR_EQ(out.c_str(), "MDhmMmNlNjliZDM4YmY2YzQ2OTEyMzk2ZmQ2MDU5ZGQzMWFjZDg2Mw==");
}

TEST(test_base64_encode_empty)
{
    ASSERT_STR_EQ(base64_encode("").c_str(), "");
}

TEST(test_base64_encode_padding_one_byte)
{
    /* RFC 4648 §4 — one input byte → 2 chars + "==". */
    ASSERT_STR_EQ(base64_encode("f").c_str(), "Zg==");
}

TEST(test_base64_encode_padding_two_bytes)
{
    /* RFC 4648 §4 — two input bytes → 3 chars + "=". */
    ASSERT_STR_EQ(base64_encode("fo").c_str(), "Zm8=");
}

TEST(test_base64_encode_no_padding)
{
    /* Three input bytes → no padding. */
    ASSERT_STR_EQ(base64_encode("foo").c_str(), "Zm9v");
}

/* ---------------------------------------------------------------------------
 * vcard_path_for_peer
 *
 * Platform-aware path construction — the macOS branch is what messenger2
 * actually runs against; Linux branch covered for CI parity.
 * ---------------------------------------------------------------------------*/

TEST(test_vcard_path_for_peer_known_layout)
{
    const std::string path = vcard_path_for_peer(
        "/Users/armen/.resonator/messenger/alice",
        "c314a87070bc4c74",
        "4748d985a10c8e84990f592a0ec0232efb733293");
#ifdef __APPLE__
    ASSERT_STR_EQ(path.c_str(),
        "/Users/armen/.resonator/messenger/alice/Library/Application Support/jami/"
        "c314a87070bc4c74/profiles/"
        "NDc0OGQ5ODVhMTBjOGU4NDk5MGY1OTJhMGVjMDIzMmVmYjczMzI5Mw==.vcf");
#else
    ASSERT_STR_EQ(path.c_str(),
        "/Users/armen/.resonator/messenger/alice/jami/"
        "c314a87070bc4c74/profiles/"
        "NDc0OGQ5ODVhMTBjOGU4NDk5MGY1OTJhMGVjMDIzMmVmYjczMzI5Mw==.vcf");
#endif
}

/* ---------------------------------------------------------------------------
 * slurp_file — round-trip a known vCard body via a temp file
 * ---------------------------------------------------------------------------*/

TEST(test_slurp_file_round_trip)
{
    char tmpl[] = "/tmp/carrier_test_vcard_XXXXXX";
    int fd = mkstemp(tmpl);
    ASSERT(fd >= 0);

    const std::string vcard =
        "BEGIN:VCARD\n"
        "FN:test-peer\n"
        "VERSION:2.1\n"
        "END:VCARD\n";
    ssize_t w = write(fd, vcard.data(), vcard.size());
    close(fd);
    ASSERT_EQ(w, static_cast<ssize_t>(vcard.size()));

    const std::string read_back = slurp_file(tmpl);
    unlink(tmpl);

    ASSERT_EQ(read_back.size(), vcard.size());
    ASSERT(read_back == vcard);

    /* Compose the two helpers — what replay_contact_names actually does. */
    ASSERT_STR_EQ(extract_vcard_fn(read_back).c_str(), "test-peer");
}

TEST(test_slurp_file_missing_returns_empty)
{
    const std::string out = slurp_file("/tmp/carrier_test_definitely_does_not_exist_xyz");
    ASSERT(out.empty());
}

/* ---------------------------------------------------------------------------
 * Entry point — registered with test_main.c
 * ---------------------------------------------------------------------------*/

extern "C" void test_vcard_utils_all(void)
{
    printf("\nvcard_utils\n");
    printf("-----------\n");

    RUN_TEST(test_extract_vcard_fn_basic_lf);
    RUN_TEST(test_extract_vcard_fn_crlf);
    RUN_TEST(test_extract_vcard_fn_real_jami_shape);
    RUN_TEST(test_extract_vcard_fn_missing_returns_empty);
    RUN_TEST(test_extract_vcard_fn_empty_input_returns_empty);
    RUN_TEST(test_extract_vcard_fn_fn_not_at_line_start_skipped);
    RUN_TEST(test_extract_vcard_fn_empty_value);

    RUN_TEST(test_base64_encode_bob_uri);
    RUN_TEST(test_base64_encode_alice_uri);
    RUN_TEST(test_base64_encode_empty);
    RUN_TEST(test_base64_encode_padding_one_byte);
    RUN_TEST(test_base64_encode_padding_two_bytes);
    RUN_TEST(test_base64_encode_no_padding);

    RUN_TEST(test_vcard_path_for_peer_known_layout);

    RUN_TEST(test_slurp_file_round_trip);
    RUN_TEST(test_slurp_file_missing_returns_empty);
}
