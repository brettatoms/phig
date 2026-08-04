{ pkgs, lib, config, inputs, ... }:

{
  packages = [
    pkgs.cmake
    pkgs.gnumake
    pkgs.gcc
    pkgs.pkg-config
    pkgs.gtk4
    pkgs.libadwaita
    pkgs.glib
  ];

  # config.env.HOME does not resolve in this devenv version; fall back to
  # reading it from the environment at eval time.
  env.PKG_CONFIG_PATH = "${builtins.getEnv "HOME"}/.local/lib/pkgconfig";

  enterShell = ''
    echo "phig-gtk dev environment"
    echo "  gtk4:       $(pkg-config --modversion gtk4)"
    echo "  libadwaita: $(pkg-config --modversion libadwaita-1)"
  '';
}
