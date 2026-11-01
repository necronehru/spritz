{
  "targets": [
    {
      "target_name": "mpv_render",
      "sources": [ "mpv_addon.cpp" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "/tmp/spritz-libmpv-win-x64/include",
        "/tmp/spritz-electron-x64/42.9.0/include/node"
      ],
      "libraries": [
        "-L/tmp/spritz-electron-import",
        "-lelectron",
        "-L/tmp/spritz-libmpv-win-x64",
        "-lmpv",
        "-ld3d11",
        "-ldxgi",
        "-ldcomp",
        "-ldwmapi"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX"
      ],
      "cflags_cc": [ "-std=c++17" ]
    }
  ]
}
