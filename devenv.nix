{ pkgs, lib, config, inputs, ... }:

{
  packages = [
    pkgs.cmake
    pkgs.gnumake
    pkgs.gcc
    pkgs.opencv4
    pkgs.libexif
    pkgs.sqlite
    pkgs.pkg-config
    pkgs.gtest
  ];

  env.CMAKE_PREFIX_PATH = lib.makeSearchPath "" [
    pkgs.opencv4
    pkgs.libexif
    pkgs.sqlite.dev
  ];

  enterShell = ''
    echo "phig dev environment"
    echo "  cmake: $(cmake --version | head -1)"
    echo "  g++:   $(g++ --version | head -1)"
    echo ""
    echo "Build: cmake -B build && cmake --build build"
  '';
}
