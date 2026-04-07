// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2024-2026 Egor Pugin <egor.pugin@gmail.com>

// also see https://github.com/PeterFeicht/cppreference-doc

//#include "cpp.h"

#include <primitives/emitter.h>
#include <primitives/executor.h>
#include <primitives/http.h>
#include <primitives/sw/main.h>
#include <primitives/templates2/sqlite.h>
//#include <primitives/templates2/xml.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <print>
#include <ranges>
#include <syncstream>

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

struct html_node {
    enum class state_type {
        expect_start_tag,
        expect_end_tag,
    };
    struct state {
        state_type st{};
        std::string_view sv;
        size_t p{}, e, cdata;
    };

    std::string_view n;
    html_node *parent{};
    std::vector<html_node> children;

    size_t parse(std::string_view d, state s = state{}) {
        std::vector<std::string_view> tokens;
        tokens.reserve(100000);
        size_t p{};
        while (1) {
            auto b = d.find('<', p);
            if (b == -1) {
                if (b != d.size()) {
                    b = d.size();
                    tokens.emplace_back(d.substr(p, b - p));
                }
                break;
            }
            if (b != p) {
                tokens.emplace_back(d.substr(p, b - p));
                p = b;
                continue;
            }
            auto sv = d.substr(b);
            if (sv.starts_with("<!--")) {
                constexpr auto etag = "-->"sv;
                auto e = d.find(etag, b);
                e += etag.size();
                tokens.emplace_back(d.substr(b, e - b));
                p = e;
                continue;
            }
            constexpr auto cdata = "<![CDATA["sv;
            if (sv.starts_with(cdata)) {
                constexpr auto etag = "]]>"sv;
                auto e = d.find(etag, b);
                b += cdata.size();
                tokens.emplace_back(d.substr(b, e - b));
                e += etag.size();
                p = e;
                continue;
            }
            auto e = d.find('>', b);
            ++e;
            tokens.emplace_back(d.substr(b, e - b));
            p = e;
        }
        while (1) {
            //switch (s.st) {
            //case state_type::expect_start_tag:
                s.p = d.find('<', s.p);
                if (s.p == -1) {
                    return 0;
                }
                s.sv = d.substr(s.p);
                if (s.sv.starts_with("<!"sv)) {
                    if (s.sv.starts_with("<!--"sv)) {
                        constexpr auto etag = "-->"sv;
                        s.e = d.find(etag, s.p);
                        s.p = s.e;
                    } else if (s.sv.starts_with("<![CDATA["sv)) {
                        constexpr auto etag = "]]>"sv;
                        s.e = d.find(etag, s.p);
                        s.p = s.e;
                    } else if (s.sv.starts_with("<!DOCTYPE"sv)) {
                        s.e = d.find('>', s.p);
                        ++s.e;
                        s.sv = std::string_view{ d.data() + s.p, s.e - s.p };
                        children.emplace_back(s.sv);
                        s.p = s.e;
                    } else {
                        throw;
                    }
                } else {
                    s.e = d.find('>', s.p);
                    ++s.e;
                    s.sv = std::string_view{ d.data() + s.p, s.e - s.p };
                    if (s.sv.ends_with("/>"sv)) {
                        s.p = s.e;
                        children.emplace_back(s.sv);
                    } else if (s.sv.starts_with("</"sv)) {
                        s.p = s.e;
                        children.emplace_back(s.sv);
                    } else if (s.sv.ends_with(">"sv)) {
                        children.emplace_back(s.sv);
                        if (!is_void_tag(s.sv)) {
                            s.e = parse(d.substr(s.e));
                            s.st = state_type::expect_end_tag;
                        }
                    } else {

                    }
                }
                //break;
            //case state_type::expect_end_tag:
                //break;
            //}
            //p = e;
        }
        return -1;
    }
    static bool is_void_tag(auto sv) {
        sv.remove_prefix(1);
        return false
            || sv.starts_with("input"sv)
            ;
    }
};

struct page {
    std::string url;
    std::string source;
    std::set<std::string> links;

    page() = default;
    page(const std::string &url) : url{url} {
        source = download_url(url);
        //source = tidy_html(source);
        parse_links();
    }
    bool is_c_page() const {
        return url.contains("/w/c/"sv) || url.ends_with("/w/c"sv);
    }
    bool is_cpp_page() const {
        return url.contains("/w/cpp/"sv) || url.ends_with("/w/cpp"sv);
    }
    void parse_links() {
        /*pugi::xml_document doc;
        if (auto r = doc.load_buffer(source.data(), source.size()); !r) {
            throw std::runtime_error{std::format("name = {}, xml parse error = {}", url, r.description())};
        }
        for (auto &&n : doc.select_nodes("//a[@href]")) {
            auto a = n.node().attribute("href");
            std::string l = a.value();
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
        }*/
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

static std::string extract_text3(auto &&n, const std::string &delim = ""s) {
    std::string s;
    /*if (n.type() == pugi::node_pcdata) {
        s += n.value() + delim;
    }
    for (auto &&c : n.children()) {
        s += extract_text3(c, delim) + delim;
    }*/
    boost::trim(s);
    return s;
}

static auto get_classes(auto &&n) {
    std::string_view cl = n.attribute("class").as_string();
    return std::set{ std::from_range, cl | std::views::split(' ') | std::views::transform([](auto &&v) {return std::string_view{v}; }) };
}

struct html_page {
    std::string data;
    html_node doc;

    html_page(const ::db::parser::schema::tables_::page &p) {
        data = p.source.value;
        doc.parse(data);
    }

    /*static auto find_node(auto &&n, auto &&attrname, auto &&idname) {
        return n.select_node((".//*[@"s + attrname + "=\""s + idname + "\"]").c_str());
    }
    static auto find_nodes(auto &&n, auto &&attrname, auto &&idname) {
        return n.select_nodes((".//*[@"s + attrname + "=\""s + idname + "\"]").c_str());
    }
    auto find_node(auto &&attrname, auto &&idname) {
        return find_node(doc, attrname, idname);
    }
    std::string value(auto &&attrname, auto &&idname) {
        auto n = find_node(attrname, idname);
        return n ? std::string{n.node().text().as_string()} : std::string{};
    }
    std::string extract_by_id(auto &&attrname, auto &&idname) {
        auto n = find_node(attrname, idname);
        if (!n) {
            return {};
        }
        n.node().print(std::cout, " ");
        int a = 5;
        a++;
        return {};
    }

    auto find_navbar() {
        return find_node("class", "t-navbar");
    }
    auto find_navbar_heads(auto &&n) {
        return n.select_nodes("./*[contains(@class, 't-navbar-head')]");
    }
    auto find_navbar_menu(auto &&n) {
        return n.select_node("./*[contains(@class, 't-navbar-menu')]");
    }
    auto find_navbar_rows(auto &&n) {
        return n.select_nodes("./div/div/table/tr");
    }
    auto find_navbar_rows2(auto &&n) {
        return n.select_nodes("./td/div/table/tr");
    }
    auto find_navbar_cols(auto &&n) {
        return n.select_nodes("./td");
    }

    /*void parse(cpp_reference::page &p) {
        p.title = value("id", "firstHeading");
        //extract_by_id("id", "bodyContent");
        extract_by_id("id", "mw-content-text");
    }
    void parse(cpp_reference::compiler_support &p) {
        //extract_by_id("id", "bodyContent");
    }
    void parse(cpp_reference::c_language_standard_page &p) {
    }
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

// FIXME: this one make indents inside raw strings with newlines, just make custom emitter
struct Emitter : primitives::CppEmitter {
    struct header {

    };

    auto format(auto &&f, auto &&...args) {
        addLine(std::vformat(f, std::make_format_args(args...)));
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
        //pugi::xml_node n;
    };

    /*struct node_wrapper : pugi::xml_node {
        bool is(std::string_view v) const {
            return v == name();
        }
    };*/

    Emitter &e;
    std::vector<state> st;
    state last_removed{};
    std::map<std::string, state_desc> known_classes;
    nlohmann::json j_state;

    enum {full_stop, continue_, skip_children};
    int depth{-1};

    cpp_traverser(Emitter &e) : e{e} {
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

    void pop_state() {
        while (!st.empty() && st.back().d >= depth) {
            last_removed = st.back();
            st.pop_back();
        }
    }
    bool is_ignored() const {
        return std::ranges::any_of(st, [](auto &&st){return st.a == action_type::ignore;});
    }
    /*bool check_classes(pugi::xml_node &n) {
        std::string_view cl = n.attribute("class").as_string();
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
                || cl.starts_with("coMULTI"sv)
                ) {
                return true;
            }
            if (r) {
                st.push_back({ kci->second, depth, n });
                return r;
            }
        }
        if (!cl.empty()) {
            std::println("unk class: {}", cl);
        }
        return false;
    }
    bool traverse1(pugi::xml_node in, auto &&cb) {
        auto root = in;
        auto cur = root ? root.first_child() : root;
        if (cur) {
            ++depth;
            do {
                if (!check_classes(cur)) {
                }
                int r = is_ignored() ? skip_children : cb(cur);
                if (r == skip_children) {
                    pop_state();
                }
                //int r = cb(cur);
                if (r == full_stop)
                    return false;
                if (cur.first_child() && r != skip_children) {
                    ++depth;
                    cur = cur.first_child();
                } else if (cur.next_sibling())
                    cur = cur.next_sibling();
                else {
                    while (!cur.next_sibling() && cur != root && cur.parent()) {
                        --depth;
                        cur = cur.parent();
                        pop_state();
                    }
                    if (cur != root) {
                        cur = cur.next_sibling();
                    }
                }
            } while (cur && cur != root);
        }
        return true;
    }
    bool traverse(pugi::xml_node n, auto &&cb) {
        return traverse1(n, cb);
    }
    bool traverse(pugi::xml_node n) {
        return traverse(n, [&](pugi::xml_node n){return for_each(n);});
    }
    int for_each(pugi::xml_node &in) {
        node_wrapper n{ in };
        std::string_view name = n.name();
        if (0) {
        } else if (n.is("div"sv)) {
            std::string_view cl = n.attribute("class").as_string();
            if (false) {
            } else if (cl.contains("t-navbar"sv)) {
            } else if (cl.contains("t-template-editlink"sv)) {
                e.add_type("template_{}"sv);
                traverse(n);
            } else if (cl.contains("mw-geshi"sv)) {
                e.add_text(extract_text3(n));
            } else {
                traverse(n);
            }
            return skip_children;
        } else if (name.size() == 2 && name[0] == 'h') {
            e.add_header(name[1] - '0');
            traverse(n);
            return skip_children;
        } else if (n.is("a"sv)) {
            std::string_view cl = n.attribute("class").as_string();
            if (0) {
            } else if (auto a = n.attribute("title"sv)) {
                std::string v = a.as_string();
                e.add_type("link{{\"{}\"}}"sv, boost::replace_all_copy(v, " "sv, "_"sv));
            } else if (auto h = n.attribute("href"sv)) {
                e.add_type("link{{\"{}\"}}"sv, h.as_string());
            } else {
                e.add_type("link{}"sv);
            }
            traverse(n);
            e.add_type("link_end{}"sv);
            return skip_children;
        } else if (n.is("span"sv)) {
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
            e.add_type("code{}"sv);
            traverse(n);
            e.add_type("code_end{}"sv);
            return skip_children;
        } else if (n.is("table"sv)) {
            e.add_type("table{}"sv);
            traverse(n);
            e.add_type("table_end{}"sv);
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
            e.add_text(n.text().as_string());
            return skip_children;
        } else {
            std::println("unhandled tag: {}", name);
            e.add_text(n.text().as_string());
            return skip_children;
        }
        return true;
    }*/
};

void pages_to_cpp(const path &root) {
    std::println("parsing...");

    Emitter all;
    all.addLine("#pragma once");
    all.emptyLines();
    auto &headers = all.createInlineEmitter<Emitter>();

    auto begin_f = [](auto &e) {
        e.beginFunction("void f(auto &&consumer)");
        e.addLine("auto &c = consumer;");
        };
    //all.emptyLines();
    begin_f(all);

    auto fix_name = [](std::string s) {
        boost::replace_all(s, "NAN", "NAN_");
        boost::replace_all(s, ".", "_");
        boost::replace_all(s, "-", "_");
        boost::replace_all(s, "(", "_lbrace");
        boost::replace_all(s, ")", "_rbrace");
        boost::replace_all(s, "\"", "_quote");
        return s;
        };
    auto make_ns = [&](std::string in) {
        std::string s;
        for (auto &&t : split_string(in, "/")) {
            s += "ns_" + t + "::";
        }
        boost::replace_all(s, "\"", "_quote");
        boost::replace_all(s, "+", "_plus");
        boost::replace_all(s, "~", "tilda_");
        boost::replace_all(s, "=", "_equal");
        s.pop_back();
        s.pop_back();
        return s;
        };

    bool all_only{};
    //all_only = true;
    std::set<std::string> pages;
    primitives::sqlite::sqlitemgr db{ path{mirror_root_dir} += ".db" };
    for (auto &&db_p : db.select<::db::parser::schema::tables_::page>()) {
        auto n = db_p.name.value;
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
            continue;
        }

        n = fix_name(n);

        pages.insert(n);
        if (!all_only) {
            std::println("[{}] {}", pages.size(), n);
        }

        auto ns = make_ns(n);

        Emitter page_emitter;
        page_emitter.beginNamespace(ns);
        begin_f(page_emitter);

        html_page p{ db_p };
        /*page_emitter.addLine(std::format("c << page{{\"{}\"s}};", boost::trim_copy(p.value("id", "firstHeading"))));
        page_emitter.addLine();

        auto contents = p.find_node("id", "mw-content-text");
        cpp_traverser t{ page_emitter };
        if (!all_only) {
            t.traverse(contents.node());
        }*/

        page_emitter.endFunction();
        page_emitter.endNamespace(ns);

        path fn = n;
        fn = fn.parent_path() / fn.stem() += ".h";
        if (!all_only) {
            write_file(root / fn, page_emitter.getText());
        }
    }

    std::println("parsing done");

    for (auto &&n : pages) {
        auto ns = make_ns(n);

        headers.addLine(std::format("#include \"{}.h\"", n));
        all.addLine(ns + "::f(c);");
    }

    headers.addLine();
    all.endFunction();
    write_file(root / "all.h", all.getText());
}

int main(int argc, char *argv[]) {
    auto d = cache().find<url_request_cache>("https://en.cppreference.com/w/cpp/header/algorithm.html"s);
    html_node n;
    n.parse(*d);
    return 0;

    path root_dir{ "data" };
    //parse();
    pages_to_cpp(root_dir);
    return 0;
}
