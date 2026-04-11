#include "cpp.h"
using namespace page_elements;

#include "data/all.h"

#include <primitives/sw/main.h>

struct mediawiki_consumer {
    using this_type = mediawiki_consumer;

    void render(auto &page) {
        *this << page;
    }
    this_type &operator<<(const page &) {
        return *this;
    }
    this_type &operator<<(const title &) {
        return *this;
    }
    this_type &operator<<(const auto &) {
        return *this;
    }
};

int main(int argc, char *argv[]) {
    mediawiki_consumer mw;
    pages a;
    // we can use reflection to do for each all members
    a.render(mw);
    return 0;
}
