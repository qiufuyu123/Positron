#include <doctest/doctest.h>
#include <pe_resolver/pe_resolver.h>
#include <Windows.h>

using namespace positron::pe;

TEST_CASE("parse_exports of kernel32 finds CreateFileW") {
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    REQUIRE(k32 != nullptr);
    auto t = parse_exports(k32);
    auto p = t.abs("CreateFileW");
    REQUIRE(p.has_value());
    auto direct = reinterpret_cast<uintptr_t>(::GetProcAddress(k32, "CreateFileW"));
    CHECK(*p == direct);
}

TEST_CASE("find_exports_with returns nullopt for nonsense") {
    auto r = find_exports_with("definitely_not_an_export_xyz_123");
    CHECK_FALSE(r.has_value());
}
