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

int main(int argc, char *argv[]) {
    parser p;
    p.start();
    return 0;
}
