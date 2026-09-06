{
  description = "NixOS-native low-latency streaming voice input";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      version = builtins.head (pkgs.lib.splitString "\n" (builtins.readFile ./VERSION));
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "voice-input";
        inherit version;
        src = self;

        nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
        buildInputs = with pkgs; [ pipewire ];
        doCheck = true;

        postInstall = ''
          substituteInPlace "$out/lib/systemd/user/voice-inputd.service" \
            --replace-fail '@out@' "$out"
        '';
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          cmake
          ninja
          pkg-config
          gcc
          gdb
          clang-tools
          pipewire
        ];
      };
    };
}
