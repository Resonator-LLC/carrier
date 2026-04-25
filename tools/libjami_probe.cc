// M1 probe: verify libjami links and the init/start/fini cycle runs clean.
//
// Expected flow:
//   libjami::init(flags) → libjami::start() → libjami::fini()
//
// Exit 0 if all three succeed. Prints version/platform/arch for
// grounding the Jami release we're linking against.

#include "jami/jami.h"

#include <cstdio>
#include <cstdlib>

int main() {
    std::printf("libjami probe\n");
    std::printf("  version:  %s\n", libjami::version());
    std::printf("  platform: %.*s\n",
                static_cast<int>(libjami::platform().size()),
                libjami::platform().data());
    std::printf("  arch:     %.*s\n",
                static_cast<int>(libjami::arch().size()),
                libjami::arch().data());

    const auto flags = static_cast<libjami::InitFlag>(
        libjami::LIBJAMI_FLAG_CONSOLE_LOG |
        libjami::LIBJAMI_FLAG_NO_AUTOLOAD);

    if (!libjami::init(flags)) {
        std::fprintf(stderr, "libjami::init failed\n");
        return EXIT_FAILURE;
    }
    std::printf("  init:     ok (initialized=%d)\n",
                static_cast<int>(libjami::initialized()));

    if (!libjami::start()) {
        std::fprintf(stderr, "libjami::start failed\n");
        libjami::fini();
        return EXIT_FAILURE;
    }
    std::printf("  start:    ok\n");

    libjami::fini();
    std::printf("  fini:     ok\n");
    return EXIT_SUCCESS;
}
