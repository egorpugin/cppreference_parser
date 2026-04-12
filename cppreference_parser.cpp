// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2024-2026 Egor Pugin <egor.pugin@gmail.com>

// also see https://github.com/PeterFeicht/cppreference-doc

//#include "cpp.h"

//#include <primitives/emitter.h>
#include <primitives/executor.h>
#include <primitives/http.h>
#include <primitives/sw/main.h>
#include <primitives/templates2/sqlite.h>
//#include <primitives/templates2/xml.h>
#include <primitives/templates2/html.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <print>
#include <ranges>
#include <syncstream>
#include <variant>

// find all templates in data dir
// grep "=Template:\K.*(?=&)" -r . -o -P -h | sort | uniq

namespace db::parser {

using namespace primitives::sqlite::db;

struct schema {
    struct tables_ {
        struct page {
            type<int64_t, primary_key{}, autoincrement{}> page_id;
            type<std::string, unique{}> name;
            type<std::string> source;
        } page_;
    } tables;
};

} // namespace db::parser

struct url_request_cache : primitives::sqlite::kv<std::string, std::string> {};
static auto &cache() {
    thread_local auto c = []() {
        primitives::sqlite::cache <
            url_request_cache
        > c{ "cache.db" };
        c.enable_wal();
        c.set_busy_timeout(5s);
        return c;
        }();
    return c;
}

const path mirror_root_dir = "cppreference";

auto url_base = "cppreference.com"s;
auto lang = "en"s;
auto protocol = "https"s;
auto normal_page = "w"s;
auto start_page = "https://en.cppreference.com/w/Main_Page.html"s;

auto make_base_url() {
    return std::format("{}://{}.{}", protocol, lang, url_base);
}
auto make_normal_page_url(auto &&page) {
    if (page.starts_with("http")) {
        return page;
    }
    return std::format("{}/{}/{}", make_base_url(), normal_page, primitives::http::url_encode(page));
}

auto download_url(auto &&url) {
    return cache().find<url_request_cache>(url, [&]() {
        std::osyncstream{ std::cout } << std::format("downloading {}\n", url);

        HttpRequest req{ httpSettings };
        req.url = url;
        req.timeout = 90;
        auto resp = url_request(req);
        if (resp.http_code != 200) {
            throw std::runtime_error{ std::format("url = {}, http code = {}", url, resp.http_code) };
        }
        return resp.response;
    });
}

struct page {
    std::string url;
    std::string source;
    std::set<std::string> links;

    page() = default;
    page(const std::string &url) : url{url} {
        source = download_url(url);
        parse_links();
    }
    bool is_c_page() const {
        return url.contains("/w/c/"sv) || url.ends_with("/w/c"sv);
    }
    bool is_cpp_page() const {
        return url.contains("/w/cpp/"sv) || url.ends_with("/w/cpp"sv);
    }
    void parse_links() {
        primitives::html::root r{source};
        for (auto &&n : r | std::views::filter([](auto &&n){return n.is("a"sv) && n.has("href"sv);})) {
            auto a = n.attribute("href");
            std::string l{*a};
            if (l.starts_with("http"sv)) {
                continue;
            }
            if (l.starts_with('/')) {
                continue;
            }
            l = l.substr(0, l.find('#')); // take everything before '#'
            l = l.substr(0, l.find('?')); // take everything before '?'
            path u{url};
            if (!l.starts_with("http"sv) && !l.starts_with("../"sv)) {
                u = u.parent_path();
            }
            if (l.starts_with("../"sv)) {
                u = u.parent_path();
            }
            path p = u / l;
            p = normalize_path(p);
            p = p.lexically_normal();
            p = normalize_path(p);
            l = p.string();
            if (auto p = l.find("http"sv); p != -1)
                l = l.substr(p);
            if (l.starts_with("https:/"sv)) {
                l = "https://" + l.substr(7);
            }
            links.insert(l);
        }
    }
};

struct parser {
    primitives::sqlite::sqlitemgr db{path{ mirror_root_dir } += ".db"};
    //primitives::sqlite::sqlitemgr db{path{ mirror_root_dir } += "_03.2026.db"};
    std::map<std::string, page> pages;
    std::set<std::string> processed_pages; // including bad pages

    parser() {
        db.create_tables(::db::parser::schema{});
        db.enable_wal();
        db.set_busy_timeout(5s);
    }
    void start() {
        Executor e{10};
        std::mutex m;
        std::set<std::string> pages_to_load;
        pages_to_load.insert(start_page);
        while (1) {
            for (auto &&p : pages_to_load) {
                e.push([&]() {
                auto pp = parse_page(p, m);
                if (pp.url.empty()) {
                    return;
                }
                std::unique_lock lk{m};
                pages.emplace(pp.url, pp);
                processed_pages.insert(pp.url);
                    });
            }
            e.wait();
            decltype(pages_to_load) links;
            for (auto &&p : pages_to_load) {
                links.insert_range(pages[p].links);
            }
            std::erase_if(links, [&](auto &&t) {
                return processed_pages.contains(t)
                    || t.starts_with("https://en.cppreference.com/w/Cppreference"sv)
                    || t.starts_with("https://en.cppreference.com/w/Talk"sv)
                    || t.starts_with("https://en.cppreference.com/w/Category"sv)
                    || t.starts_with("https://en.cppreference.com/w/File"sv)
                    || t.starts_with("https://en.cppreference.com/w/MediaWiki"sv)
                    || t.starts_with("https://en.cppreference.com/w/User"sv)
                    || t.starts_with("https://en.cppreference.com/w/c/ftp:"sv)
                    ;
                });
            pages_to_load = links;
            if (pages_to_load.empty()) {
                break;
            }
        }
    }
    page parse_page(auto &&pagename, auto &&m) {
        page p;
        auto db_page_sel = db.select<::db::parser::schema::tables_::page, &::db::parser::schema::tables_::page::name>(pagename);
        auto db_page_i = db_page_sel.begin();
        if (db_page_i != db_page_sel.end()) {
            p.url = make_normal_page_url(pagename);
            auto &db_p = *db_page_i;
            p.source = db_p.source;
            p.parse_links();
        } else {
            try {
                p = page{ make_normal_page_url(pagename) };
                std::unique_lock lk{ m };
                auto tr = db.scoped_transaction();
                auto page_ins = db.prepared_insert < ::db::parser::schema::tables_::page, primitives::sqlite::db::or_ignore{} > ();
                page_ins.insert({ .name = pagename, .source = p.source });
            } catch (std::exception &e) {
                std::cerr << e.what() << "\n";
            }
        }
        return p;
    }
};

void parse() {
    parser p;
    p.start();
}

// FIXME: use traverse and ignore ignored classes
std::string extract_text3(auto &&n, const std::string &delim = ""s) {
    std::string s;
    for (auto &&c : n) {
        if (c.type == primitives::html::token_type::text) {
            s += c.value() + delim;
        }
    }
    //if (s.size() > 5000) s.resize(5000);
    //boost::trim(s);
    return s;
}

// FIXME?: use traverse and ignore ignored classes?
std::string extract_as_is(auto &&n) {
    return n.raw();
}

auto get_classes(auto &&n) {
    std::string_view cl = n.attribute("class").as_string();
    return std::set{ std::from_range, cl | std::views::split(' ') | std::views::transform([](auto &&v) {return std::string_view{v}; }) };
}

struct html_page {
    primitives::html::root root;

    html_page(const std::string &p) : root{p} {
    }
    static auto find_node(auto &&n, auto &&attrname, auto &&idname) {
        return n.find(attrname, idname);
    }
    auto find_node(auto &&attrname, auto &&idname) {
        return find_node(root, attrname, idname);
    }
    std::string value(auto &&attrname, auto &&idname) {
        auto n = find_node(attrname, idname);
        return n ? n->text() : std::string{};
    }

    /*

    void parse_navbar(cpp_reference::page_raw &p, auto &&n) {
        for (auto &&hn : find_navbar_heads(n)) {
            auto &n = p.navbars.emplace_back();
            if (auto nbm = find_navbar_menu(hn.node())) {
                for (auto &&r : find_navbar_rows(nbm.node())) {
                    auto f = [&](this auto &&f, auto &&r) -> void {
                        constexpr auto usual_row = "t-nv"sv;
                        constexpr auto subtable_heading1 = "t-nv-h1"sv;
                        constexpr auto subtable_heading2 = "t-nv-h2"sv;
                        constexpr auto subtable = "t-nv-col-table"sv;
                        auto extract = [&](auto &&o, auto &&a) {
                            o.name = extract_text2(a);
                            o.href = a.attribute("href").as_string();
                            };
                        auto add_link_from_td = [&](auto &&r) {
                            for (auto &&s : find_navbar_cols(r)) {
                                if (auto a = find_a(s.node())) {
                                    extract(n.items.emplace_back(), a.node());
                                } else if (auto a = find_strong(s.node())) {
                                    n.items.emplace_back(extract_text2(a.node()));
                                } else {
                                    continue;
                                }
                            }
                            };
                        std::string_view c = r.node().attribute("class").as_string();
                        if (c == usual_row) {
                            add_link_from_td(r.node());
                        } else if (c == subtable_heading1) {
                            if (auto a = find_a(r.node())) {
                                extract(n.items.emplace_back(), a.node());
                            } else {
                                add_link_from_td(r.node());
                            }
                        } else if (c == subtable_heading2) {
                            if (auto a = find_a(r.node())) {
                                extract(n.items.emplace_back(), a.node());
                            } else {
                                add_link_from_td(r.node());
                            }
                        } else if (c == subtable) {
                            for (auto &&r2 : find_navbar_rows2(r.node())) {
                                f(r2);
                            }
                        } else {
                            return;
                        }
                        };
                    f(r);
                }
            }
        }
    }
    void parse_table_decl(cpp_reference::page_raw &p, auto &&n) {
        std::string s;
        for (auto &&tr : find_table_rows(n)) {
            auto cls = get_classes(tr.node());
            if (0) {
            } else if (cls.contains("t-dsc-header"sv)) { // top header
                p.declarations.head = extract_text2(tr.node());
            } else if (cls.contains("t-dcl-h"sv)) { // intermediate header
                auto &b = p.declarations.decls.emplace_back();
                b.section_head = extract_text2(tr.node());
            } else if (cls.contains("t-dcl"sv)) {
                auto &b = p.declarations.back().decls.emplace_back();
                for (auto &&cl : cls) {
                    if (cl.starts_with("t-until"sv)) {
                        b.standards.emplace_back(cl);
                    } else if (cl.starts_with("t-since"sv)) {
                        b.standards.emplace_back(cl);
                    }
                }
                auto decl = tr.node().select_node("./td[1]");
                b.d = extract_text2(decl.node());
            } else if (cls.contains("t-dcl-rev-aux"sv)) {
                continue;
            }
            s += extract_text2(tr.node()) + "\n";
        }
        p.all_text.emplace_back(s);
    }*/
};

struct cpp_emitter {
    struct line {
        std::variant<std::string, cpp_emitter*> text;

        std::string get_text() const {
            std::string s;
            std::visit(overload(
                [&](const std::string &s2) {
                    s += s2;
                },
                [&](const cpp_emitter *e) {
                    s += e->get_text();
                }), text);
            return s;
        }
    };

    int indent{};
    std::string space{"    "};
    std::string newline{"\n"};
    std::vector<line> lines;

    cpp_emitter &create_inline_emitter() {
        return *std::get<cpp_emitter*>(lines.emplace_back(new cpp_emitter{indent, space, newline}).text);
    }
    void add_line(std::string s = ""s) {
        if (!s.empty()) {
            auto n = indent;
            while (n--) s = space + s;
        }
        lines.emplace_back(std::move(s));
    }
    auto format(auto &&f, auto &&...args) {
        add_line(std::vformat(f, std::make_format_args(args...)));
    }
    void add_type(auto &&n, auto &&...args) {
        if (sizeof...(args)) {
            format("c << {};", std::vformat(n, std::make_format_args(args...)));
        } else {
            format("c << {};", n);
        }
    }
    void add_header(int level) {
        //format("auto &&t = c.add<header>{{{}}};", level);
        format("c << header{{{}}};", level);
    }
    void add_text(std::string_view t) {
        if (!t.empty()) {
            format("c << R\"xxx({})xxx\";", t);
        }
    }
    std::string get_text() const {
        std::string s;
        for (auto &&l : lines) {
            s += l.get_text() + "\n";
        }
        return s;
    }

    void increase_indent(int i = 1) {
        indent += i;
        //if (indent < 0) indent = 0;
    }
    void decrease_indent(int i = -1) {increase_indent(i);}

    void begin_block(const std::string &s = {}, bool inc_indent = true) {
        if (!s.empty())
            add_line(s);
        if (inc_indent)
            increase_indent();
    }
    void end_block(bool semicolon = false) {
        decrease_indent();
        add_line(semicolon ? "};" : "}");
    }
    void begin_function(const std::string &s = "") {
        begin_block(s);
    }
    void end_function() {
        end_block();
        add_line();
    }
    void begin_namespace(const std::string &s) {
        add_line("namespace " + s + " {");
        add_line();
    }
    void end_namespace(const std::string &ns = {}) {
        add_line("} // namespace " + ns);
        add_line();
    }
};

struct cpp_traverser {
    enum class state_type {
        not_set,

        navbar,
        navbar_head,
        navbar_sep,
        navbar_menu,
        navbar_menu_elements_begin, // <table> tag
        navbar_menu_element,
        navbar_menu_element_header1, // table col subhead (slightly indented)
        navbar_menu_element_header2, // table col subhead (slightly indented)
        navbar_menu_element_column_table,
        navbar_menu_element_inline_table, // inside col table

        t_rev_begin, // rev table
        revision,
        revision_inline,
        revision_inline_noborder,
        mark_revision,
        mark_object_type,

        noprint,
        editsection,
        ignore,
        selflink, // bold text (reference) to this page
        mw_headline, // (sub)title line
        lines, // just some text?
        wikitable,
        source_cpp,
        small_text, // _in_table?

        ambox, // some boxes like ambox-notice
        mbox_text,
        mbox_empty_cell,

        example,
        example_live_link,

        mw_geshi, // mediawiki highlight (code)

        lc, // monofont or link? or cite like cell?

        // usual table
        t_dsc_begin,
        t_dsc_header, // usually defined in <>
        t_dsc, // row
        t_dsc_member_div, // cell

        // declarations table
        t_dcl_begin,
        t_dcl, // row
        t_dcl_nopad, // row (no padding?)
        t_dcl_sep,
        t_dcl_h, // header row

        t_image,
        image,

        // defect reports? compact table or maybe no style table
        dsctable,

        t_ref_std_cpp_98,
        t_ref_std_cpp_03,
        t_ref_std_cpp_11,
        t_ref_std_cpp_14,
        t_ref_std_cpp_17,
        t_ref_std_cpp_20,
        t_ref_std_cpp_23,
        t_ref_std_cpp_26,
        t_ref_std_cpp_29,

        t_since_cpp11,
        t_since_cpp14,
        t_since_cpp17,
        t_since_cpp20,
        t_since_cpp23,
        t_since_cpp26,

        t_until_cpp11,
        t_until_cpp14,
        t_until_cpp17,
        t_until_cpp20,
        t_until_cpp23,
        t_until_cpp26,

        t_ref_std_c89,
        t_ref_std_c99,
        t_ref_std_11,
        t_ref_std_17,
        t_ref_std_23,

        t_since_c95,
        t_since_c99,
        t_since_c11,
        t_since_c17,
        t_since_c23,

        t_until_c95,
        t_until_c99,
        t_until_c11,
        t_until_c17,
        t_until_c23,

        t_member,
        t_page_template,

        table_yes, // green
        table_no, // red
        table_maybe, // yellow
        table_na, // gray
    };
    enum class action_type {
        process,
        text,
        ignore,
    };
    struct state_desc {
        state_type t{};
        action_type a{ action_type::process };
    };
    struct state : state_desc {
        int d;
        const primitives::html::node *n;
    };

    cpp_emitter &e;
    std::vector<state> st;
    std::map<std::string, state_desc> known_classes;

    cpp_traverser(cpp_emitter &e) : e{e} {
        {
        known_classes["t-navbar"] = {state_type::navbar};
        known_classes["t-navbar-head"] = {state_type::navbar_head};
        known_classes["t-navbar-menu"] = {state_type::navbar_menu};
        known_classes["t-navbar-sep"] = {state_type::navbar_sep};
        known_classes["t-nv"] = {state_type::navbar_menu_element};
        known_classes["t-nv-begin"] = {state_type::navbar_menu_elements_begin};
        known_classes["t-nv-h1"] = {state_type::navbar_menu_element_header1};
        known_classes["t-nv-h2"] = {state_type::navbar_menu_element_header2};
        known_classes["t-nv-col-table"] = {state_type::navbar_menu_element_column_table};
        known_classes["t-nv-ln-table"] = {state_type::navbar_menu_element_inline_table};

        known_classes["t-rev-begin"] = { state_type::t_rev_begin };
        known_classes["t-rev"] = {state_type::revision}; // double check
        known_classes["t-rev-inl"] = {state_type::revision_inline}; // double check
        known_classes["t-rev-inl-noborder"] = {state_type::revision_inline_noborder}; // double check
        known_classes["t-mark-rev"] = {state_type::mark_revision};
        known_classes["t-mark"] = {state_type::mark_object_type};

        known_classes["t-image"] = {state_type::t_image};
        known_classes["image"] = {state_type::image};

        known_classes["t-dsc-small"] = {state_type::small_text};

        // begin description table
        known_classes["t-dsc-begin"] = {state_type::t_dsc_begin};
        known_classes["t-dsc-header"] = {state_type::t_dsc_header};
        known_classes["t-dsc"] = {state_type::t_dsc};
        known_classes["t-dsc-member-div"] = {state_type::t_dsc_member_div};
        known_classes["t-dsc-hitem"] = {};
        known_classes["t-dsc-named-req-div"] = {};
        known_classes["t-dsc-see"] = {};
        known_classes["t-dsc-see-tt"] = {};
        known_classes["t-dsc-sep"] = {};

        known_classes["t-dcl-begin"] = { state_type::t_dcl_begin };
        known_classes["t-dcl"] = { state_type::t_dcl };
        known_classes["t-dcl-nopad"] = { state_type::t_dcl_nopad };
        known_classes["t-dcl-sep"] = { state_type::t_dcl_sep };
        known_classes["t-dcl-h"] = { state_type::t_dcl_h };
        known_classes["t-dcl-rev"] = {};
        known_classes["t-dcl-rev-aux"] = {};

        known_classes["noprint"] = {state_type::noprint, action_type::ignore};
        known_classes["toc"] = { state_type::noprint, action_type::ignore };
        known_classes["editsection"] = { state_type::editsection, action_type::ignore };
        known_classes["selflink"] = { state_type::selflink };
        known_classes["mw-headline"] = { state_type::mw_headline };
        known_classes["wikitable"] = { state_type::wikitable };
        known_classes["dsctable"] = { state_type::dsctable };

        known_classes["t-lines"] = { state_type::lines };

        known_classes["t-example"] = { state_type::example };
        known_classes["t-example-live-link"] = { state_type::example_live_link };

        known_classes["mw-geshi"] = { state_type::mw_geshi };
        known_classes["source-cpp"] = { state_type::source_cpp };

        known_classes["ambox"] = { state_type::ambox };
        known_classes["mbox-empty-cell"] = { state_type::mbox_empty_cell };
        known_classes["mbox-text"] = { state_type::mbox_text };

        known_classes["t-lc"] = { state_type::lc };

        // ignored stuff
        known_classes["external"] = {};
        known_classes["t-inheritance-diagram"] = { state_type::ignore, action_type::ignore };
        //parse_navbar(p, n);

        known_classes["t-ref-std-c++98"] = { state_type::t_ref_std_cpp_98 };
        known_classes["t-ref-std-c++03"] = { state_type::t_ref_std_cpp_03 };
        known_classes["t-ref-std-c++11"] = { state_type::t_ref_std_cpp_11 };
        known_classes["t-ref-std-c++14"] = { state_type::t_ref_std_cpp_14 };
        known_classes["t-ref-std-c++17"] = { state_type::t_ref_std_cpp_17 };
        known_classes["t-ref-std-c++20"] = { state_type::t_ref_std_cpp_20 };
        known_classes["t-ref-std-c++23"] = { state_type::t_ref_std_cpp_23 };
        known_classes["t-ref-std-c++26"] = { state_type::t_ref_std_cpp_26 };
        known_classes["t-ref-std-c++29"] = { state_type::t_ref_std_cpp_29 };

        known_classes["t-since-cxx11"] = { state_type::t_since_cpp11 };
        known_classes["t-since-cxx14"] = { state_type::t_since_cpp14 };
        known_classes["t-since-cxx17"] = { state_type::t_since_cpp17 };
        known_classes["t-since-cxx20"] = { state_type::t_since_cpp20 };
        known_classes["t-since-cxx23"] = { state_type::t_since_cpp23 };
        known_classes["t-since-cxx26"] = { state_type::t_since_cpp26 };

        known_classes["t-until-cxx11"] = { state_type::t_until_cpp11 };
        known_classes["t-until-cxx14"] = { state_type::t_until_cpp14 };
        known_classes["t-until-cxx17"] = { state_type::t_until_cpp17 };
        known_classes["t-until-cxx20"] = { state_type::t_until_cpp20 };
        known_classes["t-until-cxx23"] = { state_type::t_until_cpp23 };
        known_classes["t-until-cxx26"] = { state_type::t_until_cpp26 };

        known_classes["t-since-c95"] = { state_type::t_since_c95 };
        known_classes["t-since-c99"] = { state_type::t_since_c99 };
        known_classes["t-since-c11"] = { state_type::t_since_c11 };
        known_classes["t-since-c17"] = { state_type::t_since_c17 };
        known_classes["t-since-c23"] = { state_type::t_since_c23 };

        known_classes["t-until-c95"] = { state_type::t_until_c95 };
        known_classes["t-until-c99"] = { state_type::t_until_c99 };
        known_classes["t-until-c11"] = { state_type::t_until_c11 };
        known_classes["t-until-c17"] = { state_type::t_until_c17 };
        known_classes["t-until-c23"] = { state_type::t_until_c23 };

        known_classes["t-ref-std-c89"] = { state_type::t_ref_std_c89 };
        known_classes["t-ref-std-c99"] = { state_type::t_ref_std_c99 };
        known_classes["t-ref-std-11"] = { state_type::t_ref_std_11 };
        known_classes["t-ref-std-17"] = { state_type::t_ref_std_17 };
        known_classes["t-ref-std-23"] = { state_type::t_ref_std_23 };

        known_classes["table-yes"] = { state_type::table_yes };
        known_classes["table-no"] = { state_type::table_no };
        known_classes["table-maybe"] = { state_type::table_maybe };
        known_classes["table-na"] = { state_type::table_na };

        // TODO:

        // lists?
        known_classes["t-li"] = {};
        known_classes["t-li1"] = {};
        known_classes["t-li2"] = {};
        known_classes["t-li3"] = {};

        known_classes["t-sdsc-begin"] = {};
        known_classes["t-sdsc"] = {};
        known_classes["t-sdsc-nopad"] = {};
        known_classes["t-sdsc-sep"] = {};

        known_classes["t-par-begin"] = {};
        known_classes["t-par"] = {};
        known_classes["t-par-req"] = {};
        known_classes["t-par-hitem"] = {};

        known_classes["t-plot"] = {};
        known_classes["t-plot-bottom"] = {};
        known_classes["t-plot-image-left"] = {};
        known_classes["t-plot-image-left-right"] = {};
        known_classes["t-plot-left"] = {};
        known_classes["t-plot-right"] = {};

        known_classes["t-noexcept-box"] = {};
        known_classes["t-noexcept-full"] = {};
        known_classes["t-noexcept-inline"] = {};

        known_classes["t-page-template"] = {};
        known_classes["t-nv-ln-named-req-table"] = {};
        known_classes["t-su"] = {};
        known_classes["t-v"] = {};
        known_classes["t-vertical"] = {};

        known_classes["t-mrad"] = {};
        known_classes["t-mfrac"] = {};
        known_classes["t-mparen"] = {};
        known_classes["t-inherited"] = {};
        known_classes["t-member"] = {};
        known_classes["t-spar"] = {};

        known_classes["t-c"] = {};
        known_classes["t-cmark"] = {};
        known_classes["t-cc"] = {};

        // footnotes
        known_classes["reference"] = {};
        known_classes["reference-text"] = {};
        known_classes["references"] = {};

        known_classes["mw-collapsible"] = {};
        known_classes["mw-collapsible-content"] = {};
        known_classes["mw-redirect"] = {};
        known_classes["mw-cite-backlink"] = {};

        known_classes["texhtml"] = {};
        known_classes["new"] = {};
        known_classes["row"] = {};
        known_classes["spacer"] = {};
        known_classes["plainlinks"] = {};
        known_classes["mainpagediv"] = {};
        known_classes["mainpagetable"] = {};
        known_classes["div-col"] = {};
        known_classes["extiw"] = {};
        known_classes["eq-fun-cpp-table"] = {};
        known_classes["citation"] = {};
        known_classes["t-template"] = {};
        known_classes["t-template-editlink"] = {};

        known_classes["mbox-image"] = {};

        // math tex (formulas)
        known_classes["mjax"] = {};
        known_classes["mjax-fallback"] = {};

        known_classes["coliru-btn"] = {};
        }
    }

    void pop_state(int depth) {
        while (!st.empty() && st.back().d >= depth) {
            st.pop_back();
        }
    }
    bool is_ignored() const {
        return std::ranges::any_of(st, [](auto &&st){return st.a == action_type::ignore;});
    }
    bool check_classes(const primitives::html::node &n, int depth) {
        auto cl = n.attribute_or_default("class");
        for (auto &&i : std::views::split(cl, " "sv)) {
            std::string_view c{i};
            auto kci = known_classes.find(std::string{ c });
            auto r = kci != known_classes.end();
            auto is_kw0 = [&](auto &&sv){return c.starts_with(sv) && c.size() > 2 && isdigit(c[2]);};
            auto is_kw = is_kw0("kw"sv);
            auto is_sy = is_kw0("sy"sv);
            auto is_br = is_kw0("br"sv);
            if (false
                || is_kw0("kw"sv)
                || is_kw0("sy"sv)
                || is_kw0("br"sv)
                || is_kw0("me"sv)
                || is_kw0("st"sv)
                || is_kw0("nu"sv)
                || is_kw0("es"sv)
                || is_kw0("co"sv)
                || c.starts_with("coMULTI"sv)
                ) {
                return true;
            }
            if (r) {
                st.push_back({ kci->second, depth, &n });
                return r;
            }
        }
        if (!cl.empty()) {
            std::println("unk class: {}", cl);
            return false;
        }
        return true;
    }
    void traverse(const primitives::html::node &n) {
        n.traverse([&](auto &n, int depth){return for_each(n, depth);});
    }
    primitives::html::node::traverse_action for_each(const primitives::html::node &n, int depth) {
        using enum primitives::html::node::traverse_action;

        pop_state(depth);
        if (!check_classes(n, depth) || is_ignored()) {
            return skip_children;
        }

        struct scope_tag {
            cpp_emitter &e;
            std::string type_name;

            scope_tag(cpp_emitter &e, std::string_view tag) : e{ e }, type_name{ tag } {
                e.add_type(std::format("{}{{}}", type_name));
            }
            ~scope_tag() {
                e.add_type(std::format("{}_end{{}}", type_name));
            }
        };
        struct scoped_as_is {
            cpp_emitter &e;
            std::string tag;

            scoped_as_is(cpp_emitter &e, const primitives::html::node &n) : e{ e } {
                tag = n.tag();
                e.add_text(std::format("{}", n.tag_raw()));
            }
            ~scoped_as_is() {
                e.add_text(std::format("</{}>", tag));
            }
        };

        std::string_view cl = n.attribute_or_default("class");
        auto has_class = [&](auto &&c) {
            auto p = cl.find(c);
            if (p != -1) {
                if ((p == 0 || cl[p] == ' ' || cl[p] == '\"') && (cl.size() == p + c.size() || cl[p + c.size()] == ' ')) {
                    return true;
                }
            }
            return false;
            };
        auto has_classes = [&](auto &&...c) {
            return (false || ... || has_class(c));
            };
        // get_classes(n);

        std::string_view name = n.name();
        if (0) {
        } else if (n.is("div"sv)) {
            if (false) {
            } else if (cl.contains("t-navbar"sv)) {
            } else if (cl.contains("t-template-editlink"sv)) {
                e.add_type("template_{}"sv);
                traverse(n);
            } else if (cl.contains("t-dsc-member-div"sv)) {
                scoped_as_is sc{e,n};
                for (auto &&c : n.children) {
                    scoped_as_is sc{ e,*c };
                    traverse(*c);
                }
            } else if (cl.contains("mw-geshi"sv)) {
                scope_tag t{e, "code"};
                e.add_text(extract_text3(n));
            } else {
                traverse(n);
            }
            return skip_children;
        } else if (name.size() == 2 && name[0] == 'h') {
            e.add_header(name[1] - '0');
            traverse(n);
            e.add_type("header_end{}"sv);
            e.add_line();
            return skip_children;
        } else if (n.is("a"sv)) {
            if (0) {
            } else if (auto a = n.attribute_or_default("title"sv); !a.empty()) {
                std::string v{a};
                e.add_type("link{{\"{}\"}}"sv, boost::replace_all_copy(v, " "sv, "_"sv));
            } else if (auto h = n.attribute_or_default("href"sv); !h.empty()) {
                e.add_type("link{{\"{}\"}}"sv, h);
            } else {
                e.add_type("link{}"sv);
            }
            traverse(n);
            e.add_type("link_end{}"sv);
            return skip_children;
        } else if (n.is("span"sv)) {
            if (has_classes("t-mark"sv, "t-mark-rev"sv)) {
                e.add_text(extract_as_is(n));
                return skip_children;
            }
            if (has_classes("t-lines"sv)) {
                e.add_text(extract_as_is(n));
                return skip_children;
            }
            traverse(n);
            return skip_children;
        } else if (n.is("p"sv)) {
            e.add_type("paragraph{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("pre"sv)) {
            traverse(n);
            return skip_children;
        } else if (n.is("code"sv)) {
            scope_tag t{ e, "code_tag" };
            traverse(n);
            return skip_children;
        } else if (n.is("table"sv)) {
            e.add_type("table{}"sv);
            traverse(n);
            e.add_type("table_end{}"sv);
            e.add_line();
            return skip_children;
        } else if (n.is("tbody"sv)) {
        } else if (n.is("tr"sv)) {
            // read rowspan
            e.add_type("next_row{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("th"sv)) {
            // read colspan
            e.add_type("next_col{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("td"sv)) {
            // read colspan
            e.add_type("next_col{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("cite"sv)) {
            e.add_type("cite{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("b"sv)) {
            e.add_type("bold{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("strong"sv)) {
            e.add_type("bold{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("small"sv)) {
            e.add_type("small{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("i"sv)) {
            e.add_type("italic{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("tt"sv)) {
            e.add_type("monospace{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("br"sv)) {
            e.add_text("\n");
            return skip_children;
        } else if (n.is("abbr"sv)) {
            e.add_type("abbr{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("ul"sv)) {
            e.add_type("ul{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("ol"sv)) {
            e.add_type("ol{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("li"sv)) {
            e.add_type("li{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("dl"sv)) { // desc list
            e.add_type("dl{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("dd"sv)) { // desc, def for dl
            e.add_type("dd{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("dt"sv)) {
            e.add_type("dt{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("blockquote"sv)) {
            e.add_type("blockquote{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("img"sv)) {
            e.add_type("img{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("caption"sv)) {
            e.add_type("caption{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("sub"sv)) {
            e.add_type("sub{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is("sup"sv)) {
            e.add_type("sup{}"sv);
            traverse(n);
            return skip_children;
        } else if (n.is(""sv)) {
            e.add_text(n.text());
            return skip_children;
        } else {
            std::println("unhandled tag: {}", name);
            e.add_text(n.text());
            return skip_children;
        }
        return continue_;
    }
};

void pages_to_cpp(const path &root) {
    std::println("parsing...");

    cpp_emitter all;
    all.add_line("#pragma once");
    all.add_line();
    auto &headers = all.create_inline_emitter();
    auto struct_name = "pages"s;
    all.begin_block("struct " + struct_name + " {");
    auto &members = all.create_inline_emitter();

    auto fix_name = [](std::string s) {
        boost::replace_all(s, "NAN", "NAN_");
        boost::replace_all(s, ".", "_");
        boost::replace_all(s, "-", "_");
        boost::replace_all(s, "(", "_lbrace");
        boost::replace_all(s, ")", "_rbrace");
        boost::replace_all(s, "\"", "_quote");
        boost::replace_all(s, "+", "_plus");
        boost::replace_all(s, "~", "tilda_");
        boost::replace_all(s, "=", "_equal");
        boost::replace_all(s, "!", "_exclamation");
        boost::replace_all(s, "\n", "");
        boost::replace_all(s, "\r", "");
        return s;
        };
    auto fix2_name = [](std::string s) {
        boost::replace_all(s, "/", "_slash_");
        boost::replace_all(s, " ", "_");
        boost::replace_all(s, "\n", "");
        boost::replace_all(s, "\r", "");
        return s;
        };
    auto make_ns = [&](std::string in) {
        std::string s;
        for (auto &&t : split_string(in, "/")) {
            s += "ns_" + t + "::";
        }
        fix_name(s);
        s.pop_back();
        s.pop_back();
        return s;
        };
    std::map<std::string, int> vars;
    auto make_var = [&](std::string in) {
        in = fix2_name(in);
        if (auto [it,inserted] = vars.emplace(in, 0); !inserted) {
            in += std::format("{}", ++it->second);
        }
        return in;
        };

    bool all_only{};
    //all_only = true;
    std::set<std::string> pages;
    //primitives::sqlite::sqlitemgr db{ path{mirror_root_dir} += ".db" };
    //for (auto &&db_p : db.select<::db::parser::schema::tables_::page>()) {
    for (auto &&[p,db_p] : cache().get_all<url_request_cache>()) {
        auto n = p;
        if (n.starts_with("http")) {
            auto w = "/w/"sv;
            if (!n.contains(w)) {
                continue;
            }
            n = n.substr(n.find(w) + w.size());
        }
        if (n.ends_with(".html"s)) {
            n = n.substr(0, n.size() - 5);
        }
        boost::replace_all(n, "%2522", "\"");
        boost::replace_all(n, "%252A", "+");
        if (1
            //&& n != "cpp/utility/format"sv
            //&& n != "cpp/compiler_support"sv
            //&& n != "c/numeric/math/NAN"sv
            && n != "cpp/header/algorithm"sv
            //&& n != "cpp/header/stdatomic.h"sv
            //&& n != "cpp/utility/expected"sv
            //&& n != "cpp/memory/new/operator_delete"sv
            ) {
            //continue;
        }

        n = fix_name(n);

        pages.insert(n);
        if (!all_only) {
            std::println("[{}] {}", pages.size(), n);
        }

        auto ns = make_ns(n);

        html_page page{ db_p };

        cpp_emitter page_emitter;
        page_emitter.begin_namespace(ns);
        page_emitter.begin_block("struct page {");
        page_emitter.add_line(std::format("std::string filename{{\"{}\"s}};", n));
        page_emitter.add_line(std::format("std::string title{{R\"xxx({})xxx\"s}};", boost::trim_copy(page.value("id", "firstHeading"))));
        page_emitter.add_line();
        page_emitter.add_line("void render(auto &renderer);");
        page_emitter.end_block(true);
        page_emitter.add_line();
        page_emitter.begin_function("void page::render(auto &renderer) {");
        page_emitter.add_line("auto &c = renderer;");
        page_emitter.add_line();

        //begin_f(page_emitter);
        auto contents = page.find_node("id", "mw-content-text");
        cpp_traverser t{ page_emitter };
        if (!all_only) {
            t.traverse(*contents);
        }

        page_emitter.end_function();
        page_emitter.end_namespace(ns);

        path fn = n;
        fn = fn.parent_path() / fn.stem() += ".h";
        if (!all_only) {
            write_file(root / fn, page_emitter.get_text());
        }
    }

    std::println("parsing done");

    all.add_line("void render(auto &&renderer);");
    all.end_block(true);
    all.add_line();
    all.begin_function("void " + struct_name + "::render(auto &&renderer) {");
    all.add_line();
    auto &calls = all.create_inline_emitter();
    all.end_function();

    calls.add_line("// we can use reflection to do for each all members");
    for (auto &&n : pages) {
        auto ns = make_ns(n);
        auto v = make_var(n);

        headers.add_line(std::format("#include \"{}.h\"", n));
        members.add_line(ns + "::page " + v + ";");
        calls.add_line("renderer.render(" + v + ");");
    }
    //headers.add_line();

    write_file(root / "all.h", all.get_text());
}

int main(int argc, char *argv[]) {
    path root_dir{ "generated/cpp" };
    //parse();
    pages_to_cpp(root_dir);
    return 0;
}
