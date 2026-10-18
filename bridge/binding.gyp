{
  "targets": [
    {
      "target_name": "shadow_ring",
      "sources": ["shadow_ring.cc"],
      "include_dirs": [
        "<(module_root_dir)/native"
      ],
      "defines": [
        "NAPI_VERSION=8",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "UNICODE",
        "_UNICODE"
      ],
      "conditions": [
        ["OS=='win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "Optimization": 2,
              "ExceptionHandling": 1,
              "RuntimeTypeInfo": "true",
              "AdditionalOptions": ["/std:c++17", "/permissive-"]
            }
          },
          "libraries": ["kernel32.lib"]
        }]
      ]
    }
  ]
}
