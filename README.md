# @lumine-code/keyboard-layout

Reads and observes the current keyboard layout on Windows, macOS, and Linux.

## Features

- **Cross-platform layouts**: reports the active layout on Windows, macOS, X11, and Wayland.
- **Change observation**: notifies consumers when the operating system changes the active layout.
- **Keymap translation**: maps DOM keyboard codes to characters for supported modifier states.

## Installation

```sh
npm install @lumine-code/keyboard-layout
```

## Usage

To get the current keyboard layout, call `getCurrentKeyboardLayout`. It returns the string identifier of the current layout based on the value returned by the operating system.

```js
const KeyboardLayout = require('@lumine-code/keyboard-layout')
KeyboardLayout.getCurrentKeyboardLayout() // => "com.apple.keylayout.Dvorak"
```

If you want to watch for layout changes, use `onDidChangeCurrentKeyboardLayout` or `observeCurrentKeyboardLayout`. They work the same, except `observeCurrentKeyboardLayout` invokes the given callback immediately with the current layout value and then again next time it changes, whereas `onDidChangeCurrentKeyboardLayout` only invokes the callback on the next change.

```js
const KeyboardLayout = require('@lumine-code/keyboard-layout')
subscription = KeyboardLayout.observeCurrentKeyboardLayout((layout) => console.log(layout))
subscription.dispose() // to unsubscribe later
```

To return characters for various modifier states based on a DOM 3 `KeyboardEvent.code` value and the current system keyboard layout, use `getCurrentKeymap()`:

```js
const KeyboardLayout = require('@lumine-code/keyboard-layout')
KeyboardLayout.getCurrentKeymap()['KeyS']
/*
On a US layout, this returns:
{
  unmodified: 's',
  withShift: 'S',
  withAltGraph: 'ß',
  withShiftAltGraph: 'Í'
}
*/
```


## Caveats

### All platforms

* Having an active layout change listener (via `onDidChangeCurrentKeyboardLayout` or `observeCurrentKeyboardLayout`) will not prevent the process from exiting.

### Linux

On Linux, there is minimal X11 support and somewhat more comprehensive Wayland support.

#### Both Wayland and X11

* There is no distinction between `getCurrentKeyboardLayout` and `getCurrentKeyboardLanguage`; the latter is an alias of the former.

#### X11

* `onDidChangeCurrentKeyboardLayout` and `observeCurrentKeyboardLayout` are no-ops; we do not receive notifications when the keyboard layout changes. If you want to detect if the keyboard layout has changed, you must poll periodically.
* `getCurrentKeymap` will return objects with only two properties: `unmodified` and `withShift`. Information is not available about which characters would be produced if `AltGr` or `AltGr+Shift` were pressed.

#### Wayland

* When Wayland support is enabled (as it will be if `libwayland-client` and `libxkbcommon` are present), this library will assume a Wayland environment is present, then fall back to X11 if Wayland setup fails in any way.
* In a Wayland environment, `observeCurrentKeyboardLayout` works. However…
* …the Wayland server (as implemented by your window manager) is the authority on when keyboard layouts change, and this might not align with your expectations. For instance, GNOME will change the active layout if you have several layouts and reorder them in your settings; it _will not_ change the active layout if you use keyboard shortcuts to switch “input sources” on the fly. Other window mangers may behave differently.

## Contributing

Got ideas to make this package better, found a bug, or want to help add new features? Just drop your thoughts on GitHub. Any feedback is welcome!
