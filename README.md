# VU Meter Skin for Audacious (Qt)

A modern Qt6 port of the classic XMMS/Audacious VU Meter plugin, rewritten and optimized for **Audacious 4.6.1+** on Windows using the MinGW-w64 / MSYS2 toolchain.

Originally based on legacy GTK2 source code (inspired by the classic [XMMS VU Meter Plugin](https://audio-file.org/2017/06/28/audacious-xmms-vu-meter-plugin/)), this version has been completely refactored to integrate cleanly with Audacious's modern Qt interface framework.

## Features
- **Modern Qt6 Native Rendering:** Fully ported away from deprecated GTK2 dependencies to run smoothly with Qt6.
- **Audacious 4.6.1 ABI Compatible:** Built against contemporary header and library structures using Meson and Ninja.
- **Lightweight & Responsive:** Retains the vintage dual-needle aesthetic while leveraging hardware-accelerated UI rendering.

## Preview
![VU Meter Preview](preview.png)

## Building from Source

To compile this plugin from source, ensure you have an active MSYS2 MINGW64 environment with Audacious 4.6.1 development headers installed.

1. **Clone the repository into your Audacious plugins source tree:**
   ```bash
   cd /c/audacious-plugins-audacious-plugins/src/
   git clone https://github.com/YOUR_USERNAME/vumeter-skin-qt.git
   ```

2. **Register the plugin in the parent `src/meson.build`:**
   Add your subdirectory check under the Qt interface block:
   ```meson
   if get_option('qt')
     subdir('vumeter-skin-qt')
   endif
   ```

3. **Configure and Compile via Meson:**
   ```bash
   cd /c/audacious-plugins-audacious-plugins
   meson setup build --prefix=/mingw64 --reconfigure
   meson compile -C build
   ```

## Installation
Copy the compiled `vumeter-skin-qt.dll` binary from your build output directory into your active Windows Audacious plugin path:
`C:\Program Files\Audacious\lib\audacious\Plugins\` (or your local prefix equivalent).

## Credits & Acknowledgments
- Original GTK2 plugin and concept from the [XMMS/Audacious VU Meter archive](https://audio-file.org/2017/06/28/audacious-xmms-vu-meter-plugin/).
- Modern Qt6 architecture modernization and build pipeline adaptation.