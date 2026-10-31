# Non-flake development shell.
#
#   nix-shell
#   make
#
{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  packages = [ pkgs.gcc pkgs.gnumake pkgs.linuxHeaders ];

  shellHook = ''
    export KERNEL_HEADERS=${pkgs.linuxHeaders}/include
    echo "ds4xbox dev shell: make test"
  '';
}
