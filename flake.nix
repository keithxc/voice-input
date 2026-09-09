{
  description = "NixOS-native low-latency streaming voice input";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      version = builtins.head (pkgs.lib.splitString "\n" (builtins.readFile ./VERSION));
      vadModel = pkgs.fetchurl {
        url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx";
        sha256 = "9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6";
      };
      modelArchive = pkgs.fetchurl {
        url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2";
        hash = "sha256-J/+9nuJK0YbZmswvY1TXmSsnvKtJCBJRBmX6j5OJxfg=";
      };
      punctuationArchive = pkgs.fetchurl {
        url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/punctuation-models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8.tar.bz2";
        hash = "sha256-wNWqX47raGAyNF4YC+3zkxncLgVWeBxiZLytuoMopuE=";
      };
      # The int8 weights are a quarter of the float ones (75 MB against 294 MB)
      # and load in a fraction of the time; the daemon holds this model for its
      # whole life, so the smaller one is the one to ship.
      punctuationModel = pkgs.runCommand "voice-input-punct-ct-transformer-zh-en" {
        nativeBuildInputs = [ pkgs.bzip2 pkgs.gnutar ];
      } ''
        mkdir -p "$out"
        model=sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8
        tar -xjf ${punctuationArchive} -C "$out" --strip-components=1 \
          "$model/model.int8.onnx" \
          "$model/tokens.json"
      '';
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
      paraformerModel = let
        model = pkgs.fetchurl {
          name = "voice-input-paraformer-int8-model.int8.onnx";
          url = "https://huggingface.co/csukuangfj/sherpa-onnx-paraformer-zh-2024-03-09/resolve/906992d326ebf0c5171cde675aa0902be9e5bc6c/model.int8.onnx";
          hash = "sha256-kLwDA0rhvvlXX4zHmM0VGci+iqnotFigM+MgF/9NWEw=";
        };
        tokens = pkgs.fetchurl {
          name = "voice-input-paraformer-int8-tokens.txt";
          url = "https://huggingface.co/csukuangfj/sherpa-onnx-paraformer-zh-2024-03-09/resolve/906992d326ebf0c5171cde675aa0902be9e5bc6c/tokens.txt";
          hash = "sha256-bA47Nc7OJZgp5stbjZDRPbiPYeo6KVPRGJjksr/XouI=";
        };
      in pkgs.runCommand "voice-input-paraformer-int8" {} ''
        mkdir -p "$out"
        ln -s ${model} "$out/model.int8.onnx"
        ln -s ${tokens} "$out/tokens.txt"
      '';
      sensevoiceModel = let
        model = pkgs.fetchurl {
          name = "voice-input-sensevoice-int8-model.int8.onnx";
          url = "https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/2365baeacb507f821a0c8120fcee3d484dba7a07/model.int8.onnx";
          hash = "sha256-xx8M4AvslbB3ROEWNF4z2Mu+CM74ljgs+Qe/S1GizVE=";
        };
        tokens = pkgs.fetchurl {
          name = "voice-input-sensevoice-int8-tokens.txt";
          url = "https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/2365baeacb507f821a0c8120fcee3d484dba7a07/tokens.txt";
          hash = "sha256-9EnrKNxWdTPX+lm+NOKryoeE93GFDHikf7cxoxQpodw=";
        };
      in pkgs.runCommand "voice-input-sensevoice-int8" {} ''
        mkdir -p "$out"
        ln -s ${model} "$out/model.int8.onnx"
        ln -s ${tokens} "$out/tokens.txt"
      '';
    in {
      packages.${system} = {
        # The models are exposed so a development build can be pointed at the
        # same pinned store paths the wrapper uses, without a second download.
        asr-model = streamingModel;
        punctuation-model = punctuationModel;
        paraformer-model = paraformerModel;
        sensevoice-model = sensevoiceModel;
        vad-model = vadModel;
        default = pkgs.stdenv.mkDerivation {
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
          VOICE_INPUT_VAD_MODEL = vadModel;
          VOICE_INPUT_EMPTY_DRAFT_RESCUE = "1";
          cmakeFlags = [
            "-DVOICE_INPUT_TEST_MODEL_DIR=${streamingModel}"
            "-DVOICE_INPUT_TEST_PARAFORMER_DIR=${paraformerModel}"
            "-DVOICE_INPUT_TEST_SENSEVOICE_DIR=${sensevoiceModel}"
            "-DVOICE_INPUT_TEST_FONTCONFIG=${pkgs.fontconfig.out}/etc/fonts/fonts.conf"
            "-DVOICE_INPUT_TEST_QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
            "-DVOICE_INPUT_TEST_QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/lib/qt-6/plugins"
          ];

          postInstall = ''
            substituteInPlace "$out/lib/systemd/user/voice-inputd.service" \
              --replace-fail '@out@' "$out"
            substituteInPlace "$out/lib/systemd/user/voice-input-overlay.service" \
              --replace-fail '@out@' "$out"
            for program in voice-inputd voice-input-asr-bench; do
              wrapProgram "$out/bin/$program" \
                --set-default VOICE_INPUT_MODEL_DIR ${streamingModel} \
                --set-default VOICE_INPUT_PUNCT_MODEL_DIR ${punctuationModel} \
                --set-default VOICE_INPUT_PARAFORMER_DIR ${paraformerModel} \
                --set-default VOICE_INPUT_SENSEVOICE_DIR ${sensevoiceModel} \
                --set-default VOICE_INPUT_VAD_MODEL ${vadModel}
            done
            '';
        };
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
