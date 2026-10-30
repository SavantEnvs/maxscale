// libFuzzer harness for MaxScale's query classifier (pp_sqlite parser plugin + the MariaDB
// protocol helper). The first byte picks the SQL mode, the rest is the statement.
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <maxbase/maxbase.hh>
#include <maxscale/buffer.hh>
#include <maxscale/parser.hh>
#include <maxscale/paths.hh>
#include <maxscale/protocol/mariadb/mariadbparser.hh>
#include <maxscale/protocol/mariadb/mysql.hh>

namespace
{
struct Env
{
    Env()
        : base(MXB_LOG_TARGET_STDERR)
    {
        const char* libdir = getenv("PP_SQLITE_LIBDIR");
        mxs::set_libdir(libdir ? libdir : "/mayhem/build/fuzz/server/modules/parser_plugin/pp_sqlite");
        mxs::set_datadir("/tmp");
        mxs::set_process_datadir("/tmp");

        plugin = mxs::ParserPlugin::load("pp_sqlite");
        if (!plugin || !plugin->setup(mxs::Parser::SqlMode::DEFAULT) || !plugin->thread_init())
        {
            abort();
        }

        parser = plugin->create_parser(&MariaDBParser::Helper::get());
        parser->set_server_version(10 * 10000 + 11 * 100);

        // Prime the plugin's per-thread state with one statement so a crashing input replays the same way
        // in a fresh process as it does after the first fuzz iteration.
        GWBUF warmup = mariadb::create_query("SELECT 1");
        parser->parse(warmup, mxs::Parser::COLLECT_ALL);
    }

    maxbase::MaxBase                base;
    mxs::ParserPlugin*              plugin = nullptr;
    std::unique_ptr<mxs::Parser>    parser;
};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static Env env;

    if (size < 2 || size > 4096)
    {
        return -1;
    }

    mxs::Parser& parser = *env.parser;
    parser.set_sql_mode((data[0] & 1) ? mxs::Parser::SqlMode::ORACLE : mxs::Parser::SqlMode::DEFAULT);

    GWBUF stmt = mariadb::create_query(std::string_view(reinterpret_cast<const char*>(data) + 1, size - 1));

    parser.parse(stmt, mxs::Parser::COLLECT_ALL);

    size_t sink = parser.get_type_mask(stmt) + parser.get_operation(stmt) + parser.get_trx_type_mask(stmt);
    sink += parser.get_table_names(stmt).size();
    sink += parser.get_database_names(stmt).size();
    sink += parser.get_canonical(stmt).size();
    sink += parser.is_multi_stmt(stmt) + parser.relates_to_previous(stmt);
    sink += parser.get_prepare_name(stmt).size();
    sink += parser.get_kill_info(stmt).target.size();

    const mxs::Parser::FieldInfo* fields = nullptr;
    size_t n_fields = 0;
    parser.get_field_info(stmt, &fields, &n_fields);
    for (size_t i = 0; i < n_fields; ++i)
    {
        sink += fields[i].column.size() + fields[i].table.size() + fields[i].database.size();
    }

    const mxs::Parser::FunctionInfo* functions = nullptr;
    size_t n_functions = 0;
    parser.get_function_info(stmt, &functions, &n_functions);
    for (size_t i = 0; i < n_functions; ++i)
    {
        sink += functions[i].name.size() + functions[i].n_fields;
    }

    if (GWBUF* preparable = parser.get_preparable_stmt(stmt))
    {
        sink += parser.get_type_mask(*preparable);
    }

    sink += parser.get_query_info(stmt).type_mask;
    sink += parser.parse_to_resource("localhost", stmt) != nullptr;

    return sink == 0x7fffffff ? -1 : 0;
}
