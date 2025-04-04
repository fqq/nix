#include "nix/command.hh"
#include "nix/store-api.hh"
#include "nix/nar-accessor.hh"
#include "nix/common-args.hh"
#include <nlohmann/json.hpp>

using namespace nix;

struct MixLs : virtual Args, MixJSON
{
    std::string path;

    bool recursive = false;
    bool verbose = false;
    bool showDirectory = false;

    MixLs()
    {
        addFlag({
            .longName = "recursive",
            .shortName = 'R',
            .description = "List subdirectories recursively.",
            .handler = {&recursive, true},
        });

        addFlag({
            .longName = "long",
            .shortName = 'l',
            .description = "Show detailed file information.",
            .handler = {&verbose, true},
        });

        addFlag({
            .longName = "directory",
            .shortName = 'd',
            .description = "Show directories rather than their contents.",
            .handler = {&showDirectory, true},
        });
    }

    void listText(ref<SourceAccessor> accessor)
    {
        std::function<void(const SourceAccessor::Stat &, const CanonPath &, std::string_view, bool)> doPath;

        auto showFile = [&](const CanonPath & curPath, std::string_view relPath) {
            if (verbose) {
                auto st = accessor->lstat(curPath);
                std::string tp =
                    st.type == SourceAccessor::Type::tRegular ?
                        (st.isExecutable ? "-r-xr-xr-x" : "-r--r--r--") :
                    st.type == SourceAccessor::Type::tSymlink ? "lrwxrwxrwx" :
                    "dr-xr-xr-x";
                auto line = fmt("%s %20d %s", tp, st.fileSize.value_or(0), relPath);
                if (st.type == SourceAccessor::Type::tSymlink)
                    line += " -> " + accessor->readLink(curPath);
                logger->cout(line);
                if (recursive && st.type == SourceAccessor::Type::tDirectory)
                    doPath(st, curPath, relPath, false);
            } else {
                logger->cout(relPath);
                if (recursive) {
                    auto st = accessor->lstat(curPath);
                    if (st.type == SourceAccessor::Type::tDirectory)
                        doPath(st, curPath, relPath, false);
                }
            }
        };

        doPath = [&](const SourceAccessor::Stat & st, const CanonPath & curPath,
            std::string_view relPath, bool showDirectory)
        {
            if (st.type == SourceAccessor::Type::tDirectory && !showDirectory) {
                auto names = accessor->readDirectory(curPath);
                for (auto & [name, type] : names)
                    showFile(curPath / name, relPath + "/" + name);
            } else
                showFile(curPath, relPath);
        };

        auto path2 = CanonPath(path);
        auto st = accessor->lstat(path2);
        doPath(st, path2,
            st.type == SourceAccessor::Type::tDirectory ? "." : path2.baseName().value_or(""),
            showDirectory);
    }

    void list(ref<SourceAccessor> accessor)
    {
        if (json) {
            if (showDirectory)
                throw UsageError("'--directory' is useless with '--json'");
            logger->cout("%s", listNar(accessor, CanonPath(path), recursive));
        } else
            listText(accessor);
    }
};

struct CmdLsStore : StoreCommand, MixLs
{
    CmdLsStore()
    {
        expectArgs({
            .label = "path",
            .handler = {&path},
            .completer = completePath
        });
    }

    std::string description() override
    {
        return "show information about a path in the Nix store";
    }

    std::string doc() override
    {
        return
          #include "store-ls.md"
          ;
    }

    void run(ref<Store> store) override
    {
        list(store->getFSAccessor());
    }
};

struct CmdLsNar : Command, MixLs
{
    Path narPath;

    CmdLsNar()
    {
        expectArgs({
            .label = "nar",
            .handler = {&narPath},
            .completer = completePath
        });
        expectArg("path", &path);
    }

    std::string doc() override
    {
        return
          #include "nar-ls.md"
          ;
    }

    std::string description() override
    {
        return "show information about a path inside a NAR file";
    }

    void run() override
    {
        list(makeNarAccessor(readFile(narPath)));
    }
};

static auto rCmdLsStore = registerCommand2<CmdLsStore>({"store", "ls"});
static auto rCmdLsNar = registerCommand2<CmdLsNar>({"nar", "ls"});
