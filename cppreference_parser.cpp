/*
c++: 23
package_definitions: true
deps:
    - pub.egorpugin.primitives.http
    - pub.egorpugin.primitives.templates2
    - pub.egorpugin.primitives.sw.main
    - org.sw.demo.zeux.pugixml
    - org.sw.demo.htacg.tidy_html5
    - org.sw.demo.sqlite3
    - org.sw.demo.boost.pfr
*/

// also see https://github.com/PeterFeicht/cppreference-doc

#include "cpp.h"

#include <pugixml.hpp>
#include <primitives/http.h>
#include <primitives/sw/main.h>
#include <primitives/templates2/sqlite.h>
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
    return std::format("{}/{}/{}", make_base_url(), normal_page, page);
}

auto db_fn() {
    return path{mirror_root_dir} += "2.db";
}
auto tidy_html(auto &&s) {
    TidyDoc tidyDoc = tidyCreate();
    SCOPE_EXIT {
        tidyRelease(tidyDoc);
    };
    TidyBuffer tidyOutputBuffer = {0};
    tidyOptSetBool(tidyDoc, TidyXmlOut, yes) && tidyOptSetBool(tidyDoc, TidyQuiet, yes) &&
        tidyOptSetBool(tidyDoc, TidyNumEntities, yes) && tidyOptSetBool(tidyDoc, TidyShowWarnings, no) &&
        tidyOptSetInt(tidyDoc, TidyWrapLen, 0);
    tidyParseString(tidyDoc, s.c_str());
    tidyCleanAndRepair(tidyDoc);
    tidySaveBuffer(tidyDoc, &tidyOutputBuffer);
    if (!tidyOutputBuffer.bp)
        throw SW_RUNTIME_ERROR("tidy: cannot convert from html to xhtml");
    std::string tidyResult;
    tidyResult = (char *)tidyOutputBuffer.bp;
    tidyBufFree(&tidyOutputBuffer);
    return tidyResult;
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
    std::string name;
    std::string source;
    std::set<std::string> links;

    page() = default;
    page(const std::string &name, const std::string &url) : name{name}, source{tidy_html(download_url(url))} {
        parse_links();
    }
    void parse_links() {
        pugi::xml_document doc;
        if (auto r = doc.load_buffer(source.data(), source.size()); !r) {
            throw std::runtime_error{std::format("name = {}, xml parse error = {}", name, r.description())};
        }
        auto extract_hrefs = [&](auto &&xpath) {
            for (auto &&n : doc.select_nodes(xpath)) {
                auto a = n.node().attribute("href");
                if (a) {
                    std::string l = a.value();
                    l = l.substr(3); // skip '/w/'
                    l = l.substr(0, l.find('#')); // take everything before '#'
                    links.insert(l);
                }
            }
        };
        extract_hrefs("//a[starts-with(@href,'/w/c')]");
        // extract_hrefs("//a[starts-with(@href,'/w/cpp')]");
    }
};

struct parser {
    primitives::sqlite::sqlitemgr db{db_fn()};
    std::map<std::string, page> pages;

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
            p.name = pagename;
            auto &db_p = *db_page_i;
            p.source = db_p.source;
            p.parse_links();
            pages.emplace(pagename, p);
            return;
        }

        std::println("parsing {}", pagename);
        try {
            auto &&[it, _] = pages.emplace(pagename, page{pagename,make_normal_page_url(pagename)});
            auto &p = it->second;

            auto tr = db.scoped_transaction();
            auto page_ins = db.prepared_insert<::db::parser::schema::tables_::page, primitives::sqlite::db::or_ignore{}>();
            page_ins.insert({.name = pagename, .source = p.source});
        } catch (std::exception &e) {
            std::cerr << e.what() << "\n";
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
    void parse(cpp_reference::page_raw &p) {
        p.title = value("id", "firstHeading");

        if (auto nb = find_navbar()) {
            for (auto &&hn : find_navbar_heads(nb.node())) {
                auto &n = p.navbars.emplace_back();
                if (auto nbm = find_navbar_menu(hn.node())) {
                    for (auto &&r : find_navbar_rows(nbm.node())) {
                        auto f = [&](this auto &&f, auto &&r) -> void {
                            constexpr auto usual_row = "t-nv"sv;
                            constexpr auto subtable_heading1 = "t-nv-h1"sv;
                            constexpr auto subtable_heading2 = "t-nv-h2"sv;
                            constexpr auto subtable = "t-nv-col-table"sv;
                            auto extract_text = [](auto &&n) {
                                std::string full_text;
                                for (const auto &item : n.select_nodes(".//text()")) {
                                    full_text += item.node().value() + "\n"s;
                                }
                                boost::trim(full_text);
                                return full_text;
                            };
                            auto extract = [&](auto &&o, auto &&a) {
                                o.name = extract_text(a);
                                o.href = a.attribute("href").as_string();
                            };
                            auto add_link_from_td = [&](auto &&r) {
                                for (auto &&s : find_navbar_cols(r)) {
                                    if (auto a = find_a(s.node())) {
                                        extract(n.items.emplace_back(), a.node());
                                    } else if (auto a = find_strong(s.node())) {
                                        n.items.emplace_back(extract_text(a.node()));
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

        auto contents = find_node("id", "mw-content-text");
        auto n = contents.node().first_child();
        while (n) {
            auto cl = n.attribute("class").as_string();
            if (0) {
            } else if (cl == "t-dcl-begin"sv) {
            } else if (cl == "t-dcl-begin"sv) {
            } else {
                throw;
            }
            n = n.next_sibling();
        }
    }
};


void pages_to_cpp() {
    cppreference_website w;
    primitives::sqlite::sqlitemgr db{ path{mirror_root_dir} += ".db" };
    for (auto &&p : db.select<::db::parser::schema::tables_::page>()) {
        auto &n = p.name.value;
        if (n != "cpp/memory/voidify"sv) {
            continue;
        }
        std::println("{}", n);

        html_page page{ p };
        cpp_reference::page_raw pr;
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
}

int main(int argc, char *argv[]) {
    pages_to_cpp();
    return 0;
}
