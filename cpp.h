// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2024-2026 Egor Pugin <egor.pugin@gmail.com>

#pragma once

#include <primitives/string.h>
#include <primitives/filesystem.h>

#include <format>
#include <string>
#include <variant>

using namespace std::literals;

namespace cpp_reference {

enum class language {
    c,
    cpp
};

enum class c_standard {
    c99,
    c11,
};

enum class cpp_standard {
    cpp99,
    cpp03,
    cpp11,
    cpp14,
    cpp17,
    cpp20,
    cpp23,
    cpp26,
};

enum class language_standard {
    c99,
    c11,

    cpp99,
    cpp03,
    cpp11,
    cpp14,
    cpp17,
    cpp20,
    cpp23,
    cpp26,
};

enum class object_type {
    header,
    class_,
    class_template,
};

template <typename T>
struct parsed_object {
    //void parse();

    static bool is(const std::string &url) {
        auto page_id = split_string(url, "/");
        //auto lang = page_id.at(0);
        return page_id.size() > 1 && page_id[1] == T::page_id;
    }
};

struct navbar {
    struct table {

    };

    //std::vector<table*> tables;

    struct item {
        std::string name;
        std::string href;
    };
    std::vector<item> items; // simple
};
struct page_raw {
    struct header {
        std::string value;
    };
    struct text {
        std::string value;
    };
    struct table {

    };
    using paragraph = std::variant<header, text, table>;
    struct declarations_type {
        struct decl {
            std::string d;
            int number{};
            std::vector<std::string> standards;
        };
        struct decl_list {
            std::string section_head;
            std::vector<decl> decls;

            bool empty() const {return section_head.empty() && decls.empty();}
            auto &back() {
                return decls.empty() ? decls.emplace_back() : decls.back();
            }
        };
        std::string head;
        std::vector<decl_list> decls;

        auto &back() {
            return decls.empty() ? decls.emplace_back() : decls.back();
        }
    };

    //location l;
    std::string name;
    std::string title;
    std::vector<navbar> navbars;
    declarations_type declarations;

    std::vector<std::string> all_text;
};

// non language entity
struct page {
    std::string title;

    static bool is(const std::string &url) {
        return !url.contains('/');
    }
};

struct c_language_standard_page {
    static bool is(const std::string &url) {
        auto page_id = split_string(url, "/");
        return page_id.size() == 2
            && page_id[0] == "c"sv
            && (std::ranges::all_of(page_id[1], isdigit) || page_id[1] == "current_status"sv)
            ;
    }
};

struct compiler_support : parsed_object<compiler_support> {
    static inline constexpr auto page_id = "compiler_support"sv;

    auto title() const {return std::format("Compiler support for C++11");}
};


} // namespace cpp_reference

template <typename ... Types>
struct type_list {
    using variant_type = std::variant<Types...>;

    static void for_each(auto &&f) {
        (f((Types**)nullptr),...);
    }
};

struct cppreference_website {
    std::map<std::string, cpp_reference::page_raw> pages;

    auto print_raw() const {
        std::string s;
        for (auto &&[_, p] : pages) {
            for (auto &&t : p.all_text) {
                s += std::format("{}\n\n", t);
            }
        }
        return s;
    }
    auto print_latex() const {
        auto dblnl = "\n\n"s;

        auto make_tex_string = [](auto &&in) {
            std::string s{ in };
            boost::replace_all(s, "\\", "\\textbackslash");
            boost::replace_all(s, "_", "\\_");
            boost::replace_all(s, "$", "\\$");
            boost::replace_all(s, "&", "\\&");
            boost::replace_all(s, "^", "\\^");
            boost::replace_all(s, "#", "\\#");
            boost::replace_all(s, "{", "\\{");
            boost::replace_all(s, "}", "\\}");
            boost::replace_all(s, "%", "\\%");
            return s;
            };
        auto tex_command = [&](auto &&n, auto &&...args) {
            auto s = std::format("\\{}", make_tex_string(n));
            ((s += std::format("{{{}}}", make_tex_string(args))),...);
            return s;
            };
        auto begin = [&](auto &&n) {
            auto s = tex_command("begin", n);
            return s;
            };
        auto end = [&](auto &&n) {
            auto s = tex_command("end", n);
            return s;
            };
        auto newpage = [&]() {
            auto s = tex_command("newpage") + dblnl;
            return s;
            };

        std::string s;
        s += tex_command("documentclass"sv, "article"sv) + dblnl;
        s += begin("document") + dblnl;
        s += tex_command("tableofcontents") + dblnl;
        s += newpage();
        for (int i{}; auto &&[_, p] : pages) {
            auto fn = std::format("gen/{:06}.tex", i++);
            {
            std::string s;
            s += tex_command("section", p.title) + dblnl;
            s += make_tex_string(p.declarations.head) + dblnl;
            for (auto &&d : p.declarations.decls) {
                s += tex_command("textbf", d.section_head) + dblnl;
                for (auto &&d : d.decls) {
                    s += tex_command("texttt"sv, d.d) + dblnl;
                }
            }
            for (auto &&t : p.all_text) {
                s += std::format("{}", make_tex_string(t)) + dblnl;
            }
            s += newpage();
            write_file(fn, s);
            }
            s += tex_command("input", fn) + dblnl;
        }
        s += end("document") + "\n";
        return s;
    }
    auto print_json() const {
        // use glaze lib
        std::string s;
        return s;
    }
};

namespace page_elements {

struct page {
    std::string value;
};
struct title {
    std::string value;
};
struct header {
    int level;
};
struct header_end {};
struct link {
    std::string value;
};
struct link_end{};

struct code{};
struct code_end {};

struct code_tag {};
struct code_tag_end {};

struct table{};
struct table_end{};
struct next_row {
    int rowspan;
};
struct next_col {
    int colspan;
};

struct paragraph{};
struct cite {};

struct bold {};
struct monospace {};
struct italic {};
#undef small
struct small {};
struct abbr {};

struct ul {};
struct ol {};
struct li {};

struct dl {};
struct dd {};
struct dt {};

struct blockquote {};
struct img {};
struct caption {};
struct sub {};
struct sup {};

struct template_ {};
struct br {};

} // namespace page_elements
