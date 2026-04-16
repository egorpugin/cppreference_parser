void build(Solution &s) {
    auto &parser = s.addExecutable("cppreference_parser");
    {
        auto &t = parser;
        t.PackageDefinitions = true;
        t += cpp26;
        t += "cppreference_parser.cpp";
        t += ".*\\.h"_r;
        t +=
            //"pub.egorpugin.primitives.emitter"_dep,
            "pub.egorpugin.primitives.executor"_dep,
            "pub.egorpugin.primitives.http"_dep,
            "pub.egorpugin.primitives.templates2"_dep,
            "pub.egorpugin.primitives.sw.main"_dep,
            "org.sw.demo.nlohmann.json.natvis"_dep,
            //"org.sw.demo.zeux.pugixml"_dep,
            //"pub.egorpugin.htacg.tidy_html5"_dep,
            "org.sw.demo.sqlite3"_dep,
            "org.sw.demo.boost.pfr"_dep
            ;
    }

    auto &mw_output = s.addExecutable("mediawiki_output");
    {
        auto &t = mw_output;
        t.PackageDefinitions = true;
        t += cpp26;
        t += "mediawiki_output.cpp";
        t += ".*\\.h"_r;
        t -= "generated/.*"_rr;
        t +=
            "pub.egorpugin.primitives.templates2"_dep,
            "pub.egorpugin.primitives.sw.main"_dep
            ;
        /*t.addCommand()
            << cmd::prog(parser)
            << cmd::end()
            << cmd::out("generated/cpp/all.h");
            ;*/
    }
}
