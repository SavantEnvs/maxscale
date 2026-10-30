// libFuzzer harness for maxsimd's SQL canonicalizer and multi-statement detector.
// Runs the SIMD-dispatched and the generic implementations over the same arbitrary input (plus the
// canonical-arguments rebuild) so memory errors in either path surface. The two implementations are
// not required to agree on malformed SQL, so their results are not compared.
#include <cstdint>
#include <string>

#include <maxsimd/canonical.hh>
#include <maxsimd/multistmt.hh>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > 8192)
    {
        return -1;
    }

    std::string sql(reinterpret_cast<const char*>(data), size);

    std::string a = sql;
    std::string b = sql;
    maxsimd::get_canonical(&a);
    maxsimd::generic::get_canonical(&b);

    std::string c = sql;
    maxsimd::CanonicalArgs args;
    maxsimd::get_canonical_args(&c, &args);

    std::string d = sql;
    maxsimd::CanonicalArgs generic_args;
    maxsimd::generic::get_canonical_args(&d, &generic_args);

    std::string rebuilt = maxsimd::canonical_args_to_sql(c, args);
    maxsimd::get_canonical(&rebuilt);
    std::string generic_rebuilt = maxsimd::canonical_args_to_sql(d, generic_args);
    maxsimd::generic::get_canonical(&generic_rebuilt);

    bool multi = maxsimd::is_multi_stmt(sql);
    bool generic_multi = maxsimd::generic::is_multi_stmt(sql);

    std::string old = sql;
    maxsimd::generic::get_canonical_old(&old);

    static volatile size_t sink;
    sink = a.size() + b.size() + old.size() + generic_rebuilt.size() + (multi ? 1 : 0) + (generic_multi ? 1 : 0);
    return 0;
}
