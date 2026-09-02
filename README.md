# Menu Studio

Pauses the world when you open a menu in Skyrim and keeps your character live
and posed in a clean studio. SKSE plugin for Special Edition and Anniversary
Edition.

https://www.nexusmods.com/skyrimspecialedition/mods/185362

## Requirements

* [CMake](https://cmake.org/)
	* Add this to your `PATH`
* [Vcpkg](https://github.com/microsoft/vcpkg)
	* Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
* [Visual Studio Community 2022](https://visualstudio.microsoft.com/)
	* Desktop development with C++

## User Requirements

* Skyrim SE 1.5.97, or AE 1.6.317 and later. VR is not supported.
* [SKSE64](https://skse.silverlock.org/) for your runtime
* [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
	* The SE or the AE database, whichever matches your game
* One mod that puts your character in the menu, because Menu Studio stages that
  view rather than adding it:
	* [Show Player In Menus](https://www.nexusmods.com/skyrimspecialedition/mods/81291), or
	* [Show Player In Inventory](https://www.nexusmods.com/skyrimspecialedition/mods/178689)
* [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603), optional, for the in-game settings panel. Everything is configurable through the INI without it.

## Building

```
git clone https://github.com/maartenharms/menu-studio.git
cd menu-studio
cmake --preset release
cmake --build build/release
```

## License

[GPL-3.0](LICENSE)
