{ nixpkgs ?  import <nixpkgs> {}
, useEGL ? false
}:
with nixpkgs;
let
  f = ffmpeg_4.override {
    openglSupport = true;
    patches = [ ./ffmpeg.diff ];
  };
  egldef = if useEGL then "define" else "undef";

in f.overrideAttrs (o: rec {
  prePatch = ''
    cp ${./vf_gltransition.c} ./libavfilter/vf_gltransition.c
    sed -i "s^define GL_TRANSITION_USING_EGL^${egldef} GL_TRANSITION_USING_EGL^g" ./libavfilter/vf_gltransition.c
  '';
  buildInputs = o.buildInputs ++ [ glew glfw ];
  configureFlags = o.configureFlags ++ [
   "--extra-libs=-lGLEW"
   "--extra-libs=-lglfw"
  ];
})
