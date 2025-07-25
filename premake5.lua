include("tools/build")
require("third_party/premake-export-compile-commands/export-compile-commands")
require("third_party/premake-androidndk/androidndk")
require("third_party/premake-cmake/cmake")

location(build_root)
targetdir(build_bin)
objdir(build_obj)

-- Define variables for enabling specific submodules
-- Todo: Add changing from xb command
enableTests = false
enableMiscSubprojects = false

-- Define an ARCH variable
-- Only use this to enable architecture-specific functionality.
if os.istarget("linux") then
  ARCH = os.outputof("uname -p")
else
  ARCH = "unknown"
end

includedirs({
  ".",
  "src",
  "third_party",
})

defines({
  "_UNICODE",
  "UNICODE",
})

cppdialect("C++20")
exceptionhandling("On")
rtti("On")
symbols("On")
characterset("Unicode")
fatalwarnings("All")

-- TODO(DrChat): Find a way to disable this on other architectures.
if ARCH ~= "ppc64" then
  filter("architecture:x86_64")
    vectorextensions("AVX")
  filter({})
end

filter("kind:StaticLib")
  defines({
    "_LIB",
  })

filter("configurations:Checked")
  runtime("Debug")
  sanitize("Address")
  flags("NoIncrementalLink")
  editandcontinue("Off")
  staticruntime("Off")
  optimize("Off")
  defines({
    "DEBUG",
  })

filter({"configurations:Checked", "platforms:Windows"})
  buildoptions({
    "/RTCsu",           -- Full Run-Time Checks.
  })

filter({"configurations:Checked", "platforms:Linux"})
  defines({
    "_GLIBCXX_DEBUG",   -- libstdc++ debug mode
  })

filter("configurations:Debug")
  runtime("Release")
  optimize("Off")
  defines({
    "DEBUG",
    "_NO_DEBUG_HEAP=1",
  })

filter({"configurations:Debug", "platforms:Linux"})
  defines({
    "_GLIBCXX_DEBUG",   -- make dbg symbols work on some distros
  })

filter("configurations:Release")
  runtime("Release")
  defines({
    "NDEBUG",
    "_NO_DEBUG_HEAP=1",
  })
  optimize("Speed")
  symbols("Off")
  inlining("Auto")
  editandcontinue("Off")
  -- Not using floatingpoint("Fast") - NaN checks are used in some places
  -- (though rarely), overall preferable to avoid any functional differences
  -- between debug and release builds, and to have calculations involved in GPU
  -- (especially anything that may affect vertex position invariance) and CPU
  -- (such as constant propagation) emulation as predictable as possible,
  -- including handling of specials since games make assumptions about them.

filter({"configurations:Release", "platforms:Windows"})
  linktimeoptimization("On")
  symbols("On")
  flags({
    "NoBufferSecurityCheck"
  })
  buildoptions({
    "/Gw",
    "/Ob3",
  })

filter("platforms:Linux")
  system("linux")
  toolset("clang")
  vectorextensions("AVX2")
  buildoptions({
    -- "-mlzcnt",  -- (don't) Assume lzcnt is supported.
  })
  pkg_config.all("gtk+-x11-3.0")
  links({
    "stdc++fs",
    "dl",
    "lz4",
    "pthread",
    "rt",
  })

filter({"platforms:Linux", "kind:*App"})
  linkgroups("On")

filter({"platforms:Linux", "language:C++", "toolset:gcc"})
  disablewarnings({
    "unused-result",
    "deprecated-volatile",
    "switch",
    "deprecated-enum-enum-conversion",
  })

filter({"platforms:Linux", "toolset:gcc"})
  if ARCH == "ppc64" then
    buildoptions({
      "-m32",
      "-mpowerpc64"
    })
    linkoptions({
      "-m32",
      "-mpowerpc64"
    })
  end

filter({"platforms:Linux", "language:C++", "toolset:clang"})
  disablewarnings({
    "deprecated-register",
    "deprecated-volatile",
    "switch",
    "deprecated-enum-enum-conversion",
    "attributes",
  })
filter({"platforms:Linux", "language:C++", "toolset:clang", "files:*.cc or *.cpp"})
  buildoptions({
    "-stdlib=libstdc++",
    "-std=c++20", -- clang doesn't respect cppdialect(?)
  })

filter("platforms:Android-*")
  system("android")
  systemversion("24")
  cppstl("c++")
  staticruntime("On")
  -- Hidden visibility is needed to prevent dynamic relocations in FFmpeg
  -- AArch64 Neon libavcodec assembly with PIC (accesses extern lookup tables
  -- using `adrp` and `add`, without the Global Object Table, expecting that all
  -- FFmpeg symbols that aren't a part of the FFmpeg API are hidden by FFmpeg's
  -- original build system) by resolving those relocations at link time instead.
  visibility("Hidden")
  links({
    "android",
    "dl",
    "log",
  })

filter("platforms:Windows")
  system("windows")
  toolset("msc")
  buildoptions({
    "/utf-8",   -- 'build correctly on systems with non-Latin codepages'.
    -- Disable warnings
    "/wd4201",  -- Nameless struct/unions are ok.
  })
  flags({
    "MultiProcessorCompile",  -- Multiprocessor compilation.
    "NoMinimalRebuild",       -- Required for /MP above.
  })

  defines({
    "_CRT_NONSTDC_NO_DEPRECATE",
    "_CRT_SECURE_NO_WARNINGS",
    "WIN32",
    "_WIN64=1",
    "_AMD64=1",
  })
  linkoptions({
    "/ignore:4006",  -- Ignores complaints about empty obj files.
    "/ignore:4221",
  })
  links({
    "ntdll",
    "wsock32",
    "ws2_32",
    "xinput",
    "comctl32",
    "shcore",
    "shlwapi",
    "dxguid",
    "bcrypt",
  })

-- Embed the manifest for things like dependencies and DPI awareness.
filter({"platforms:Windows", "kind:ConsoleApp or WindowedApp"})
  files({
    "src/xenia/base/app_win32.manifest"
  })

-- Create scratch/ path
if not os.isdir("scratch") then
  os.mkdir("scratch")
end

workspace("xenia")
  uuid("931ef4b0-6170-4f7a-aaf2-0fece7632747")
  startproject("xenia-app")
  if os.istarget("android") then
    platforms({"Android-ARM64", "Android-x86_64"})
    filter("platforms:Android-ARM64")
      architecture("ARM64")
    filter("platforms:Android-x86_64")
      architecture("x86_64")
    filter({})
  else
    architecture("x86_64")
    if os.istarget("linux") then
      platforms({"Linux"})
    elseif os.istarget("macosx") then
      platforms({"Mac"})
      xcodebuildsettings({
        ["ARCHS"] = "x86_64"
      })
    elseif os.istarget("windows") then
      platforms({"Windows"})
      -- 10.0.15063.0: ID3D12GraphicsCommandList1::SetSamplePositions.
      -- 10.0.19041.0: D3D12_HEAP_FLAG_CREATE_NOT_ZEROED.
      -- 10.0.22000.0: DWMWA_WINDOW_CORNER_PREFERENCE.
      filter("action:vs2017")
        systemversion("10.0.22000.0")
      filter("action:vs2019")
        systemversion("10.0")
      filter({})
    end
  end
  configurations({"Checked", "Debug", "Release"})

  include("third_party/aes_128.lua")
  include("third_party/capstone.lua")
  include("third_party/dxbc.lua")
  include("third_party/discord-rpc.lua")
  include("third_party/cxxopts.lua")
  include("third_party/tomlplusplus.lua")
  include("third_party/FFmpeg/premake5.lua")
  include("third_party/fmt.lua")
  include("third_party/libcurl.lua")
  include("third_party/glslang-spirv.lua")
  include("third_party/imgui.lua")
  include("third_party/mspack.lua")
  include("third_party/snappy.lua")
  include("third_party/xxhash.lua")
  include("third_party/zarchive.lua")
  include("third_party/zstd.lua")
  include("third_party/zlib.lua")
  include("third_party/pugixml.lua")
  include("third_party/rapidjson.lua")
  include("third_party/miniupnp.lua")
  include("third_party/wolfssl.lua")

  if os.istarget("windows") then
    include("third_party/libusb.lua")
  end

  if not os.istarget("android") then
    -- SDL2 requires sdl2-config, and as of November 2020 isn't high-quality on
    -- Android yet, most importantly in game controllers - the keycode and axis
    -- enums are being ruined during conversion to SDL2 enums resulting in only
    -- one controller (Nvidia Shield) being supported, digital triggers are also
    -- not supported; lifecycle management (especially surface loss) is also
    -- complicated.
    include("third_party/SDL2.lua")
  end

  -- Disable treating warnings as fatal errors for all third party projects, as
  -- well as other things relevant only to Xenia itself.
  for _, prj in ipairs(premake.api.scope.current.solution.projects) do
    project(prj.name)
    removefiles({
      "src/xenia/base/app_win32.manifest"
    })
    removefatalwarnings("All")
  end

  include("src/xenia")
  include("src/xenia/app")
  include("src/xenia/app/discord")
  include("src/xenia/apu")
  include("src/xenia/apu/nop")
  include("src/xenia/base")
  include("src/xenia/cpu")
  include("src/xenia/cpu/backend/x64")
  include("src/xenia/debug/ui")
  include("src/xenia/gpu")
  include("src/xenia/gpu/null")
  include("src/xenia/gpu/vulkan")
  include("src/xenia/hid")
  include("src/xenia/hid/nop")
  include("src/xenia/hid/skylander")
  include("src/xenia/kernel")
  include("src/xenia/patcher")
  include("src/xenia/ui")
  include("src/xenia/ui/vulkan")
  include("src/xenia/vfs")

  if not os.istarget("android") then
    include("src/xenia/apu/sdl")
    include("src/xenia/helper/sdl")
    include("src/xenia/hid/sdl")
  end

  if os.istarget("windows") then
    include("src/xenia/apu/xaudio2")
    include("src/xenia/gpu/d3d12")
    include("src/xenia/hid/winkey")
    include("src/xenia/hid/xinput")
    include("src/xenia/ui/d3d12")
  end
