#include "cpp.h"
using namespace page_elements;

#include "generated/cpp/all.h"
#include <primitives/sw/main.h>
#include <print>

struct mediawiki_consumer {
    using this_type = mediawiki_consumer;

    path root_dir;
    std::string s;
    int last_head;
    int inside_table{};
    int n{};
    bool external_link{};
    bool in_pre{};
    std::string python_uploader{R"(# -*- coding: utf-8 -*-

from wikiapi import *

with ThreadPoolExecutor(max_workers=1) as executor:
)"};

    ~mediawiki_consumer() {
        write_file("wikiapi_pages.py", python_uploader);
    }
    void render(auto &page) {
        std::println("[{}] {}", ++n, page.title);
        auto t = page.title;
        boost::replace_all(t, "<", "_lt");
        boost::replace_all(t, ">", "_gt");
        boost::replace_all(t, "[", "_lsq");
        boost::replace_all(t, "]", "_gsq");
        boost::replace_all(t, "\n", "");
        boost::replace_all(t, "\r", "");
        auto fn = root_dir / page.filename;
        fn += ".txt";
        python_uploader += std::format("    executor.submit(make_page, {}, '{}', '{}')\n", n, page.filename, normalize_path(fn).string());
        page.render(*this);
        write_file(fn, s);
        s.clear();
    }
    this_type &operator<<(paragraph &&) {
        s += "\n";
        return *this;
    }
    this_type &operator<<(header &&v) {
        last_head = v.level;
        if (inside_table) {
            s += std::format("<span class=\"mw-heading mw-heading{}\">", v.level);
        } else {
            s += std::string(v.level, '=') + " ";
        }
        return *this;
    }
    this_type &operator<<(header_end &&v) {
        if (inside_table) {
            s += std::format("</span>");
        } else {
            s += " " + std::string(last_head, '=') + "\n";
        }
        return *this;
    }
    this_type &operator<<(table &&v) {
        s += "\n{|";
        ++inside_table;
        return *this;
    }
    this_type &operator<<(next_row &&v) {
        s += "\n|-";
        if (v.rowspan) {
            s += std::format("rowspan=\"{}\" | ", v.rowspan);
        }
        return *this;
    }
    this_type &operator<<(next_col &&v) {
        s += "\n| ";
        if (v.colspan) {
            s += std::format("colspan=\"{}\" | ", v.colspan);
        }
        return *this;
    }
    this_type &operator<<(table_end &&v) {
        s += "\n|}\n\n";
        --inside_table;
        return *this;
    }
    this_type &operator<<(link &&v) {
        external_link = v.value.starts_with("http"sv);
        s += external_link ? std::format("[{} ", v.value) : std::format("[[{}|", v.value);
        return *this;
    }
    this_type &operator<<(link_end &&v) {
        s += external_link ? std::format("]") : std::format("]]");
        return *this;
    }
    this_type &operator<<(code &&v) {
        s += std::format("\n<syntaxhighlight lang=\"cpp\">\n");
        return *this;
    }
    this_type &operator<<(code_end &&v) {
        s += std::format("\n</syntaxhighlight>\n\n");
        return *this;
    }
    this_type &operator<<(code_tag &&v) {
        s += std::format("<code>");
        return *this;
    }
    this_type &operator<<(code_tag_end &&v) {
        s += std::format("</code>");
        return *this;
    }
    this_type &operator<<(br &&v) {
        s += std::format("<br>");
        return *this;
    }
    template <auto N>
    this_type &operator<<(const char (&v)[N]) {
        s += v;
        return *this;
    }
    this_type &operator<<(auto &&p) {
        int a = 5;
        a++;
        return *this;
    }
};

int main(int argc, char *argv[]) {
    mediawiki_consumer mw{"generated/mediawiki"};
    pages a;
    // we can use reflection to do for each all members
    a.render(mw);
    return 0;
}
