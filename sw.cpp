void build(Solution &s) {
    auto &parser = s.addExecutable("cppreference_parser");
    parser.PackageDefinitions = true;
    parser += cpp26;
    parser += "cppreference_parser.cpp";
    parser += ".*\\.h"_r;
    parser +=
        "pub.egorpugin.primitives.http"_dep,
        "pub.egorpugin.primitives.templates2"_dep,
        "pub.egorpugin.primitives.sw.main"_dep,
        "org.sw.demo.zeux.pugixml"_dep,
        "org.sw.demo.htacg.tidy_html5"_dep,
        "org.sw.demo.sqlite3"_dep,
        "org.sw.demo.boost.pfr"_dep
        ;
}
