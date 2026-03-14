#pragma once

#include <primitives/string.h>

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
    //location l;
    std::string title;
    std::vector<navbar> navbars;
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

struct header {

};

} // namespace cpp_reference

template <typename ... Types>
struct type_list {
    using variant_type = std::variant<Types...>;

    static void for_each(auto &&f) {
        (f((Types**)nullptr),...);
    }
};

using cppreference_objects = type_list<
    cpp_reference::page,
    cpp_reference::c_language_standard_page,
    cpp_reference::compiler_support
>;

struct cppreference_website {
    std::map<std::string, cpp_reference::page_raw> pages;
};
