#!/usr/bin/env -S bash ../.port_include.sh

port='nix'
version='2.16.1'
files="https://github.com/NixOS/nix/archive/refs/tags/${version}.tar.gz nix-${version}.tar.gz dcea9d91faf5cbe4198a45bf4f4a325902bc7056d04fb44eefbcdf8b8fc46f18"
useconfigure='true'
use_fresh_config_sub='true'
config_sub_paths=('config/config.sub')
configopts=('--disable-cpuid' '--disable-tests' '--disable-doc-gen' '--disable-largefile' "--with-boost=${SERENITY_INSTALL_ROOT}/usr/local")
auth_type='sha256'
depends=(
    'bdwgc'
    'openssl'
    'brotli'
    'curl'
    'sqlite'
    'boost'
    'editline'
    'libsodium'
    'libarchive'
    'nlohmann-json'
    'lowdown'
)

pre_configure() {
    run ./bootstrap.sh
}
