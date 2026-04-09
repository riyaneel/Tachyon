{
	"targets": [
		{
	      "target_name": "tachyon_node",
	      "sources": [
	        "src/native/tachyon_node.cpp",
	        "src/native/_core_local/src/arena.cpp",
	        "src/native/_core_local/src/shm.cpp",
	        "src/native/_core_local/src/transport_uds.cpp",
	        "src/native/_core_local/src/tachyon_c.cpp"
	      ],
	      "include_dirs": [
	        "<!(node -p \"require('node-addon-api').include\")",
	        "src/native/_core_local/include"
	      ],
	      "dependencies": [
	        "<!(node -p \"require('node-addon-api').gyp\")"
	      ],
	      "defines": [
	        "NAPI_VERSION=8"
	      ],
	      "cflags!": [ "-fno-exceptions", "-fno-rtti" ],
	      "cflags_cc!": [ "-fno-exceptions", "-fno-rtti" ],
	      "cflags_cc": [
	        "-std=c++23",
	        "-O3"
	      ],
	      "conditions": [
	        ["OS=='linux'", {
	          "libraries": [
	            "-lrt",
	            "-lpthread"
	          ]
	        }],
	        ["OS=='mac'", {
	          "xcode_settings": {
	            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
	            "CLANG_CXX_LIBRARY": "libc++",
	            "CLANG_CXX_LANGUAGE_STANDARD": "c++23",
	            "OTHER_CPLUSPLUSFLAGS": [
	              "-std=c++2b",
	              "-O3"
	            ]
	          }
	        }]
	      ]
        }
    ]
}
