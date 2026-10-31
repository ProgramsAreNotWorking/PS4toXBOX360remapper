{
  description = "DualShock 4 / DualSense to Xbox 360/One remapper for Linux";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];

      forAllSystems = f:
        nixpkgs.lib.genAttrs systems
          (system: f (import nixpkgs { inherit system; }));
    in
    {
      packages = forAllSystems (pkgs:
        let
          ds4xbox = pkgs.callPackage ./nix/package.nix { };
        in
        {
          default = ds4xbox;
          inherit ds4xbox;
        });

      nixosModules =
        let
          module = import ./nix/module.nix;
        in
        {
          default = module;
          ds4xbox = module;
        };

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = [ pkgs.gcc pkgs.gnumake pkgs.linuxHeaders ];
          shellHook = ''
            export KERNEL_HEADERS=${pkgs.linuxHeaders}/include
            echo "ds4xbox dev shell: make test"
          '';
        };
      });

      checks = forAllSystems (pkgs: {
        default = self.packages.${pkgs.system}.default;
      });
    };
}
