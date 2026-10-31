# Non-flake entry point.
#
#   nix-build
#   ./result/bin/ds4xbox --self-test
#
# For NixOS, import ./nix/module.nix directly from configuration.nix:
#
#   imports = [ /path/to/PS4toXBOX360remapper/nix/module.nix ];
#   services.ds4xbox.enable = true;
#
{ pkgs ? import <nixpkgs> { } }:

pkgs.callPackage ./nix/package.nix { }
