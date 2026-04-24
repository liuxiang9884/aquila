# third party library installed by vcpkg
# fmtlib
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)
# quill - 依赖线程库，所以在找到线程库后查找
find_package(quill CONFIG REQUIRED)
# tomlplusplus
find_package(PkgConfig REQUIRED)
pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET tomlplusplus)
# CLI11
find_package(CLI11 CONFIG REQUIRED)
# magic enum
find_package(magic_enum CONFIG REQUIRED)
# csv parser
find_package(unofficial-vincentlaucsb-csv-parser CONFIG REQUIRED)
# yyjson
find_package(yyjson CONFIG REQUIRED)
# nameof
find_package(nameof CONFIG REQUIRED)
# drogon
find_package(Drogon CONFIG REQUIRED)

# fast-float
find_package(FastFloat CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# absl
find_package(absl CONFIG REQUIRED COMPONENTS btree flat_hash_map)
find_package(OpenSSL REQUIRED)
set(ABSL_LIBS
        absl::flat_hash_map
        absl::btree)


set(THIRD_PARTY_LIBS
        CLI11::CLI11
        PkgConfig::tomlplusplus
        quill::quill
        magic_enum::magic_enum
        fmt::fmt-header-only
        unofficial::vincentlaucsb-csv-parser::csv
        yyjson::yyjson
        nameof::nameof 
        Drogon::Drogon
        FastFloat::fast_float
        OpenSSL::SSL
        OpenSSL::Crypto
        ${ABSL_LIBS}
)
