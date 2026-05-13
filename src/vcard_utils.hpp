/*  vcard_utils.hpp — Pure helpers for parsing vCards and locating libjami's
 *  cached profile files. Factored out of carrier_jami_signals.cc so the
 *  unit-test suite can exercise them without dragging in libjami.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_VCARD_UTILS_HPP
#define CARRIER_VCARD_UTILS_HPP

#include <cstddef>
#include <cstdio>
#include <string>

namespace carrier_vcard {

/* Read the whole file at `path` into a string. Returns empty on miss/empty. */
inline std::string slurp_file(const std::string &path)
{
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            out.append(buf, n);
        }
        std::fclose(f);
    }
    return out;
}

/* Extract the `FN:` line value from a vCard body. vCards use CRLF or LF;
 * we accept both. Returns empty if no FN line is present. Folded
 * continuation lines (RFC 6350 §3.2, leading whitespace) are uncommon for
 * FN — skip support unless a real-world vCard triggers a bug. */
inline std::string extract_vcard_fn(const std::string &vcard)
{
    const std::string needle = "FN:";
    std::size_t pos = 0;
    while (pos < vcard.size()) {
        std::size_t eol = vcard.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = vcard.size();
        if (eol - pos >= needle.size() &&
            vcard.compare(pos, needle.size(), needle) == 0) {
            return vcard.substr(pos + needle.size(), eol - pos - needle.size());
        }
        pos = (eol < vcard.size() && vcard[eol] == '\r' &&
               eol + 1 < vcard.size() && vcard[eol + 1] == '\n')
                  ? eol + 2
                  : eol + 1;
    }
    return {};
}

/* Standard base64 encoder (alphabet A-Za-z0-9+/=). libjami stores cached
 * vCards at <profiles_dir>/<base64(peer_uri)>.vcf — the encoding is plain
 * (not URL-safe) base64 of the URI's ASCII bytes. */
inline std::string base64_encode(const std::string &in)
{
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned a = static_cast<unsigned char>(in[i]);
        unsigned b = static_cast<unsigned char>(in[i + 1]);
        unsigned c = static_cast<unsigned char>(in[i + 2]);
        out.push_back(alpha[(a >> 2) & 0x3f]);
        out.push_back(alpha[((a << 4) | (b >> 4)) & 0x3f]);
        out.push_back(alpha[((b << 2) | (c >> 6)) & 0x3f]);
        out.push_back(alpha[c & 0x3f]);
        i += 3;
    }
    if (i < in.size()) {
        unsigned a = static_cast<unsigned char>(in[i]);
        unsigned b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0;
        out.push_back(alpha[(a >> 2) & 0x3f]);
        out.push_back(alpha[((a << 4) | (b >> 4)) & 0x3f]);
        if (i + 1 < in.size()) {
            out.push_back(alpha[(b << 2) & 0x3f]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

/* Build the on-disk path of libjami's cached vCard for `peer_uri` under
 * the account's data tree. Layout (verified empirically against libjami
 * 2a7ef0b):
 *   macOS:  <data_dir>/Library/Application Support/jami/<account>/profiles/<b64>.vcf
 *   Linux:  <data_dir>/jami/<account>/profiles/<b64>.vcf
 * The platform fork mirrors libjami's get_data_dir() (D11 — see
 * carrier_new in carrier_jami.cc): macOS hard-codes
 * $HOME/Library/Application Support; Linux honors XDG_DATA_HOME. We set
 * both env vars to `data_dir` in carrier_new, so the path resolves
 * deterministically. */
inline std::string vcard_path_for_peer(const std::string &data_dir,
                                       const std::string &account_id,
                                       const std::string &peer_uri)
{
#ifdef __APPLE__
    const std::string root = data_dir + "/Library/Application Support/jami/";
#else
    const std::string root = data_dir + "/jami/";
#endif
    return root + account_id + "/profiles/" + base64_encode(peer_uri) + ".vcf";
}

} /* namespace carrier_vcard */

#endif /* CARRIER_VCARD_UTILS_HPP */
