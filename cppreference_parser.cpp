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
        struct templates {
            type<int64_t, primary_key{}, autoincrement{}> template_id;
            type<std::string, unique{}> name;
        } templates_;
        struct page_template {
            type<int64_t, primary_key{}, autoincrement{}> page_template_id;
            foreign_key<page, &page::page_id> page_id;
            foreign_key<templates, &templates::template_id> template_id;
        } page_template_;
    } tables;
};

} // namespace db::parser

const path mirror_root_dir = "cppreference";

auto url_base = "cppreference.com"s;
auto lang = "en"s;
auto protocol = "https"s;
auto normal_page = "w"s;
auto edit_page = "mwiki"s;
auto start_page = "Main_Page"s;

auto make_base_url() {
    return std::format("{}://{}.{}", protocol, lang, url_base);
}
auto make_normal_page_url(auto &&page) {
    return std::format("{}/{}/{}", make_base_url(), normal_page, primitives::http::url_encode(page));
}
auto make_edit_page_url(auto &&page) {
    return std::format("{}/{}/index.php?title={}&action=edit", make_base_url(), edit_page, primitives::http::url_encode(page));
}

auto db_fb() {
    return path{mirror_root_dir} += ".db";
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

auto find_text_between(auto &&text, auto &&from, auto &&to) {
    std::vector<std::string> fragments;
    auto pos = text.find(from);
    while (pos != -1) {
        auto pose = text.find(to, pos);
        if (pose == -1) {
            break;
        }
        fragments.emplace_back(text.substr(pos + from.size(), pose - (pos + from.size())));
        pos = text.find(from, pos + 1);
    }
    return fragments;
}

struct page {
    std::string text;
    std::string name;
    std::string source;
    std::set<std::string> links;
    std::set<std::string> templates;

    page() = default;
    page(const std::string &name, const std::string &url) : name{name}, text{tidy_html(download_url(url))} {
        pugi::xml_document doc;
        if (auto r = doc.load_buffer(text.data(), text.size()); !r) {
            throw std::runtime_error{std::format("url = {}, xml parse error = {}", url, r.description())};
        }
        if (auto n = doc.select_node("//textarea[@name='wpTextbox1']"); n) {
            source = n.node().first_child().text().as_string();
        }
        for (auto &&n : doc.select_nodes("//div[@class='templatesUsed']//li")) {
            auto a = n.node().select_node("a");
            if (a) {
                templates.insert(a.node().first_child().text().as_string());
            }
        }
        parse_links();
    }
    void parse_links() {
        for (auto &&l : find_text_between(source, "[["sv, "]]"sv)) {
            if (l.empty()) {
                continue;
            }
            auto v = split_string(l, "|");
            if (v.empty()) {
                continue;
            }
            auto &link = v[0];
            if (false
                || link.contains('{')
                || link.contains('#')
                || link.contains('(')
                || link.contains('<')
                ) {
                continue;
            }
            boost::trim(link);
            links.insert(link);
        }

        // actually there is no visible mapping between link or link text and pages
        // see https://en.cppreference.com/w/c/23 and <stdnoreturn.h> for this
        // it leads to https://en.cppreference.com/w/c/language/_Noreturn instead
        //
        // so we need to:
        // 1. parse all page
        // 2. find <a> links to this site
        // 3. parse page name from it, check for c/ or cpp/ start
        // 4. save its source (or full page?)
        // "//a[starts-with(@href,'/w/c')]"
        // "//a[starts-with(@href,'/w/cpp')]"
        for (auto &&l : find_text_between(source, "{{"sv, "}}"sv)) {
            if (l.empty()) {
                continue;
            }
            auto v = split_string(l, "|");
            if (v.size() < 2) {
                continue;
            }
            auto &func = v[0];
            boost::trim(func);
            if (!(false
                || func.starts_with("dsc"sv)
                || func.starts_with("ltt"sv) // type
                || func.starts_with("ltf"sv) // function
                || func.starts_with("lc"sv)
                || func.starts_with("lt"sv)
                || func.starts_with("ls"sv)
                || func.starts_with("tt"sv) // type
                || func.starts_with("header"sv)
                || func.starts_with("attr"sv)
                )) {
                continue;
            }
            auto &link = v[1];
            boost::trim(link);
            boost::replace_all(link, "dsc ", "");
            if (false
                || link.contains('{')
                || link.contains('#')
                || link.contains('(')
                || link.contains('<')
                ) {
                continue;
            }
            if (func == "attr") {
                if (is_c_page()) {
                    link = "c/language/attributes/" + link;
                }
                if (is_cpp_page()) {
                    link = "cpp/language/attributes/" + link;
                }
            }
            if (func == "header") {
                if (is_c_page()) {
                    link = "c/header/" + link;
                }
                if (is_cpp_page()) {
                    link = "cpp/header/" + link;
                }
            }
            links.insert(link);
        }
    }
    bool is_c_page() const { return name.starts_with("c/"); }
    bool is_cpp_page() const { return name.starts_with("cpp/"); }
};

struct parser {
    primitives::sqlite::sqlitemgr db{db_fb()};
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
                for (auto &&t : p.templates) {
                    parse_page(t);
                }
            }
            if (pages.size() == old) {
                break;
            }
        }
    }
    void parse_page(auto &&pagename) {
        if (false
            || pagename.starts_with("Talk")
            || pagename.starts_with("Template talk")
            || pagename.starts_with("User")
            //|| pagename.starts_with("User talk") // we skip all users
            || pagename.starts_with("File")
            // langs
            || pagename.starts_with("ar:")
            || pagename.starts_with("cs:")
            || pagename.starts_with("de:")
            || pagename.starts_with("es:")
            || pagename.starts_with("fr:")
            || pagename.starts_with("it:")
            || pagename.starts_with("ja:")
            || pagename.starts_with("ko:")
            || pagename.starts_with("pl:")
            || pagename.starts_with("pt:")
            || pagename.starts_with("ru:")
            || pagename.starts_with("tr:")
            || pagename.starts_with("zh:")
            ) {
            return;
        }
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
            for (auto &&t : db.select<::db::parser::schema::tables_::page_template,
                                      &::db::parser::schema::tables_::page_template::page_id>(db_p.page_id)) {
                auto tt = *db.select<::db::parser::schema::tables_::templates,
                                     &::db::parser::schema::tables_::templates::template_id>(t.template_id)
                               .begin();
                p.templates.insert(tt.name);
            }
            pages.emplace(pagename, p);
            return;
        }

        /*if (!(pagename.starts_with("c/") || pagename.starts_with("cpp/"))) {
            return;
        }*/

        std::println("parsing {}", pagename);
        try {
            auto &&[it, _] = pages.emplace(pagename, page{pagename,make_edit_page_url(pagename)});
            auto &p = it->second;

            auto tr = db.scoped_transaction();
            auto templates_ins = db.prepared_insert<::db::parser::schema::tables_::templates, primitives::sqlite::db::or_ignore{}>();
            auto page_ins = db.prepared_insert<::db::parser::schema::tables_::page, primitives::sqlite::db::or_ignore{}>();
            auto page_template_ins = db.prepared_insert<::db::parser::schema::tables_::page_template, primitives::sqlite::db::or_ignore{}>();
            auto [page_id,pn] = page_ins.insert({.name = pagename, .source = p.source});
            for (auto &&t : p.templates) {
                auto [template_id,tn] = templates_ins.insert({.name = t});
                page_template_ins.insert({.page_id=page_id, .template_id=template_id});
            }
        } catch (std::exception &e) {
            std::cerr << e.what() << "\n";
        }
    }
};

int main(int argc, char *argv[]) {
    parser p;
    p.start();
    return 0;
}
