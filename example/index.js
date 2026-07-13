// Sample usage of `keyboard-layout`. The next line imports the library at a
// relative path so that it will work in place, but in regular usage you'd
// import at `@lumine-code/keyboard-layout` instead.
const KeyboardLayout = require('../lib/keyboard-layout');

console.log('Current layout:', KeyboardLayout.getCurrentKeyboardLayout());

KeyboardLayout.onDidChangeCurrentKeyboardLayout((current) => {
  console.log('Layout changed to', current);
});

// The presence of the listener we registered above will not prevent the
// process from exiting. So for this example script we'll create a `setTimeout`
// to delay the exiting of the process for a while.
setTimeout(() => {
  console.log('Done waiting; process exiting');
}, 60000)
