{
  description = "p9wl - Wayland compositor for Plan 9";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        
        buildInputs = with pkgs; [
          wlroots_0_19
          wayland
          lz4
          zlib
          wayland-protocols
          libxkbcommon
          pixman
          fftwFloat
          openssl
        ];
        
        nativeBuildInputs = with pkgs; [
          pkg-config
          gcc
          autoPatchelfHook
        ];
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "p9wl";
          version = "0.1.0";
          
          src = ./.;
          
          inherit buildInputs nativeBuildInputs;
          
          buildPhase = ''
            make
          '';
          
          installPhase = ''
            mkdir -p $out/bin
            cp p9wl $out/bin/
          '';
        };

        devShells.default = pkgs.mkShell {
          buildInputs = buildInputs ++ nativeBuildInputs;
          
          shellHook = ''
            export C_INCLUDE_PATH="${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell:$C_INCLUDE_PATH"
          '';
        };
      }
    );
}
