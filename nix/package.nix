{ lib, stdenv, linuxHeaders }:

let
  # Keep local build artifacts and the nix-build "result" symlink out of the
  # source tree.  Without this, a second nix-build would copy the store path
  # through the symlink and produce a different derivation each time.
  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let
        name = baseNameOf (toString path);
      in
      lib.cleanSourceFilter path type
      && !(builtins.elem name [
        "result"
        "build"
        "ds4xbox"
        "fake_ds4"
        "read_pad"
      ]);
  };
in
stdenv.mkDerivation {
  pname = "ds4xbox";
  version = "1.0.0";

  inherit src;

  buildInputs = [ linuxHeaders ];

  makeFlags = [
    "KERNEL_HEADERS=${linuxHeaders}/include"
  ];

  preBuild = ''
    make clean
  '';

  installPhase = ''
    runHook preInstall

    make install PREFIX=$out KERNEL_HEADERS=${linuxHeaders}/include

    install -Dm644 udev/72-ds4xbox.rules $out/lib/udev/rules.d/72-ds4xbox.rules
    install -Dm644 systemd/ds4xbox.service.in $out/lib/systemd/system/ds4xbox.service
    substituteInPlace $out/lib/systemd/system/ds4xbox.service \
      --replace-fail '@BINDIR@' "$out/bin"

    install -Dm644 README.md $out/share/doc/ds4xbox/README.md
    install -Dm644 LICENSE $out/share/doc/ds4xbox/LICENSE
    install -Dm644 docs/ARCHITECTURE.md $out/share/doc/ds4xbox/ARCHITECTURE.md
    install -Dm644 docs/TROUBLESHOOTING.md $out/share/doc/ds4xbox/TROUBLESHOOTING.md
    install -Dm644 docs/COMPATIBILITY.md $out/share/doc/ds4xbox/COMPATIBILITY.md

    runHook postInstall
  '';

  enableParallelBuilding = true;

  meta = with lib; {
    description = "DualShock 4 / DualSense to Xbox 360/One remapper for Linux";
    license = licenses.unlicense;
    platforms = platforms.linux;
    mainProgram = "ds4xbox";
  };
}
