# Force upper/lower case ZMK Module

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ZMK Compatible](https://img.shields.io/badge/ZMK-compatible-blue)](https://zmk.dev/)
[![Version](https://img.shields.io/badge/version-v1.0.0-green)](https://github.com/DoctorMachete/zmk-behavior-force-case)


## Overview

Force-case is a module for allowing more control over how characters are capitalized, specially when Capslock can be toggled by the keyboard but it actually depends on the host. That's what actually determines if Caps lock is actually active or not.

One use case is managing multiple virtual machines, each one having their own independent capslock status. So it can happen that in the host capslock is disabled, then when moving the cursor to VM-A capslock is now on (and it will be reflected on the keyboard if it has some indicator for it like leds or a display) and then without touching the keyboard, just hovering the mouse cursor to another VM capslock might be disabled. 

This module provides four behaviors meant to provide consistent output no matter the state of capslock.
- **flcase** short for _force_lower_case_. This behavior always ignores capslock and will alwazy output lowercase **unless** shift is active, in which case outputs uppercase. 
- **fucase** short for _force_upper_case_. This behavior always ignores capslock and will alwazy output uppercase **unless** shift is active, in which case outputs lowercase. 
- **ftlcase** short for _force_true_lower_case_. This behavior always output lowercase ignoring both capslock and shift.
- **ftucase** short for _force_true_upper_case_. This behavior always output uppercase ignoring both capslock and shift.

## Usage
- No need for _.conf_ settings
- Remote:
  ```
  remotes:
    - name: doctormachete
      url-base: https://github.com/DoctorMachete
  projects:
    - name: zmk-behavior-force-case
      remote: doctormachete
      revision: main
  ```
- Behavior examples:
  ```
  // can be directly placed on the keymap and/or used inside of macros or other behaviors
  &flcase A   aaaaaaa <no shift>   AAAAAA <with shift>    CAPSLOCK doesn't make a difference either case
  &fucase B   BBBBBBB <no shift>   bbbbbb <with shift>    CAPSLOCK doesn't make a difference either case
  &ftlcase C   ccccccc <no shift>   cccccc <with shift>    CAPSLOCK doesn't make a difference either case
  &ftucase D   DDDDDDD <no shift>   DDDDDD <with shift>    CAPSLOCK doesn't make a difference either case
  ```
### Compatibility
- **ZMK 0.3** (Zephyr 3.5): this is the only one tested. And specifically [the Darknao ZMK fork for including the per-layer feature in moergo boards](https://github.com/darknao/zmk/tree/rgb-layer-24.12)
- Made for personal use, **use at your own risk**.

### Notes about development
- I'd say aound 99.9% made with Claude AI, but there were a couple occasions where it got stuck and hit a wall without any progress insisting on A, then on B, then on C... then on A again....
- One of the brick walls that I remember well because it consumed many full sessions of free AI usage were that it couldn't figure out a good way to check the current capslock status. It had the function to get the capslock status itself but the value it tried to use for checking it out wasn't defined. I'm not a programmer but knowing the board can indicate capslock via per-key RGB I was able to get the right constant to compare against.
- Another big brick wall was once that it was almost there, all four functions worked with the exception of sticky shift (with _quick-release_ enabled) not being actually released/consumed after the first key. After asking several times for some of the files (and after uploading them) it got worse. I actually lost all functionality and all four behaviors worked exactly as regular keys, entering a loop where tried many things in a loop but nothing worked. I had to ask the AI to backtrack a little to see if it could figure out with "fresh eyes" but nothing again. Only when I suggested it to look at the _mod-morph_ functionality was when it could figure it out, after asking me to upload the mod-morph source file and examining it.


  


