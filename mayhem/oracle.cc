// Known-answer oracle for test.sh: classifies fixed statements through the pp_sqlite parser plugin
// and the maxsimd canonicalizer and compares against hard-coded expected results.
// Prints one "PASS <name>" / "FAIL <name>: <why>" line per check.
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <maxbase/maxbase.hh>
#include <maxscale/buffer.hh>
#include <maxscale/parser.hh>
#include <maxscale/paths.hh>
#include <maxscale/protocol/mariadb/mariadbparser.hh>
#include <maxscale/protocol/mariadb/mysql.hh>
#include <maxsimd/canonical.hh>
#include <maxsimd/multistmt.hh>

namespace
{
struct Case
{
    const char* sql;
    const char* op;
    const char* type;
    const char* tables;
    const char* fields;
    const char* canonical;
    bool        multi;
};

const Case cases[] = {
    // CASES-BEGIN
    {"SELECT a, b FROM db1.tbl1 WHERE c = 42", "sql::OP_SELECT", "sql::TYPE_READ", "db1.tbl1", "..a,..b,..c", "SELECT a, b FROM db1.tbl1 WHERE c = ?", false},
    {"select * from t1 join t2 on t1.id = t2.id where t2.name = 'bob'", "sql::OP_SELECT", "sql::TYPE_READ", ".t1,.t2", ".t1.id,.t2.id,..*,.t2.name", "select * from t1 join t2 on t1.id = t2.id where t2.name = ?", false},
    {"INSERT INTO accounts (id, name) VALUES (1, 'x')", "sql::OP_INSERT", "sql::TYPE_WRITE", ".accounts", "..id,..name", "INSERT INTO accounts (id, name) VALUES (?, ?)", false},
    {"UPDATE db2.inventory SET qty = qty - 1 WHERE sku = 'A-1'", "sql::OP_UPDATE", "sql::TYPE_WRITE", "db2.inventory", "..qty,..sku", "UPDATE db2.inventory SET qty = qty - ? WHERE sku = ?", false},
    {"DELETE FROM sessions WHERE expires < NOW()", "sql::OP_DELETE", "sql::TYPE_WRITE", ".sessions", "..expires", "DELETE FROM sessions WHERE expires < NOW()", false},
    {"CREATE TABLE test.t (id INT PRIMARY KEY, v VARCHAR(10))", "sql::OP_CREATE_TABLE", "sql::TYPE_WRITE", "test.t", "", "CREATE TABLE test.t (id INT PRIMARY KEY, v VARCHAR(?))", false},
    {"DROP TABLE IF EXISTS t9", "sql::OP_DROP_TABLE", "sql::TYPE_WRITE", ".t9", "", "DROP TABLE IF EXISTS t9", false},
    {"BEGIN", "sql::OP_UNDEFINED", "sql::TYPE_BEGIN_TRX", "", "", "BEGIN", false},
    {"COMMIT", "sql::OP_UNDEFINED", "sql::TYPE_COMMIT", "", "", "COMMIT", false},
    {"SET autocommit=0", "sql::OP_SET", "sql::TYPE_SESSION_WRITE|sql::TYPE_BEGIN_TRX|sql::TYPE_DISABLE_AUTOCOMMIT", "", "", "SET autocommit=?", false},
    {"USE mydb", "sql::OP_CHANGE_DB", "sql::TYPE_SESSION_WRITE", "", "", "USE mydb", false},
    {"SELECT LAST_INSERT_ID()", "sql::OP_SELECT", "sql::TYPE_READ|sql::TYPE_MASTER_READ", "", "", "SELECT LAST_INSERT_ID()", false},
    {"SELECT @@global.max_allowed_packet", "sql::OP_SELECT", "sql::TYPE_READ", "", "", "SELECT @@global.max_allowed_packet", false},
    {"SELECT a FROM t1; SELECT b FROM t2", "sql::OP_SELECT", "sql::TYPE_READ", ".t1", "..a", "SELECT a FROM t1; SELECT b FROM t2", true},
    {"SELECT COUNT(*), MAX(price) FROM shop.items GROUP BY category", "sql::OP_SELECT", "sql::TYPE_READ", "shop.items", "..price,..category", "SELECT COUNT(*), MAX(price) FROM shop.items GROUP BY category", false},
    {"SHOW DATABASES", "sql::OP_SHOW_DATABASES", "sql::TYPE_READ", "", "", "SHOW DATABASES", false},
    {"KILL 12", "sql::OP_KILL", "sql::TYPE_WRITE", "", "", "KILL ?", false},
    {"SELECT 1 /* a comment */ FROM dual", "sql::OP_SELECT", "sql::TYPE_READ", "", "", "SELECT ?  FROM dual", false},
    {"GRANT ALL ON db.* TO 'u'@'h'", "sql::OP_GRANT", "sql::TYPE_WRITE", "", "", "GRANT ALL ON db.* TO ?@?", false},
    {"SELECT * FROM t1 FOR UPDATE", "sql::OP_SELECT", "sql::TYPE_READ|sql::TYPE_WRITE", ".t1", "..*", "SELECT * FROM t1 FOR UPDATE", false},
    {"LOAD DATA INFILE 'x' INTO TABLE t", "sql::OP_LOAD", "sql::TYPE_WRITE", ".t", "", "LOAD DATA INFILE ? INTO TABLE t", false},
    // CASES-END
};

int failures = 0;

void report(const std::string& name, const std::string& got, const std::string& want)
{
    if (got == want)
    {
        printf("PASS %s\n", name.c_str());
    }
    else
    {
        printf("FAIL %s: got [%s] want [%s]\n", name.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    bool dump = argc > 1 && std::string(argv[1]) == "--dump";

    maxbase::MaxBase base(MXB_LOG_TARGET_STDERR);
    const char* libdir = getenv("PP_SQLITE_LIBDIR");
    mxs::set_libdir(libdir ? libdir : "../pp_sqlite");
    mxs::set_datadir("/tmp");
    mxs::set_process_datadir("/tmp");

    mxs::ParserPlugin* plugin = mxs::ParserPlugin::load("pp_sqlite");
    if (!plugin || !plugin->setup(mxs::Parser::SqlMode::DEFAULT) || !plugin->thread_init())
    {
        printf("FAIL load-plugin\n");
        return 1;
    }

    auto parser = plugin->create_parser(&MariaDBParser::Helper::get());
    parser->set_server_version(10 * 10000 + 11 * 100);

    std::vector<Case> sql_cases(std::begin(cases), std::end(cases));
    if (dump)
    {
        sql_cases.clear();
        for (int i = 2; i < argc; ++i)
        {
            sql_cases.push_back(Case {argv[i], "", "", "", "", "", false});
        }
    }

    for (const auto& c : sql_cases)
    {
        GWBUF stmt = mariadb::create_query(c.sql);
        parser->parse(stmt, mxs::Parser::COLLECT_ALL);

        std::string op = mxs::sql::to_string(parser->get_operation(stmt));
        std::string type = mxs::Parser::type_mask_to_string(parser->get_type_mask(stmt));

        std::ostringstream tables;
        for (const auto& t : parser->get_table_names(stmt))
        {
            tables << (tables.tellp() ? "," : "") << t.db << "." << t.table;
        }

        std::ostringstream fields;
        const mxs::Parser::FieldInfo* fi = nullptr;
        size_t n = 0;
        parser->get_field_info(stmt, &fi, &n);
        for (size_t i = 0; i < n; ++i)
        {
            fields << (i ? "," : "") << fi[i].database << "." << fi[i].table << "." << fi[i].column;
        }

        std::string canonical(c.sql);
        maxsimd::get_canonical(&canonical);
        bool multi = maxsimd::is_multi_stmt(c.sql);

        if (dump)
        {
            printf("    {\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", %s},\n", c.sql, op.c_str(),
                   type.c_str(), tables.str().c_str(), fields.str().c_str(), canonical.c_str(),
                   multi ? "true" : "false");
            continue;
        }

        std::string name = c.sql;
        report("op: " + name, op, c.op);
        report("type: " + name, type, c.type);
        report("tables: " + name, tables.str(), c.tables);
        report("fields: " + name, fields.str(), c.fields);
        report("canonical: " + name, canonical, c.canonical);
        report("multi: " + name, multi ? "true" : "false", c.multi ? "true" : "false");
    }

    plugin->thread_end();
    return failures ? 1 : 0;
}
