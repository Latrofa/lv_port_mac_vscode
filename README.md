# VSCode Simulator project for LVGL

[LVGL](https://github.com/lvgl/lvgl) is written mainly for microcontrollers and embedded systems, however you can run the library **on your PC** as well without any embedded hardware. The code written on PC can be simply copied when your are using an embedded system.

This project is pre-configured for VSCode and should work work on Windows, Linux and MacOs as well. FreeRTOS is also included and can be optionally enabled to better simulate embedded system's behavior.

## Get started
By not trusting this yet.

CMakeLists.txt needs some loving to get it to work on a mac.

I'm playing with the  multimedia sides of things for now. Once thats stable this will revert to a 'Demos' style repo.

### Install SDL and the build tools

- **macOS (Homebrew):** `brew install sdl2 cmake make`
- **Verify Installation:** `sdl2-config --version`, `cmake --version`, `gcc --version`, `g++ --version` (should return the installed version).

### Get the PC project

Clone the PC project and the related sub modules:

```bash
git clone --recursive https://github.com/latrofa/lv_port_mac_vscode
```

## Usage

### Visual Studio Code

1. Be sure you have installed [SDL and the build tools](#install-sdl-and-the-build-tools)
2. Open the project by double clicking on `simulator.code-workspace` or opening it with `File/Open Workspace from File`
3. Install the recommended plugins
4. Click the Run and Debug page on the left, and select `Debug LVGL demo with gdb` from the drop-down on the top. Like this:
![image](https://github.com/lvgl/lv_port_pc_vscode/assets/7599318/f527b235-5718-4949-b5f0-bd807b3a64ba)
5. Click the Play button or hit F5 to start debugging.


#### macOS
### Do not trust this yet
Apple's default clang does not support the `-fsanitize=leak` flag.

to build using the latest version of clang from homebrew, do the following:

1. `brew install llvm` //this is not necessary if you installed xcode...

2. cmd+shift+p and run `Cmake: select a kit`, then `[Scan for kits]`

3. then cmd+shift+p and run `Cmake: select a kit`, select the version of clang you just installed from homebrew (it should say `Using compilers C=/opt/homebrew/opt/llvm/bin/clang ...`)

4. reconfigure by running cmd+shift+p `Cmake: Configure`

5. build using [step 4 above](#visual-studio-code)

### FreeRTOS configuration
This is being removed from this because I'm not dealing with it.

### CMake
This project uses CMake under the hood which can be used without Visula Studio Code too. Just type these in a Terminal when you are in the project's root folder:

```bash
mkdir build
cd build
cmake ..
make -j
```

## Run demos and examples

By default, the widgets demo (`lv_demo_widgets()`) will run. If you want to run a different demo or example from the LVGL library,
simply replace the demo function call in the code with another one—such as `lv_demo_benchmark()` or `lv_example_label_1()`.

```c
int main(int argc, char **argv)
{
  /* ... */
  /* Run the default demo */
  /* To try a different demo or example, replace this with one of: */
  /* - lv_demo_benchmark(); */
  /* - lv_demo_stress(); */
  /* - lv_example_label_1(); */
  /* - etc. */
  lv_demo_widgets();

  while(1) {
      /* ... */
  }
  return 0;
}
```

## Optional library

There are also FreeType and FFmpeg support. You can install these according to the followings:

### Linux

```bash
# FreeType support
wget https://kumisystems.dl.sourceforge.net/project/freetype/freetype2/2.13.2/freetype-2.13.2.tar.xz
tar -xf freetype-2.13.2.tar.xz
cd freetype-2.13.2
make
make install
```

```bash
# FFmpeg support
brew install ffmpeg

### Xquartz
brew install xquartz
```
