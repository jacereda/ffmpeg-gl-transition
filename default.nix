{ nixpkgs ?  import <nixpkgs> {}
, useEGL ? true
, ffmpeg ? nixpkgs.ffmpeg_4
}:
with nixpkgs;
let egldef = if useEGL then "define" else "undef";
in ffmpeg.overrideAttrs (o: rec {
  patches = o.patches or [] ++ [ ./ffmpeg.diff ];
  prePatch = o.prePatch or "" + ''
    cp ${./vf_gltransition.c} ./libavfilter/vf_gltransition.c
    substituteInPlace ./libavfilter/vf_gltransition.c \
      --replace "define GL_TRANSITION_USING_EGL" "${egldef} GL_TRANSITION_USING_EGL"
  '';
  buildInputs = o.buildInputs ++ [ glew glfw ];
})
