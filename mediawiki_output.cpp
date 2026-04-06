#include "cpp.h"
using namespace page_elements;

#include "data/all.h"

#include <primitives/sw/main.h>

struct mediawiki_consumer {
    using this_type = mediawiki_consumer;

    this_type &operator<<(const page &) {
        return *this;
    }
    this_type &operator<<(const title &) {
        return *this;
    }
};

int main(int argc, char *argv[]) {
    mediawiki_consumer mw;
    f(mw);
    return 0;
}
