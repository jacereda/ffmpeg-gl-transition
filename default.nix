{ nixpkgs ?  import <nixpkgs> {}
, useEGL ? true
}:
with nixpkgs;
let
  f = ffmpeg-full.override {
    patches = [ ./ffmpeg.diff ];
  };
  egldef = if useEGL then "define" else "undef";

in f.overrideAttrs (o: rec {
  prePatch = o.prePatch + ''
    cp ${./vf_gltransition.c} ./libavfilter/vf_gltransition.c
    substituteInPlace ./libavfilter/vf_gltransition.c \
      --replace "define GL_TRANSITION_USING_EGL" "${egldef} GL_TRANSITION_USING_EGL"
  '';
  buildInputs = o.buildInputs ++ [ glew glfw ];
})
