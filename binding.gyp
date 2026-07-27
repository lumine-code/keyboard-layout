{
  "targets": [
    {
      "target_name": "keyboard-layout-manager",
      # A ThreadSafeFunction callback that lands while the environment is
      # tearing down (window reload, app quit) cannot run JS. Without these two
      # defines node-addon-api escalates that to napi_fatal_error and aborts the
      # process. The graceful path in Error::ThrowAsJavaScriptException needs
      # NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS *and* NAPI_VERSION >= 10, since
      # below 10 it expects napi_pending_exception instead of napi_cannot_run_js
      # and never matches. napi_build_version is 10 on Node 22+ and Electron 43.
      "defines": [
        "NAPI_VERSION=<(napi_build_version)",
        "NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS",
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.7",
      },
      "msvs_settings": {
        "VCCLCompilerTool": {"ExceptionHandling": 1},
      },
      "sources": [
        "src/keyboard-layout-manager.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "chrome_headers",
      ],
      "conditions": [
        ['OS=="mac"', {
          "sources": [
            "src/keyboard-layout-manager-mac.mm",
          ],
          "link_settings": {
            "libraries": [
              "-framework", "AppKit"
            ]
          }
        }],  # OS=="mac"
        ['OS=="win"', {
          "sources": [
            "src/keyboard-layout-manager-windows.cc",
          ],
          'msvs_settings': {
            'VCCLCompilerTool': {
              'ExceptionHandling': 1,  # /EHsc
              'WarnAsError': 'true',
            },
          },
          'msvs_disabled_warnings': [
            4309,  # 'static_cast': truncation of constant value
            4018,  # signed/unsigned mismatch
            4244,  # conversion from 'type1' to 'type2', possible loss of data
            4267,  # conversion from 'size_t' to 'type', possible loss of data
            4302,  # 'type cast': truncation from 'HKL' to 'UINT'
            4311,  # 'type cast': pointer truncation from 'HKL' to 'UINT'
            4530,  # C++ exception handler used, but unwind semantics are not
                   # enabled
            4506,  # no definition for inline function
            4577,  # 'noexcept' used with no exception handling mode specified
            4996,  # function was declared deprecated
          ],
        }],  # OS=="win"
        ['OS=="linux"', {
          "sources": [
            "src/keyboard-layout-manager-linux.cc",
          ],
          "cflags_cc": ["-std=c++17"],
          "variables": {
            "wayland_available%": "<!(pkg-config --exists wayland-client && echo 1 || echo 0)"
          },
          "conditions": [
            ["wayland_available==1", {
              "link_settings": {
                "libraries": [
                  "-lX11",
                  "-lxkbfile",
                  "-lxkbcommon",
                  "-lwayland-client"
                ]
              },
              "defines": ["HAS_WAYLAND=1"]
            }, {
              "link_settings": {
                "libraries": [
                  "-lX11",
                  "-lxkbfile"
                ]
              },
              "defines": ["HAS_WAYLAND=0"]
            }]
          ]
        }],  # OS=="linux"
        ['OS=="freebsd"', {
          "sources": [
            "src/keyboard-layout-manager-linux.cc",
          ],
          "include_dirs": [
            "<!(node -p \"require('node-addon-api').include_dir\")",
            "/usr/local/include", "/usr/local/include/X11",
          ],
          "ldflags": [
            "-lX11", "-lxkbfile", "-lxkbcommon", "-L/usr/local/lib",
          ],
        }],  # OS=="posix"
      ]
    }
  ]
}
