// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2024-2026 Egor Pugin <egor.pugin@gmail.com>

// also see https://github.com/PeterFeicht/cppreference-doc

#include "cpp.h"

#include <pugixml.hpp>
#include <primitives/http.h>
#include <primitives/sw/main.h>
#include <primitives/templates2/sqlite.h>
#include <primitives/templates2/xml.h>
#include <tidy.h>
#include <tidybuffio.h>

#include <format>
#include <print>

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

const path mirror_root_dir = "cppreference";

auto url_base = "cppreference.com"s;
auto lang = "en"s;
auto protocol = "https"s;
auto normal_page = "w"s;
auto start_page = "Main_Page"s;

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
    HttpRequest req{httpSettings};
    req.url = url;
    auto resp = url_request(req);
    if (resp.http_code != 200) {
        throw std::runtime_error{std::format("url = {}, http code = {}", url, resp.http_code)};
    }
    return resp.response;
}

struct page {
    path url;
    std::string source;
    std::set<std::string> links;

    page() = default;
    page(const std::string &url) : url{url}, source{tidy_html(download_url(url))} {
        parse_links();
    }
    bool is_c_page() const {
        auto u = url.string();
        return u.contains("/w/c/"sv) || u.ends_with("/w/c"sv);
    }
    bool is_cpp_page() const {
        auto u = url.string();
        return u.contains("/w/cpp/"sv) || u.ends_with("/w/cpp"sv);
    }
    void parse_links() {
        pugi::xml_document doc;
        if (auto r = doc.load_buffer(source.data(), source.size()); !r) {
            throw std::runtime_error{std::format("name = {}, xml parse error = {}", url.string(), r.description())};
        }

        auto extract_hrefs2 = [&]() {
            for (auto &&n : doc.select_nodes("//a[@href]")) {
                auto a = n.node().attribute("href");
                std::string l = a.value();
                l = primitives::http::url_decode(l);
                //std::println("href = {}", l);
                if (l.starts_with("http"sv)) {
                    continue;
                }
                if (l.starts_with('/')) {
                    continue;
                }
                l = l.substr(0, l.find('#')); // take everything before '#'
                l = l.substr(0, l.find('%')); // take everything before '%'
                if (l.ends_with(".html"sv)) {
                    l = l.substr(0, l.size() - 5);
                }
                // skip uninteresting pages
                if (l.starts_with("User"sv) || (!l.empty() && isupper(l[0]))) {
                    continue;
                }
                auto u = url;
                if (!l.starts_with("http"sv) && !l.starts_with("../"sv)) {
                    //links.insert(l);
                    //continue;
                    u = u.parent_path();
                }
                if (l.starts_with("../"sv)) {
                    u = u.parent_path();
                    //if (is_c_page() || is_cpp_page()) {
                    //    l = l.substr(3);
                    //    continue;
                    //}
                    //break;
                    //while (u.filename() != "c" && u.filename() != "cpp" && u.filename() != "w") {
                    //    u = u.parent_path();
                    //}
                    //u /= "dummy_element_that_will_be_removed";
                }
                //auto p = url.parent_path() / l;
                path p = u / l;
                p = normalize_path(p);
                p = p.lexically_normal();
                p = normalize_path(p);
                l = p.string();
                if (auto p = l.find("http"sv); p != -1)
                    l = l.substr(p);
                if (l.starts_with("https:/"sv)) {
                    //l = "https://" + l.substr(7);
                    l = "https://" + l.substr(7);
                }
                links.insert(l);
            }
        };
        extract_hrefs2();
        return;

        auto extract_hrefs = [&](auto &&xpath, int len) {
            for (auto &&n : doc.select_nodes(xpath)) {
                auto a = n.node().attribute("href");
                if (a) {
                    std::string l = a.value();
                    if (len) {
                        l = l.substr(len); // skip the beginning
                    }
                    l = l.substr(0, l.find('#')); // take everything before '#'
                    if (l.ends_with(".html"sv)) {
                        l = l.substr(0, l.size() - 5);
                    }
                    links.insert(l);
                }
            }
        };
        extract_hrefs("//a[starts-with(@href,'/w/c')]", 3); // old (2024)
        extract_hrefs("//a[starts-with(@href,'w/c')]", 2); // newer (2025-)
        //extract_hrefs("//a[starts-with(@href,'c')]", 0); // newer (2025-)
        // extract_hrefs("//a[starts-with(@href,'/w/cpp')]");
    }
};

struct parser {
    primitives::sqlite::sqlitemgr db{path{ mirror_root_dir } += "_03.2026.db"};
    std::map<std::string, page> pages;
    std::set<std::string> bad_pages;

    parser() {
        db.create_tables(::db::parser::schema{});
    }
    void start() {
        parse_page(start_page);
        while (1) {
            auto old = pages.size();
            for (auto &&[_, p] : pages) {
                for (auto &&t : p.links) {
                    parse_page(t);
                }
            }
            if (pages.size() == old) {
                break;
            }
        }
    }
    void parse_page(auto &&pagename) {
        if (pages.contains(pagename)) {
            return;
        }

        auto db_page_sel = db.select<::db::parser::schema::tables_::page, &::db::parser::schema::tables_::page::name>(pagename);
        auto db_page_i = db_page_sel.begin();
        if (db_page_i != db_page_sel.end()) {
            page p;
            p.url = make_normal_page_url(pagename);
            auto &db_p = *db_page_i;
            p.source = db_p.source;
            p.parse_links();
            pages.emplace(pagename, p);
            return;
        }
        if (bad_pages.contains(pagename)) {
            return;
        }

        std::println("parsing {}", pagename);
        try {
            auto &&[it, _] = pages.emplace(pagename, page{make_normal_page_url(pagename)});
            auto &p = it->second;

            auto tr = db.scoped_transaction();
            auto page_ins = db.prepared_insert<::db::parser::schema::tables_::page, primitives::sqlite::db::or_ignore{}>();
            page_ins.insert({.name = pagename, .source = p.source});
        } catch (std::exception &e) {
            std::cerr << e.what() << "\n";
            bad_pages.insert(pagename);
        }
    }
};

void parse() {
    parser p;
    p.start();
}

struct html_page {
    pugi::xml_document doc;

    html_page(const ::db::parser::schema::tables_::page &p) {
        auto &page = p.source.value;
        if (!doc.load_buffer(page.data(), page.size())) {
            std::println("cannot read xml {}", p.name.value);
            throw;
        }
    }

    static auto find_node(auto &&n, auto &&attrname, auto &&idname) {
        return n.select_node(("//*[@"s + attrname + "=\""s + idname + "\"]").c_str());
    }
    static auto find_nodes(auto &&n, auto &&attrname, auto &&idname) {
        return n.select_nodes(("//*[@"s + attrname + "=\""s + idname + "\"]").c_str());
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
    static auto extract_text1(auto &&n) {
        std::string full_text;
        for (const auto &item : n.select_nodes(".//text()")) {
            full_text += item.node().value() + "\n"s;
        }
        boost::trim(full_text);
        return full_text;
    }
    static std::string extract_text2(auto &&n) {
        std::string s;
        for (auto &&c : n.children()) {
            if (c.type() == pugi::node_pcdata) {
                s += c.value();
            } else if (c.type() == pugi::node_element) {
                auto cls = get_classes(c);
                if (cls.contains("noprint"sv) || cls.contains("editsection"sv)) {
                    continue;
                }
                s += extract_text2(c);
            }
        }
        boost::trim(s);
        return s;
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
    auto find_a(auto &&n) {
        return n.select_node(".//a");
    }
    auto find_strong(auto &&n) {
        return n.select_node(".//strong");
    }
    auto find_table_rows(auto &&n) {
        return n.select_nodes(".//tr");
    }
    static auto get_classes(auto &&n) {
        std::string_view cl = n.attribute("class").as_string();
        return std::set{std::from_range, cl | std::views::split(' ') | std::views::transform([](auto &&v){return std::string_view{v};})};
    }

    void parse(cpp_reference::page &p) {
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
    }
    void parse(cpp_reference::page_raw &p) {
        p.title = boost::trim_copy(value("id", "firstHeading"));

        auto contents = find_node("id", "mw-content-text");
        auto n = contents.node().first_child();

        auto default_text = [&]() {
            p.all_text.emplace_back(extract_text2(n));
            };

        while (n) {
            auto cl = n.attribute("class").as_string();
            auto cls = get_classes(n);
            if (0) {
            } else if (n.name() == "h1"sv) {
                default_text();
            } else if (n.name() == "h2"sv) {
                default_text();
            } else if (n.name() == "h3"sv) {
                default_text();
            } else if (n.name() == "h4"sv) {
                default_text();
            } else if (n.name() == "h5"sv) {
                default_text();
            } else if (n.name() == "h6"sv) {
                default_text();
            } else if (n.name() == "div"sv) {
            } else if (n.name() == "table"sv) {
            //} else if (n.name() == "table"sv || n.name() == "div"sv) {
            } else if (n.name() == "ul"sv) {
                default_text();
            } else if (n.name() == "dl"sv) {
                default_text();
            } else if (n.name() == "ol"sv) {
                default_text();
            } else if (n.name() == "pre"sv) {
                default_text();
            } else if (n.name() == "code"sv) {
                default_text();
            } else if (n.name() == "sup"sv) {
                default_text();
            } else if (n.name() == "span"sv) {
                default_text();
            } else if (n.name() == "a"sv) {
                default_text();
            } else if (n.name() == "i"sv) {
                default_text();
            } else if (n.name() == "br"sv) {
            } else if (n.name() == "hr"sv) {
            } else if (n.name() == "blockquote"sv) {
                default_text();
            } else if (n.name() == "p"sv) {
                default_text();
                //std::println("{}", extract_text2(n));
            } else if (n.name() == ""sv) { // ?
            } else {
                std::println("unknown tag {}", n.name());
            }

            if (0) {
            } else if (cls.contains("t-navbar"sv)) {
                parse_navbar(p, n);
            } else if (cls.contains("t-dcl-begin"sv)) { // declaration
                parse_table_decl(p, n);
            } else if (cls.contains("t-par-begin"sv)) { // parameters
                parse_table_decl(p, n);
            } else if (cls.contains("t-rev-begin"sv)) { // return value or exceptions?
                parse_table_decl(p, n);
            } else if (cls.contains("t-dsc-begin"sv)) { // see also
                parse_table_decl(p, n);
            } else if (cls.contains("t-example"sv)) { // example?
                parse_table_decl(p, n);
            } else if (cls.contains("t-sdsc-begin"sv)) { // ?
                parse_table_decl(p, n);
            } else if (cls.contains("dsctable"sv)) { // defect reports
            } else if (cls.contains("references"sv)) { //
            } else if (cls.contains("eq-fun-cpp-table"sv)) { // ?
            } else if (cls.contains("toc"sv)) { // (almost) always hidden
            } else if (cls.contains("metadata"sv)) { // not needed?
            } else if (cls.contains("wikitable"sv)) { // not needed?
            } else if (cls.contains("mainpagetable"sv)) { // not needed?
            } else if (cls.contains("plainlinks"sv)) { // prob some info pages?
            } else if (cls.contains("t-template-editlink"sv)) { //
            } else if (cls.contains("mw-geshi"sv)) { // ?
            } else if (cls.contains("t-li1"sv)) { // list
                default_text();
            } else if (cls.contains("t-li2"sv)) { // list, 2nd intendation
                default_text();
            } else if (cls.contains("t-ref-std-c89"sv)) {
            } else if (cls.contains("t-ref-std-c99"sv)) {
            } else if (cls.contains("t-ref-std-11"sv)) {
            } else if (cls.contains("t-ref-std-17"sv)) {
            } else if (cls.contains("t-ref-std-23"sv)) {
            } else if (cl == ""sv) {
            } else {
                //std::println("unknown table {}", cl);
            }

            n = n.next_sibling();
        }
    }
};

void pages_to_cpp() {
    cppreference_website w;
    primitives::sqlite::sqlitemgr db{ path{mirror_root_dir} += ".db" };
    std::println("parsing...");
    for (auto &&p : db.select<::db::parser::schema::tables_::page>()) {
        auto n = p.name.value;
        if (n.starts_with("http")) {
            auto w = "/w/"sv;
            if (!n.contains(w)) {
                continue;
            }
            n = n.substr(n.find(w) + w.size());
        }
        if (n != "cpp/utility/expected"sv) {
            continue;
        }
        if (n != "cpp/memory/new/operator_delete"sv) {
            //continue;
        }
        //std::println("{}", n);

        html_page page{ p };
        cpp_reference::page_raw pr;
        pr.name = n;
        page.parse(pr);
        w.pages[pr.title] = pr;
        continue;



        //std::optional<cppreference_objects::variant_type> obj;
        //cppreference_objects::for_each([&]<typename T>(T**){
        //    if (T::is(n)) {
        //        obj = T{};
        //        page.parse(std::get<T>(*obj));
        //    }
        //});
        //if (!obj) {
        //    cpp_reference::page p;
        //    page.parse(p);
        //    continue;
        //}
        //if (!obj) {
        //    cpp_reference::page p;
        //    page.parse(p);
        //    continue;
        //}
    }

    std::println("parsing done");

    write_file("all.txt", w.print_raw());
    write_file("all.tex", w.print_latex());
    write_file("all.json", w.print_json());
}

int main(int argc, char *argv[]) {
    //parse();
    pages_to_cpp();
    return 0;
}
