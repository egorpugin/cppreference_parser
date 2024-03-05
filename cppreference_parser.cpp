/*
c++: 23
package_definitions: true
deps:
    - pub.egorpugin.primitives.http
    - pub.egorpugin.primitives.sw.main
    - org.sw.demo.zeux.pugixml
    - org.sw.demo.htacg.tidy_html5
*/

#include <pugixml.hpp>
#include <primitives/http.h>
#include <primitives/sw/main.h>
#include <tidy.h>
#include <tidybuffio.h>

#include <format>
#include <print>

path mirror_root_dir = "cppreference";

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

auto make_page_fn(auto &&pagename) {
    auto pagefn = mirror_root_dir / pagename;
    pagefn = boost::replace_all_copy(pagefn.u8string(), "File:", "File/");
    pagefn = boost::replace_all_copy(pagefn.u8string(), "Template:", "Template/");
#ifdef _WIN32 // always?
    pagefn = boost::replace_all_copy(pagefn.u8string(), ":", "_");
#endif
    return pagefn;
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
    return tidy_html(resp.response);
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
    std::string source;
    std::set<std::string> links;
    std::set<std::string> templates;

    page() = default;
    page(const std::string &url) : text{download_url(url)} {
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
        auto f = [&](auto &&b, auto &&e) {
            for (auto &&l : find_text_between(source, b, e)) {
                if (l.empty()) {
                    continue;
                }
                auto v = split_string(l, "|");
                if (v.empty()) {
                    continue;
                }
                if (false
                    || v[0].contains('{')
                    || v[0].contains('#')
                    ) {
                    continue;
                }
                boost::trim(v[0]);
                links.insert(v[0]);
            }
        };
        f("[["sv, "]]"sv);
        //f("{{"sv, "}}"sv);
    }
};

struct parser {
    std::map<std::string, page> pages;

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
            || pagename.starts_with("Template_talk")
            || pagename.starts_with("File")
            ) {
            return;
        }
        if (pages.contains(pagename)) {
            return;
        }
        auto pagefnbase = make_page_fn(pagename);
        auto pagefn = path{pagefnbase} += ".txt";
        auto pagefn_templates = path{pagefnbase} += ".templates";
        if (fs::exists(pagefn)) {
            page p;
            p.source = read_file(pagefn);
            p.parse_links();
            for (auto &&l : read_lines(pagefn_templates)) {
                p.templates.insert(l);
            }
            pages.emplace(pagename, p);
            return;
        }
        std::println("parsing {}", pagename);
        auto &&[it,_] = pages.emplace(pagename, make_edit_page_url(pagename));
        auto &p = it->second;
        write_file(pagefn, p.source);
        write_lines(pagefn_templates, p.templates);
    }
};

int main(int argc, char *argv[]) {
    parser p;
    p.start();
    return 0;
}
