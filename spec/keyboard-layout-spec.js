'use strict'

const KeyboardLayout = require('../lib/keyboard-layout')

describe('Keyboard Layout', () => {
  if (process.platform === 'darwin' || process.platform === 'win32') {
    describe('.getCurrentKeymap()', function () {
      it('returns characters corresponding to the given DOM 3 keyboard event code based on the current keyboard layout', function () {
        const currentLayout = KeyboardLayout.getCurrentKeyboardLayout()
        switch (currentLayout) {
          case 'com.apple.keylayout.Dvorak':
            expect(KeyboardLayout.getCurrentKeymap()['KeyS']).toEqual({
              unmodified: 'o',
              withShift: 'O',
              withAltGraph: 'ø',
              withAltGraphShift: 'Ø'
            })
            break

          case '00010409':
            expect(KeyboardLayout.getCurrentKeymap()['KeyS']).toEqual({
              unmodified: 'o',
              withShift: 'O',
              withAltGraph: null,
              withAltGraphShift: null
            })
            break

          case 'com.apple.keylayout.US':
            expect(KeyboardLayout.getCurrentKeymap()['KeyS']).toEqual({
              unmodified: 's',
              withShift: 'S',
              withAltGraph: 'ß',
              withAltGraphShift: 'Í'
            })
            break;

          case '00000409':
            expect(KeyboardLayout.getCurrentKeymap()['KeyS']).toEqual({
              unmodified: 's',
              withShift: 'S',
              withAltGraph: null,
              withAltGraphShift: null
            })
            break

          default:
            const key = KeyboardLayout.getCurrentKeymap()['KeyS']
            expect(key.unmodified).toBeDefined()
            expect(key.withShift).toBeDefined()
            expect(key.withAltGraph === null || typeof key.withAltGraph === 'string').toBe(true)
            expect(key.withAltGraphShift === null || typeof key.withAltGraphShift === 'string').toBe(true)
        }
      })
    })

    describe('.getCurrentKeyboardLayout()', () => {
      it('returns the current keyboard layout', () => {
        const layout = KeyboardLayout.getCurrentKeyboardLayout()
        expect(typeof layout).toBe('string')
        expect(layout.length).toBeGreaterThan(0)
      })
    })

    describe('.observeCurrentKeyboardLayout(callback)', () => {
      it('calls back immediately with the current keyboard layout', () => {
        const callback = jasmine.createSpy('observeCurrentKeyboardLayout')
        const disposable = KeyboardLayout.observeCurrentKeyboardLayout(callback)
        disposable.dispose()
        expect(callback).toHaveBeenCalledTimes(1)
        const layout = callback.calls.argsFor(0)[0]
        expect(typeof layout).toBe('string')
        expect(layout.length).toBeGreaterThan(0)
      })
    })

    describe('.getCurrentKeyboardLanguage()', () => {
      it('returns the current keyboard language', () => {
        const language = KeyboardLayout.getCurrentKeyboardLanguage()
        expect(typeof language).toBe('string')
        expect(language.length).toBeGreaterThan(0)
      })
    })

    describe('.getInstalledKeyboardLanguages()', () => {
      it('returns an array of string keyboard languages', () => {
        const languages = KeyboardLayout.getInstalledKeyboardLanguages()
        expect(Array.isArray(languages)).toBe(true)

        if (!(process.platform === 'win32' && process.env.CI)) {
          expect(languages.length).toBeGreaterThan(0)
        }

        return (() => {
          for (const language of languages) {
            expect(typeof language).toBe('string')
            expect(language.length).toBeGreaterThan(0)
          }
        })()
      })
    })
  }

  // Smoke tests on Linux
  if (process.platform === 'linux') {
    describe('.getCurrentKeymap()', function () {
      it('returns a keymap with unmodified and shift-modified keys without blowing up (basic smoke test)', function () {
        const keymap = KeyboardLayout.getCurrentKeymap()
        console.log('KEYMAP:', keymap);
        expect(keymap.KeyG.unmodified).toBeDefined()
        expect(keymap.KeyG.withShift).toBeDefined()
      })
    })

    describe('.getCurrentKeyboardLayout()', function () {
      it('returns an identifier for the current keyboard layout (basic smoke test)', function () {
        let layout = KeyboardLayout.getCurrentKeyboardLayout();
        console.warn('Linux keyboard layout:', layout);
        expect(layout).toBeDefined()
      })
    })
  }
})
