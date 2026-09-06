{
  description = "NixOS-native low-latency streaming voice input";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      version = builtins.head (pkgs.lib.splitString "\n" (builtins.readFile ./VERSION));
      modelArchive = pkgs.fetchurl {
        url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2";
        hash = "sha256-J/+9nuJK0YbZmswvY1TXmSsnvKtJCBJRBmX6j5OJxfg=";
      };
      streamingModel = pkgs.runCommand "voice-input-streaming-zipformer-zh-en" {
        nativeBuildInputs = [ pkgs.bzip2 pkgs.gnutar ];
      } ''
        mkdir -p "$out"
        model=sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20
        tar -xjf ${modelArchive} -C "$out" --strip-components=1 \
          "$model/encoder-epoch-99-avg-1.int8.onnx" \
          "$model/decoder-epoch-99-avg-1.onnx" \
          "$model/joiner-epoch-99-avg-1.int8.onnx" \
          "$model/tokens.txt" \
          "$model/test_wavs/2.wav"
      '';
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "voice-input";
        inherit version;
        src = self;

        nativeBuildInputs = with pkgs; [
          cmake
          makeWrapper
          ninja
          pkg-config
          qt6.wrapQtAppsHook
        ];
        buildInputs = with pkgs; [
          fcitx5
          fontconfig
          kdePackages.layer-shell-qt
          pipewire
          sherpa-onnx
          qt6.qtbase
          qt6.qtdeclarative
        ];
        doCheck = true;
        cmakeFlags = [
          "-DVOICE_INPUT_TEST_MODEL_DIR=${streamingModel}"
          "-DVOICE_INPUT_TEST_FONTCONFIG=${pkgs.fontconfig.out}/etc/fonts/fonts.conf"
          "-DVOICE_INPUT_TEST_QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
          "-DVOICE_INPUT_TEST_QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/lib/qt-6/plugins"
        ];

        postInstall = ''
          substituteInPlace "$out/lib/systemd/user/voice-inputd.service" \
            --replace-fail '@out@' "$out"
          substituteInPlace "$out/lib/systemd/user/voice-input-overlay.service" \
            --replace-fail '@out@' "$out"
          wrapProgram "$out/bin/voice-inputd" \
            --set-default VOICE_INPUT_MODEL_DIR ${streamingModel}
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
          fcitx5
          sherpa-onnx
          qt6.qtbase
          qt6.qtdeclarative
          kdePackages.layer-shell-qt
        ];
      };
    };
}
